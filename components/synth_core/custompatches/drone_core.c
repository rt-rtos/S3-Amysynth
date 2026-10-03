/* drone_core.c - standalone "stutter house drone" synth.
 * Design notes: drone_core.h.
 *
 * Race safety: every AMY interaction goes through the amy_helpers ingress seam
 * (amy_helpers.h), never direct synth[] access - the render path walks synth[]
 * under the queue lock, so all config/notes are queued deltas. All callers here
 * are FreeRTOS tasks, never ISRs. */

#include "custompatches/wavetable_bank.h"
#include "custompatches/drone_core.h"
#include "synth_ui.h"      /* seq_get_bpm() (live global BPM) */
#include "amy_fx.h"        /* synth_ui_fx_reassert() */
#include "sequencer_core.h"    /* sequencer_core_push_envelope */
#include "seq_clamp.h"
#include "chord_types.h"   /* chord_type_t, chord_type_name() */
#include "quantizer.h"     /* quantizer_chord_intervals() */
#include "amy.h"
#include "amy_helpers.h"
#include "voice_config.h"  /* voice_build_wave + shared voice param helpers */
#include "sdkconfig.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "drone_core";

/* ── Synth slots: DRONE_SYNTH_MAIN/SUB from synth_slots.h's static pool ── */
#include "synth_slots.h"
#define DRONE_OSCS_PER_VC  2     /* osc0 = carrier, osc1 = stutter LFO */
#define DRONE_MAIN_VOICES  DRONE_CHORD_MAX_NOTES  /* one voice per chord note */
#define DRONE_SUB_VOICES   1     /* sub is always a single tone (root)        */

#define DRONE_GATE_VEL     1.0f

#define DRONE_ROOT_MIN     24    /* C1 — lowest selectable drone root */
#define DRONE_ROOT_MAX     72    /* C5 — highest selectable drone root */
#define DRONE_ROOT_DEFAULT 45    /* A2 — matches the former Am7 voicing */

/* Limits */
#define DRONE_RES_MIN      0.1f
#define DRONE_RES_MAX      3.0f    /* AMY imposes no upper Q cap (floor 0.51); our
                                    * ceiling keeps headroom before self-oscillation
                                    * transients spike the clip LUT. */
#define DRONE_SWEEP_MIN    100.0f
#define DRONE_SWEEP_MAX    8000.0f
#define DRONE_BARS_MIN     1
#define DRONE_BARS_MAX     16
#define DRONE_PATCH_MIN    0
/* Drone PATCH-mode ceiling: the raw waves SINE..TRIANGLE, plus the vendored
 * wavetable banks (AMY_WAVETABLE only); ranges in sequencer_core.h. NOISE, KS
 * and the bass presets are excluded - their excitation/multi-osc model
 * misbehaves with the drone's one-shot trigger style; drone_set_patch() snaps
 * values in that gap down to TRIANGLE. */
#if CONFIG_AMY_WAVETABLE
#define DRONE_PATCH_MAX    SEQ_PATCH_WAVETABLE_MAX
#else
#define DRONE_PATCH_MAX    SEQ_PATCH_TRIANGLE
#endif
#define DRONE_GATE_MIN     0.05f   /* osc1 PULSE duty: tighter chop as it shrinks */
#define DRONE_GATE_MAX     0.95f
#define DRONE_SWING_MAX    66      /* percent of one subdivision, applied to odd steps */
#define DRONE_BLIP_MAX     1.0f    /* per-step downward filter-zap depth (0 = off)     */
#define DRONE_PAT_STEPS    8       /* steps per bar in a pattern mask                  */

/* Chord intervals come from the shared quantizer_chord_intervals(chord_type_t)
 * table (quantizer.h). */

/* Stutter rate -> multiplier on the beat rate (beats/sec * mult = LFO Hz);
 * triplets are 1.5x their straight division. All map to integer subs-per-bar
 * (4x mult) so the stutter grid stays tick-exact. Deliberately NOT
 * frequency-capped like the LFO rates: audio-adjacent stutter gating is a
 * playable effect here. */
static const float s_rate_mult[DRONE_RATE_COUNT] = {
    [DRONE_RATE_1_4]   = 1.0f,
    [DRONE_RATE_1_8]   = 2.0f,
    [DRONE_RATE_1_16]  = 4.0f,
    [DRONE_RATE_1_32]  = 8.0f,
    [DRONE_RATE_1_1]   = 0.25f,
    [DRONE_RATE_1_4T]  = 1.5f,
    [DRONE_RATE_1_8T]  = 3.0f,
    [DRONE_RATE_1_16T] = 6.0f,
    [DRONE_RATE_1_32T] = 12.0f,
};

static const char *s_rate_names[DRONE_RATE_COUNT] = {
    [DRONE_RATE_1_4]   = "1/4",
    [DRONE_RATE_1_8]   = "1/8",
    [DRONE_RATE_1_16]  = "1/16",
    [DRONE_RATE_1_32]  = "1/32",
    [DRONE_RATE_1_1]   = "1/1",
    [DRONE_RATE_1_4T]  = "1/4T",
    [DRONE_RATE_1_8T]  = "1/8T",
    [DRONE_RATE_1_16T] = "1/16T",
    [DRONE_RATE_1_32T] = "1/32T",
};

/* ── Step patterns ── 8-bit per-bar masks (bit0 = step0, LSB-first). A 0 bit
 * means that stutter subdivision is skipped (filter closed). FULL = legacy. */
static const uint8_t s_pattern_mask[DRONE_PAT_COUNT] = {
    [DRONE_PAT_FULL]    = 0xFF,   /* 1 1 1 1 1 1 1 1 */
    [DRONE_PAT_FOUR]    = 0x55,   /* 1 0 1 0 1 0 1 0 */
    [DRONE_PAT_OFFBEAT] = 0xAA,   /* 0 1 0 1 0 1 0 1 */
    [DRONE_PAT_GALLOP]  = 0x5B,   /* 1 1 0 1 1 0 1 0 */
    [DRONE_PAT_DUB]     = 0x51,   /* 1 0 0 0 1 0 1 0 */
};

static const char *s_pattern_names[DRONE_PAT_COUNT] = {
    [DRONE_PAT_FULL]    = "FULL",
    [DRONE_PAT_FOUR]    = "FOUR",
    [DRONE_PAT_OFFBEAT] = "OFFBT",
    [DRONE_PAT_GALLOP]  = "GALOP",
    [DRONE_PAT_DUB]     = "DUB",
};

/* ── State ── */
typedef struct {
    bool           enabled;
    bool           solo_muted;  /* silenced because a sequencer track is soloed;
                                   separate from `enabled` so the user's own
                                   on/off survives a solo round-trip. Unrelated
                                   to amp_duck, which is the stutter depth. */
    bool           paused;      /* silenced by a stopped transport, likewise */
    drone_source_t source;
    uint16_t       wave;        /* AMY wave constant for the carrier */
    chord_type_t   chord;       /* chord preset (shared chord_type_t) */
    uint8_t        root_note;   /* drone-local root (DRONE_ROOT_MIN..MAX) */
    drone_follow_t follow;      /* chord progression follow mode */
    /* Notes this drone has note-on'd and not yet released, kept by
     * drone_reconcile() so every note-off names a note that is sounding. */
    uint8_t        sounding[DRONE_CHORD_MAX_NOTES];
    uint8_t        sounding_n;
    int16_t        sounding_sub;  /* -1 = none */
    uint32_t       last_sched_ms; /* latest amy_event.time sent for a note */
    float          resonance;
    float          amp_peak;    /* 0..1 on-beat level knob (linear: peak_lin = amp_peak) */
    float          amp_duck;    /* 0..1 duck depth knob  (duck_db = amp_duck * 40 dB)   */
    drone_rate_t   rate;
    uint16_t       patch;
    bool           sub_enabled;
    int8_t         sub_interval;/* semitones below the main          */
    float          sweep_lo;
    float          sweep_hi;
    uint8_t        sweep_bars;
    /* stutter-house controls */
    float          gate_len;    /* osc1 PULSE duty: 0.05..0.95 (chop length)  */
    uint8_t        swing_pct;   /* 0..66 swing on the filter/pattern grid     */
    float          blip_depth;  /* 0..1 per-step downward filter-zap depth    */
    drone_pattern_t pattern;    /* per-bar step on/off mask                   */
    float          last_blip_cutoff; /* to avoid redundant cutoff re-sends    */
    /* sweep phase, advanced each service tick (0..2pi) */
    float          last_lfo_hz; /* to avoid redundant LFO re-sends   */
    voice_params_t vp;          /* shared voice params. Drone uses env, env1
                                   (EG1 timing storage), their authored flags,
                                   and amp_trim (folded into amp_peak by
                                   s_amp_peak_lin()). vp.filter/vp.lfo unused:
                                   the sweep filter and stutter gate are the
                                   drone's own concepts. */
} drone_state_t;

static drone_state_t s_d;

/* Setters only record and raise a flag; drone_core_service() drains once per
 * UI frame (the arp's s_arp_dirty discipline, arp_core.c): s_d_rebuild
 * reconfigures the synths and re-voices, s_d_dirty only re-voices
 * (drone_reconcile). Setters run on Core-0 input tasks, the drain on the
 * Core-0 synth_ui_task - single-core, one-way flags, so volatile is for
 * compiler ordering only. Notes go out from synth_ui_task alone, so the
 * sounding-set bookkeeping has a single writer. */
static volatile bool s_d_dirty = false;
static volatile bool s_d_rebuild = false;
static inline void drone_mark_dirty(void) { s_d_dirty = true; }
static inline void drone_mark_rebuild(void) { s_d_rebuild = true; }

/* ── Peak/Duck dB amp helpers ──────────────────────────────────────────────
 * SINGLE source of truth for the engine math: drone_configure_wave_synth() and
 * drone_get_amp_levels_norm() both call these - never inline the formula.
 *
 * Knobs:  peak_lin = amp_peak; duck_db = amp_duck * 40 (0..-40 dB floor).
 * Coefs:  m = duck_db/120; const_sent = peak_lin * 10^(-duck_db/40), solved so
 *         the dB combine (voice_config.h), amp = const_sent * 10^(3*m*LFO),
 *         gives peak_lin on-beat (LFO=+1) and peak_lin * 10^(-duck_db/20)
 *         off-beat (LFO=-1).
 */
static inline float s_amp_peak_lin(void)
{
    /* Fold the graph editor's per-target trim (0..1) into amp_peak here so
     * every caller picks up the trimmed level. */
    float v = s_d.amp_peak * s_d.vp.amp_trim;
    v = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
    return v;
}

static inline float s_amp_duck_db(void)
{
    return s_d.amp_duck * 40.0f;
}

static inline float s_amp_m(void)
{
    return s_amp_duck_db() / 120.0f;
}

static inline float s_amp_const_sent(void)
{
    return s_amp_peak_lin() * powf(10.0f, -s_amp_duck_db() / 40.0f);
}

/* ── Tempo helpers ── */

/* Beats per second from the live global BPM. */
static inline float drone_bps(void)
{
    uint16_t bpm = seq_get_bpm();
    if (bpm < 1) bpm = 1;
    return (float)bpm / 60.0f;
}

/* Stutter LFO frequency for the current rate + tempo. */
static inline float drone_lfo_hz(void)
{
    return drone_bps() * s_rate_mult[s_d.rate];
}

/* Notes in a chord formula (up to the first -1 sentinel); 0 if out of range. */
static uint8_t drone_chord_note_count(chord_type_t chord)
{
    const int8_t *row = quantizer_chord_intervals(chord);
    if (!row) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < DRONE_CHORD_MAX_NOTES; i++) {
        if (row[i] < 0) break;
        n++;
    }
    return n;
}

/* ── Synth configuration (WAVE mode) ──
 * Build a `voices`-voice synth, each voice = osc0 carrier (NOTE-following)
 * amplitude-gated by an osc1 PULSE LFO, so a multi-voice main synth sounds a
 * chord when fed multiple notes.
 * `wave`: carrier waveform (s_d.wave, or patch-derived in PATCH mode).
 * `wt_preset`: wavetable bank index (pcm_wavetable_base..+samples-1) when
 * wave==WAVETABLE; -1 elsewhere (no-op). */
static void drone_configure_wave_synth(uint8_t synth, uint8_t voices, uint16_t wave,
                                       int16_t wt_preset)
{
    float lfo_hz    = drone_lfo_hz();
    /* AMY 1.2.12 (#720) dB/exp amp model: amp = const_sent * 10^(3*m*LFO).
     * Peak/Duck mapping: see the s_amp_* helpers. */
    float m          = s_amp_m();
    float const_sent = s_amp_const_sent();

    /* Shared skeleton: N voices, 2 oscs each, no patch; osc0 NOTE-following so
     * each voice plays its own chord note. Velocity does not scale amp
     * (osc0_amp_vel=0); EG0 multiplies the whole amp, so the ADSR shapes the
     * drone swell/fade *around* the LFO stutter. */
    voice_wave_cfg_t cfg = {
        .synth                = synth,
        .num_voices           = voices,
        .oscs_per_voice       = DRONE_OSCS_PER_VC,
        .wave                 = wave,
        .osc0_amp_const       = const_sent,
        .osc0_amp_vel         = 0.0f,
        .ks_feedback_authored = false,   /* fixed 0.9 KS default */
        .ks_feedback          = 0.0f,
        .ks_duty_ofs          = 0.0f,
        .wt_preset            = wt_preset,
    };
    voice_build_wave(&cfg);

    /* osc1 = PULSE LFO, absolute Hz, full const amp. The PULSE duty IS the
     * gate length: 0.5 = square, lower = tighter percussive chop. */
    amy_event *e = amy_helpers_event_begin();
    e->synth                 = synth;
    e->osc                   = 1;
    e->wave                  = PULSE;
    e->duty_coefs[COEF_CONST]= s_d.gate_len;
    e->freq_coefs[COEF_CONST]= lfo_hz;
    e->freq_coefs[COEF_NOTE] = 0.0f;     /* absolute Hz, ignore note */
    e->amp_coefs[COEF_CONST] = 1.0f;
    e->amp_coefs[COEF_VEL]   = 0.0f;     /* turn off the default vel/eg amp */
    e->amp_coefs[COEF_EG0]   = 0.0f;
    amy_helpers_event_send(e);

    /* osc0: amp = const + mod(=osc1) stutter gate, plus the LPF24 sweep filter.
     * Sent after osc1 so mod_source always points at a configured oscillator. */
    e = amy_helpers_event_begin();
    e->synth                  = synth;
    e->osc                    = 0;
    e->amp_coefs[COEF_MOD]    = m;
    e->mod_source[0]          = 1;       /* osc1 of this voice (base-osc rel) */
    e->filter_type            = FILTER_LPF24;
    e->filter_freq_coefs[COEF_CONST] = s_d.sweep_hi;
    e->resonance              = s_d.resonance;
    amy_helpers_event_send(e);
}

/* ── Synth configuration (PATCH mode) ──
 * Load an AMY patch onto the carrier synth; apply filter + resonance on top. */
static void drone_configure_patch_synth(uint8_t synth, uint8_t voices)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth        = synth;
    e->num_voices   = voices;
    e->patch_number = s_d.patch;
    amy_helpers_event_send(e);

    /* Patch strings carry global EQ/chorus commands; keep them per-synth. */
    synth_ui_fx_reassert(synth);

    /* Apply filter sweep + resonance on osc0 of the patch (best-effort). */
    e = amy_helpers_event_begin();
    e->synth                  = synth;
    e->osc                    = 0;
    e->filter_type            = FILTER_LPF24;
    e->filter_freq_coefs[COEF_CONST] = s_d.sweep_hi;
    e->resonance              = s_d.resonance;
    amy_helpers_event_send(e);
}

/* (Re)build both carrier synths for the current source/params. Main always
 * holds DRONE_CHORD_MAX_NOTES voices, so a chord change (own or followed)
 * re-voices without a rebuild; the sub is always a single voice. */
static void drone_rebuild(void)
{
    const uint8_t chord_n = DRONE_MAIN_VOICES;

    if (s_d.source == DRONE_SRC_WAVE) {
        drone_configure_wave_synth(DRONE_SYNTH_MAIN, chord_n, s_d.wave, -1);
        if (s_d.sub_enabled) {
            drone_configure_wave_synth(DRONE_SYNTH_SUB, DRONE_SUB_VOICES, s_d.wave, -1);
        }
    } else if (s_d.source == DRONE_SRC_PATCH && s_d.patch >= SEQ_PATCH_WAVE_BASE) {
        /* Virtual wave patches (257-261, plus 267-271 with AMY_WAVETABLE) are
         * not real AMY patches - sending them as patch numbers is silent. Map
         * to a wave synth here, as sequencer_configure_synth() does. */
        static const uint16_t s_wave_for_patch[] = {
            SINE, SAW_DOWN, SAW_UP, PULSE, TRIANGLE,
        };
        uint16_t wave = SAW_DOWN;
        int16_t  wt_preset = -1;
#if CONFIG_AMY_WAVETABLE
        if (sequencer_core_is_wavetable_patch(s_d.patch)) {
            wave = WAVETABLE;
            wt_preset = (int16_t)wavetable_bank_preset_for_patch(s_d.patch);
        } else
#endif
        {
            uint16_t widx = s_d.patch - SEQ_PATCH_WAVE_BASE;
            wave = (widx < 5) ? s_wave_for_patch[widx] : SAW_DOWN;
        }
        drone_configure_wave_synth(DRONE_SYNTH_MAIN, chord_n, wave, wt_preset);
        if (s_d.sub_enabled) {
            drone_configure_wave_synth(DRONE_SYNTH_SUB, DRONE_SUB_VOICES, wave, wt_preset);
        }
    } else {
        drone_configure_patch_synth(DRONE_SYNTH_MAIN, chord_n);
        if (s_d.sub_enabled) {
            drone_configure_patch_synth(DRONE_SYNTH_SUB, DRONE_SUB_VOICES);
        }
    }
    /* Re-impose the authored ADSR: rebuild/patch resets the synth oscs.
     * Deferred authority, matching melodic + arp. */
    if (s_d.vp.env_authored) {
        sequencer_core_push_envelope(DRONE_SYNTH_MAIN, &s_d.vp.env);
        if (s_d.sub_enabled) {
            sequencer_core_push_envelope(DRONE_SYNTH_SUB, &s_d.vp.env);
        }
    }
    /* EG1: nothing in the drone's own setup routes a coef to COEF_EG1, so this
     * is a no-op unless a loaded PATCH-mode instrument already does. */
    if (s_d.vp.env1_authored) {
        sequencer_core_push_envelope_eg1(DRONE_SYNTH_MAIN, 0, &s_d.vp.env1);
        if (s_d.sub_enabled) {
            sequencer_core_push_envelope_eg1(DRONE_SYNTH_SUB, 0, &s_d.vp.env1);
        }
    }
    s_d.last_lfo_hz = drone_lfo_hz();
}

/* Fire one note-on/off on a synth (the instrument allocator picks a voice) at
 * land_ms, or at the latest time already sent if that is later: a follow change
 * scheduled for the bar line must not be overtaken by a later edit's notes, and
 * AMY keeps equal-time events in send order. */
static void drone_note(uint8_t synth, bool on, float midi_note, uint32_t land_ms)
{
    uint32_t t = AMY_TIME_GEQ(land_ms, s_d.last_sched_ms) ? land_ms : s_d.last_sched_ms;
    amy_event *e = amy_helpers_event_begin();
    e->synth     = synth;
    e->midi_note = midi_note;
    e->velocity  = on ? DRONE_GATE_VEL : 0.0f;
    e->time      = t;
    amy_helpers_event_send(e);
    s_d.last_sched_ms = t;
}

/* Whether the drone should be making sound right now: the user's switch AND not
 * silenced by a solo elsewhere or a stopped transport. */
static inline bool drone_sounding(void)
{
    return s_d.enabled && !s_d.solo_muted && !s_d.paused;
}

/* Root and chord the drone voices now: its own, or under follow the applied
 * progression chord's (drone_follow_t). */
static void drone_effective(uint8_t *root, chord_type_t *chord)
{
    *root  = s_d.root_note;
    *chord = s_d.chord;
    if (s_d.follow == DRONE_FOLLOW_OFF) return;
    uint8_t pc;
    chord_type_t type;
    if (!sequencer_core_progression_applied_chord(&pc, &type, NULL)) return;
    int d   = (((int)pc - (int)(s_d.root_note % 12)) % 12 + 12 + 6) % 12 - 6;
    int eff = (int)s_d.root_note + d;
    if (eff < DRONE_ROOT_MIN) eff += 12;
    if (eff > DRONE_ROOT_MAX) eff -= 12;
    *root = (uint8_t)eff;
    if (s_d.follow == DRONE_FOLLOW_CHORD) *chord = type;
}

static bool drone_note_in(const uint8_t *set, uint8_t n, uint8_t note)
{
    for (uint8_t i = 0; i < n; i++) {
        if (set[i] == note) return true;
    }
    return false;
}

/* The single note emitter: bring the sounding set to the target voicing - one
 * note per chord note on main (the root alone for an empty chord), one at
 * root+sub_interval on the sub, nothing while not sounding. Notes in both sets
 * are held, not retriggered; releases go out before attacks. */
static void drone_reconcile(uint32_t land_ms)
{
    uint8_t tgt[DRONE_CHORD_MAX_NOTES];
    uint8_t tn = 0;
    int16_t tsub = -1;
    if (drone_sounding()) {
        uint8_t root;
        chord_type_t chord;
        drone_effective(&root, &chord);
        const int8_t *formula = quantizer_chord_intervals(chord);
        uint8_t chord_n = drone_chord_note_count(chord);
        if (chord_n < 1) {
            tgt[tn++] = root;
        } else {
            for (uint8_t i = 0; i < chord_n; i++) {
                if (!formula || formula[i] < 0) break;
                tgt[tn++] = (uint8_t)SEQ_CLAMP_INT((int)root + (int)formula[i], 0, 127);
            }
        }
        if (s_d.sub_enabled) {
            tsub = (int16_t)SEQ_CLAMP_INT((int)root + (int)s_d.sub_interval, 12, 108);
        }
    }

    for (uint8_t i = 0; i < s_d.sounding_n; i++) {
        if (!drone_note_in(tgt, tn, s_d.sounding[i])) {
            drone_note(DRONE_SYNTH_MAIN, false, (float)s_d.sounding[i], land_ms);
        }
    }
    if (s_d.sounding_sub >= 0 && s_d.sounding_sub != tsub) {
        drone_note(DRONE_SYNTH_SUB, false, (float)s_d.sounding_sub, land_ms);
    }
    for (uint8_t i = 0; i < tn; i++) {
        if (!drone_note_in(s_d.sounding, s_d.sounding_n, tgt[i])) {
            drone_note(DRONE_SYNTH_MAIN, true, (float)tgt[i], land_ms);
        }
    }
    if (tsub >= 0 && s_d.sounding_sub != tsub) {
        drone_note(DRONE_SYNTH_SUB, true, (float)tsub, land_ms);
    }

    memcpy(s_d.sounding, tgt, tn);
    s_d.sounding_n   = tn;
    s_d.sounding_sub = tsub;
}

/* Note-off the whole sounding set and forget it (ahead of a rebuild). */
static void drone_release_all(uint32_t land_ms)
{
    for (uint8_t i = 0; i < s_d.sounding_n; i++) {
        drone_note(DRONE_SYNTH_MAIN, false, (float)s_d.sounding[i], land_ms);
    }
    if (s_d.sounding_sub >= 0) {
        drone_note(DRONE_SYNTH_SUB, false, (float)s_d.sounding_sub, land_ms);
    }
    s_d.sounding_n   = 0;
    s_d.sounding_sub = -1;
}

/* amy_sysclock() ms at which a progression change due on land_tick sounds;
 * now when the tick is 0 or already reached. */
static uint32_t drone_land_ms(uint32_t land_tick)
{
    uint32_t now = amy_sysclock();
    if (land_tick == 0) return now;
    int32_t ahead = (int32_t)(land_tick - sequencer_ticks());
    if (ahead <= 0) return now;
    return now + (uint32_t)((uint64_t)ahead * amy_global.us_per_tick / 1000u);
}

/* Push the current filter cutoff to a synth's osc0. */
static void drone_push_cutoff(uint8_t synth, float cutoff)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth = synth;
    e->osc   = 0;
    e->filter_freq_coefs[COEF_CONST] = cutoff;
    amy_helpers_event_send(e);
}

/* ── Public API ── */

void drone_core_init(void)
{
    amy_helpers_init();

    memset(&s_d, 0, sizeof(s_d));
    voice_params_init_defaults(&s_d.vp);   /* unauthored, amp trim at unity */
    s_d.enabled      = false;
    s_d.source       = DRONE_SRC_WAVE;
    s_d.wave         = SAW_DOWN;
    s_d.chord        = CHORD_OFF;   /* root note only until a chord is chosen */
    s_d.root_note    = DRONE_ROOT_DEFAULT;
    s_d.follow       = DRONE_FOLLOW_OFF;
    s_d.sounding_sub = -1;
    s_d.resonance    = 1.5f;
    s_d.amp_peak     = 0.5f;     /* on-beat level (linear; 0.5 = -6 dB)   */
    s_d.amp_duck     = 0.5f;     /* duck depth knob (0.5 -> 20 dB duck)   */
    s_d.rate         = DRONE_RATE_1_16;
    s_d.patch        = 25;
    s_d.sub_enabled  = true;
    s_d.sub_interval = -12;      /* one octave below */
    s_d.sweep_lo     = 600.0f;
    s_d.sweep_hi     = 2000.0f;
    s_d.sweep_bars   = 4;
    s_d.gate_len     = 0.5f;     /* 50/50 square = legacy gate            */
    s_d.swing_pct    = 0;        /* straight grid                        */
    s_d.blip_depth   = 0.0f;     /* filter-zap off by default            */
    s_d.pattern      = DRONE_PAT_FULL;
    s_d.last_blip_cutoff = -1.0f;
    /* Default ADSR: slow drone swell. Stays unauthored until the user commits
     * in the graph editor - an unauthored env is never pushed, so EG0 holds at
     * 1.0 and the raw wave plays at full sustain. */
    s_d.vp.env.attack_ms   = 200;
    s_d.vp.env.decay_ms    = 300;
    s_d.vp.env.sustain_pct = 100;
    s_d.vp.env.release_ms  = 600;
    s_d.vp.env.eg_type     = 0;   /* ENVELOPE_NORMAL */
    /* EG1 is unrouted by the drone's own WAVE patches - filter movement comes
     * from the sweep + BPM-synced service tick, and a second envelope-driven
     * cutoff shift would double-modulate it. Kept as timing storage for a
     * PATCH-mode drone whose AMY patch routes its own bp1. */
    s_d.vp.env1.attack_ms   = 15;
    s_d.vp.env1.decay_ms    = 400;
    s_d.vp.env1.sustain_pct = 25;
    s_d.vp.env1.release_ms  = 400;
    s_d.vp.env1.eg_type     = 0;   /* ENVELOPE_NORMAL */

    drone_rebuild();
    ESP_LOGI(TAG, "drone_core initialized (synths %u/%u)",
             DRONE_SYNTH_MAIN, DRONE_SYNTH_SUB);
}

void drone_core_service(void)
{
    /* Drain the coalesced rebuild/re-voice BEFORE the enabled gate: a param
     * changed while the drone is off must still rebuild so it is live on the
     * next enable, and a disable must release its notes. Before the LFO-hz
     * tracking so rebuild and tempo push order correctly. */
    if (s_d_rebuild) {
        s_d_rebuild = false;
        s_d_dirty = false;
        uint32_t now = amy_sysclock();
        drone_release_all(now);
        drone_rebuild();
        drone_reconcile(now);
    } else if (s_d_dirty) {
        s_d_dirty = false;
        drone_reconcile(amy_sysclock());
    }

    if (!s_d.enabled) return;

    /* Keep the LFO locked to tempo: re-send osc1 freq if the BPM changed the
     * stutter rate by more than a small epsilon. */
    float lfo_hz = drone_lfo_hz();
    if (fabsf(lfo_hz - s_d.last_lfo_hz) > 0.01f) {
        amy_event *e = amy_helpers_event_begin();
        e->synth                  = DRONE_SYNTH_MAIN;
        e->osc                    = 1;
        e->freq_coefs[COEF_CONST] = lfo_hz;
        e->freq_coefs[COEF_NOTE]  = 0.0f;
        amy_helpers_event_send(e);
        if (s_d.sub_enabled) {
            e = amy_helpers_event_begin();
            e->synth                  = DRONE_SYNTH_SUB;
            e->osc                    = 1;
            e->freq_coefs[COEF_CONST] = lfo_hz;
            e->freq_coefs[COEF_NOTE]  = 0.0f;
            amy_helpers_event_send(e);
        }
        s_d.last_lfo_hz = lfo_hz;
    }

    /* Everything below is a pure function of the global musical clock, NOT of
     * how often this service runs - sweep, stutter grid, swing, pattern and
     * blip stay frame-rate-independent and beat-locked across BPM changes. */
    const uint32_t bar_ticks = (uint32_t)(AMY_SEQUENCER_PPQ * 4);   /* 192 */
    /* AMY's monotonic 48 PPQ tick counter, advanced by the audio-rate
     * sequencer: the same clock the sequencer and arp ride. */
    uint32_t now = sequencer_ticks();

    /* (a) Slow bar-length sweep = the BASE cutoff. */
    uint32_t period_ticks = (uint32_t)s_d.sweep_bars * bar_ticks;
    if (period_ticks < 1) period_ticks = 1;
    uint32_t pos = now % period_ticks;
    float phase = (2.0f * (float)M_PI) * ((float)pos / (float)period_ticks);
    float mid  = 0.5f * (s_d.sweep_lo + s_d.sweep_hi);
    float half = 0.5f * (s_d.sweep_hi - s_d.sweep_lo);
    float base = mid + half * sinf(phase);

    /* (b) Stutter subdivision grid; the pattern mask is an 8-step per-bar grid,
     * so the step index wraps mod DRONE_PAT_STEPS. */
    float subs_per_bar = 4.0f * s_rate_mult[s_d.rate];
    uint32_t ticks_per_sub = (uint32_t)((float)bar_ticks / subs_per_bar + 0.5f);
    if (ticks_per_sub < 1) ticks_per_sub = 1;

    /* (c) Swing: push ODD subdivisions later by swing_pct% of one subdivision,
     * by offsetting the clock used for the sub-phase calc. */
    uint32_t pos_in_bar = now % bar_ticks;
    uint32_t sub_index_raw = pos_in_bar / ticks_per_sub;
    uint32_t swing_off = 0;
    if ((sub_index_raw & 1u) && s_d.swing_pct > 0) {
        swing_off = (uint32_t)((float)ticks_per_sub * (float)s_d.swing_pct / 100.0f);
    }
    /* effective position within the current subdivision (0..1), swing-shifted */
    uint32_t swung = (now + bar_ticks - swing_off) % bar_ticks; /* avoid underflow */
    uint32_t sub_index = (swung / ticks_per_sub) % DRONE_PAT_STEPS;
    float frac_in_sub = (float)(swung % ticks_per_sub) / (float)ticks_per_sub;

    /* (d) Pattern mask: a 0 bit closes the filter for that step (skips it). */
    bool step_on = (s_pattern_mask[s_d.pattern] >> sub_index) & 1u;

    /* (e) Per-step filter blip: a downward zap at the start of each open step,
     * decaying across the subdivision. AMY's logfreq downward slew-limit
     * smooths the attack edge into something musical. */
    float cutoff;
    if (!step_on) {
        cutoff = DRONE_SWEEP_MIN;            /* closed: skip this subdivision */
    } else if (s_d.blip_depth > 0.0001f) {
        float env = expf(-4.0f * frac_in_sub);          /* 1 -> ~0 across the step */
        cutoff = base * (1.0f - s_d.blip_depth * env);
        if (cutoff < DRONE_SWEEP_MIN) cutoff = DRONE_SWEEP_MIN;
    } else {
        cutoff = base;                       /* blip off = plain sweep (legacy) */
    }

    /* Skip near-identical pushes to keep the event queue light while idling on
     * a sustained step. */
    if (fabsf(cutoff - s_d.last_blip_cutoff) > 1.0f) {
        drone_push_cutoff(DRONE_SYNTH_MAIN, cutoff);
        if (s_d.sub_enabled) {
            drone_push_cutoff(DRONE_SYNTH_SUB, cutoff * 0.5f);  /* sub sits lower */
        }
        s_d.last_blip_cutoff = cutoff;
    }
}

void drone_core_follow_changed(void)
{
    if (s_d.follow == DRONE_FOLLOW_OFF) return;
    uint32_t land_tick = 0;
    (void)sequencer_core_progression_applied_chord(NULL, NULL, &land_tick);
    /* Synchronous: drone_core_service() already ran this frame. */
    drone_reconcile(drone_land_ms(land_tick));
}

void drone_set_enabled(bool on)
{
    if (s_d.enabled == on) return;
    s_d.enabled = on;
    if (on) {
        /* Rebuild so a fresh enable reflects the current params. Sweep phase
         * comes from the global tick clock (never reset here), keeping the
         * sweep phase-locked to the transport bar grid. */
        drone_mark_rebuild();
    } else {
        drone_mark_dirty();
    }
    ESP_LOGI(TAG, "drone %s", on ? "ON" : "OFF");
}

void drone_set_solo_muted(bool muted)
{
    if (s_d.solo_muted == muted) return;
    s_d.solo_muted = muted;
    if (!s_d.enabled) return;      /* nothing sounding either way */
    /* Coming back from a solo is a fresh start, exactly like a fresh enable:
     * rebuild so the voices reflect any params edited while silenced. */
    if (!muted) drone_mark_rebuild();
    else        drone_mark_dirty();
}

void drone_set_paused(bool paused)
{
    if (s_d.paused == paused) return;
    s_d.paused = paused;
    if (!s_d.enabled) return;
    if (!paused) drone_mark_rebuild();   /* as for the solo round-trip */
    else         drone_mark_dirty();
}

void drone_set_source(drone_source_t src)
{
    if (src != DRONE_SRC_WAVE && src != DRONE_SRC_PATCH) return;
    if (s_d.source == src) return;
    s_d.source = src;
    drone_mark_rebuild();
}

void drone_set_wave(uint16_t amy_wave)
{
    /* NOISE and KS are excluded (excitation model misbehaves with the drone's
     * one-shot trigger style); coerce any persisted/stale value. */
    if (amy_wave == NOISE || amy_wave == KS) amy_wave = SAW_DOWN;
    if (s_d.wave == amy_wave) return;
    s_d.wave = amy_wave;
    if (s_d.source == DRONE_SRC_WAVE) drone_mark_rebuild();
}

void drone_set_chord(chord_type_t chord)
{
    if (chord >= CHORD_TYPE_COUNT) return;
    if (s_d.chord == chord) return;
    s_d.chord = chord;
    drone_mark_dirty();
}

void drone_set_root_note(uint8_t note)
{
    uint8_t clamped = (uint8_t)SEQ_CLAMP_INT((int)note, DRONE_ROOT_MIN, DRONE_ROOT_MAX);
    if (s_d.root_note == clamped) return;
    s_d.root_note = clamped;
    drone_mark_dirty();
    ESP_LOGI(TAG, "drone root -> %u", (unsigned)clamped);
}

void drone_set_follow(drone_follow_t f)
{
    if (f >= DRONE_FOLLOW_COUNT) return;
    if (s_d.follow == f) return;
    s_d.follow = f;
    drone_mark_dirty();
}

void drone_set_resonance(float r)
{
    r = SEQ_CLAMP_F32(r, DRONE_RES_MIN, DRONE_RES_MAX);
    if (fabsf(s_d.resonance - r) < 0.001f) return;
    s_d.resonance = r;
    /* Resonance is an osc0 param; push without full rebuild. */
    amy_event *e = amy_helpers_event_begin();
    e->synth     = DRONE_SYNTH_MAIN;
    e->osc       = 0;
    e->resonance = r;
    amy_helpers_event_send(e);
    if (s_d.sub_enabled) {
        e = amy_helpers_event_begin();
        e->synth     = DRONE_SYNTH_SUB;
        e->osc       = 0;
        e->resonance = r;
        amy_helpers_event_send(e);
    }
}

void drone_set_amp_peak(float c)
{
    c = SEQ_CLAMP_F32(c, 0.0f, 1.0f);
    if (fabsf(s_d.amp_peak - c) < 0.001f) return;
    s_d.amp_peak = c;
    if (s_d.source == DRONE_SRC_WAVE) drone_mark_rebuild();
}

void drone_set_amp_duck(float m)
{
    /* Full range: duck_knob=1 -> -40 dB floor, not silence, so no cap needed. */
    m = SEQ_CLAMP_F32(m, 0.0f, 1.0f);
    if (fabsf(s_d.amp_duck - m) < 0.001f) return;
    s_d.amp_duck = m;
    if (s_d.source == DRONE_SRC_WAVE) drone_mark_rebuild();
}

void drone_set_rate(drone_rate_t rate)
{
    if (rate >= DRONE_RATE_COUNT) return;
    if (s_d.rate == rate) return;
    s_d.rate = rate;
    /* service() picks up the new LFO Hz on the next frame. Nudge immediately. */
    s_d.last_lfo_hz = -1.0f;
}

bool drone_patch_excluded(uint16_t patch)
{
    /* Kconfig-gated ranges are excluded here too, on top of the drone's own
     * rules. */
    if (sequencer_core_patch_compiled_out(patch)) return true;
#if CONFIG_AMY_WAVETABLE
    if (patch > DRONE_PATCH_MAX && !sequencer_core_is_wavetable_patch(patch)) return true;
#else
    if (patch > DRONE_PATCH_MAX) return true;   /* incl. bass/FM/additive w/o wavetable */
#endif
#if CONFIG_AMY_WAVETABLE
    /* NOISE/KS/bass (262-266) sit below the supported wavetable range, so
     * they need excluding individually. */
    if (patch > SEQ_PATCH_TRIANGLE && patch < SEQ_PATCH_WAVETABLE_BASE) {
        return true;
    }
#endif
    return false;
}

void drone_set_patch(uint16_t patch)
{
    /* Cycling never offers an excluded patch; this snap defends direct
     * programmatic callers. */
    if (drone_patch_excluded(patch)) patch = SEQ_PATCH_TRIANGLE;
    if (s_d.patch == patch) return;
    s_d.patch = patch;
    if (s_d.source == DRONE_SRC_PATCH) drone_mark_rebuild();
}

void drone_set_sub_enabled(bool on)
{
    if (s_d.sub_enabled == on) return;
    s_d.sub_enabled = on;
    /* On configures the sub synth; off only releases its note. */
    if (on) drone_mark_rebuild();
    else    drone_mark_dirty();
}

void drone_set_sub_interval(int8_t st)
{
    int v = SEQ_CLAMP_INT((int)st, -36, 0);
    if (s_d.sub_interval == (int8_t)v) return;
    s_d.sub_interval = (int8_t)v;
    if (s_d.sub_enabled) drone_mark_dirty();
}

void drone_set_sweep_lo(float hz)
{
    hz = SEQ_CLAMP_F32(hz, DRONE_SWEEP_MIN, DRONE_SWEEP_MAX);
    if (hz > s_d.sweep_hi) hz = s_d.sweep_hi;
    s_d.sweep_lo = hz;
}

void drone_set_sweep_hi(float hz)
{
    hz = SEQ_CLAMP_F32(hz, DRONE_SWEEP_MIN, DRONE_SWEEP_MAX);
    if (hz < s_d.sweep_lo) hz = s_d.sweep_lo;
    s_d.sweep_hi = hz;
}

void drone_set_sweep_bars(uint8_t bars)
{
    s_d.sweep_bars = SEQ_CLAMP_U8((int)bars, DRONE_BARS_MIN, DRONE_BARS_MAX);
}

void drone_set_gate_len(float frac)
{
    frac = SEQ_CLAMP_F32(frac, DRONE_GATE_MIN, DRONE_GATE_MAX);
    if (fabsf(s_d.gate_len - frac) < 0.001f) return;
    s_d.gate_len = frac;
    /* Gate length is osc1's PULSE duty; push it without a full rebuild. WAVE
     * mode only (PATCH carriers have no osc1 LFO). */
    if (s_d.source != DRONE_SRC_WAVE) return;
    amy_event *e = amy_helpers_event_begin();
    e->synth                  = DRONE_SYNTH_MAIN;
    e->osc                    = 1;
    e->duty_coefs[COEF_CONST] = frac;
    amy_helpers_event_send(e);
    if (s_d.sub_enabled) {
        e = amy_helpers_event_begin();
        e->synth                  = DRONE_SYNTH_SUB;
        e->osc                    = 1;
        e->duty_coefs[COEF_CONST] = frac;
        amy_helpers_event_send(e);
    }
}

void drone_set_swing(uint8_t pct)
{
    s_d.swing_pct = SEQ_CLAMP_U8((int)pct, 0, DRONE_SWING_MAX);
    /* Consumed by service(); force a cutoff re-push next frame. */
    s_d.last_blip_cutoff = -1.0f;
}

void drone_set_blip(float depth)
{
    depth = SEQ_CLAMP_F32(depth, 0.0f, DRONE_BLIP_MAX);
    s_d.blip_depth = depth;
    s_d.last_blip_cutoff = -1.0f;
}

void drone_set_pattern(drone_pattern_t p)
{
    if (p >= DRONE_PAT_COUNT) return;
    s_d.pattern = p;
    s_d.last_blip_cutoff = -1.0f;
}

void drone_get_envelope(seq_env_t *out)
{
    if (out) *out = s_d.vp.env;
}

void drone_set_envelope(const seq_env_t *env)
{
    if (!env) return;
    s_d.vp.env = *env;
    if (s_d.vp.env.attack_ms < 2) s_d.vp.env.attack_ms = 2;  /* 2 ms floor */
    if (s_d.vp.env.release_ms < 5) s_d.vp.env.release_ms = 5;  /* 5 ms declick floor */
    s_d.vp.env_authored = true;
    sequencer_core_push_envelope(DRONE_SYNTH_MAIN, &s_d.vp.env);
    if (s_d.sub_enabled) {
        sequencer_core_push_envelope(DRONE_SYNTH_SUB, &s_d.vp.env);
    }
    ESP_LOGI(TAG, "drone env -> A%u D%u S%u%% R%u",
             (unsigned)s_d.vp.env.attack_ms, (unsigned)s_d.vp.env.decay_ms,
             (unsigned)s_d.vp.env.sustain_pct, (unsigned)s_d.vp.env.release_ms);
}

/* ── Editor live-preview (AMY only; drone store + authored flags untouched) ── */

void drone_preview_envelope(const seq_env_t *env)
{
    if (!env) return;
    sequencer_core_push_envelope(DRONE_SYNTH_MAIN, env);
    if (s_d.sub_enabled) {
        sequencer_core_push_envelope(DRONE_SYNTH_SUB, env);
    }
}

void drone_preview_envelope2(const seq_env_t *env)
{
    if (!env) return;
    sequencer_core_push_envelope_eg1(DRONE_SYNTH_MAIN, 0, env);
    if (s_d.sub_enabled) {
        sequencer_core_push_envelope_eg1(DRONE_SYNTH_SUB, 0, env);
    }
}

void drone_get_envelope2(seq_env_t *out)
{
    if (out) *out = s_d.vp.env1;
}

void drone_set_envelope2(const seq_env_t *env)
{
    if (!env) return;
    s_d.vp.env1 = *env;
    if (s_d.vp.env1.attack_ms < 2) s_d.vp.env1.attack_ms = 2;  /* 2 ms floor */
    if (s_d.vp.env1.release_ms < 5) s_d.vp.env1.release_ms = 5;  /* 5 ms declick floor */
    s_d.vp.env1_authored = true;
    sequencer_core_push_envelope_eg1(DRONE_SYNTH_MAIN, 0, &s_d.vp.env1);
    if (s_d.sub_enabled) {
        sequencer_core_push_envelope_eg1(DRONE_SYNTH_SUB, 0, &s_d.vp.env1);
    }
    ESP_LOGI(TAG, "drone env1 -> A%u D%u S%u%% R%u",
             (unsigned)s_d.vp.env1.attack_ms, (unsigned)s_d.vp.env1.decay_ms,
             (unsigned)s_d.vp.env1.sustain_pct, (unsigned)s_d.vp.env1.release_ms);
}

void drone_get_voice_params(voice_params_t *out)
{
    if (out) *out = s_d.vp;
}

void drone_set_voice_params(const voice_params_t *vp)
{
    if (!vp) return;
    s_d.vp = *vp;
    if (s_d.vp.env.attack_ms < 2)   s_d.vp.env.attack_ms = 2;
    if (s_d.vp.env.release_ms < 5)  s_d.vp.env.release_ms = 5;
    if (s_d.vp.env1.attack_ms < 2)  s_d.vp.env1.attack_ms = 2;
    if (s_d.vp.env1.release_ms < 5) s_d.vp.env1.release_ms = 5;
    s_d.vp.amp_trim = SEQ_CLAMP_F32(s_d.vp.amp_trim, 0.0f, 1.0f);
    /* The rebuild resets every osc, then re-imposes only the authored
     * envelopes and re-reads s_amp_peak_lin(). */
    drone_mark_rebuild();
}

/* ── Getters ── */
bool           drone_get_enabled(void)      { return s_d.enabled; }
drone_source_t drone_get_source(void)       { return s_d.source; }
uint16_t       drone_get_wave(void)         { return s_d.wave; }
chord_type_t   drone_get_chord(void)        { return s_d.chord; }
uint8_t        drone_get_root_note(void)    { return s_d.root_note; }
drone_follow_t drone_get_follow(void)       { return s_d.follow; }
float          drone_get_resonance(void)    { return s_d.resonance; }
float          drone_get_amp_peak(void)     { return s_d.amp_peak; }
float          drone_get_amp_duck(void)     { return s_d.amp_duck; }
drone_rate_t   drone_get_rate(void)         { return s_d.rate; }
uint16_t       drone_get_patch(void)        { return s_d.patch; }
bool           drone_get_sub_enabled(void)  { return s_d.sub_enabled; }
int8_t         drone_get_sub_interval(void) { return s_d.sub_interval; }
float          drone_get_sweep_lo(void)     { return s_d.sweep_lo; }
float          drone_get_sweep_hi(void)     { return s_d.sweep_hi; }
uint8_t        drone_get_sweep_bars(void)   { return s_d.sweep_bars; }
float          drone_get_gate_len(void)     { return s_d.gate_len; }
uint8_t        drone_get_swing(void)        { return s_d.swing_pct; }
float          drone_get_blip(void)         { return s_d.blip_depth; }
drone_pattern_t drone_get_pattern(void)     { return s_d.pattern; }

const char *drone_rate_name(drone_rate_t rate)
{
    if (rate >= DRONE_RATE_COUNT) return "?";
    return s_rate_names[rate];
}

const char *drone_wave_name(uint16_t amy_wave)
{
    switch (amy_wave) {
        case SINE:     return "SINE";
        case PULSE:    return "PULSE";
        case SAW_DOWN: return "SAW";
        case SAW_UP:   return "SAWUP";
        case TRIANGLE: return "TRI";
        case NOISE:    return "NOISE";
        case KS:       return "KS";
        default:       return "?";
    }
}

const char *drone_chord_name(chord_type_t chord)
{
    return chord_type_name(chord);
}

void drone_get_amp_levels_norm(float *floor_norm, float *ceil_norm)
{
    /* ON/OFF-beat linear amplitudes from the SAME s_amp_* math as
     * drone_configure_wave_synth(). */
    float peak    = s_amp_peak_lin();
    float duck_db = s_amp_duck_db();
    if (ceil_norm)  *ceil_norm  = peak;
    if (floor_norm) *floor_norm = peak * powf(10.0f, -duck_db / 20.0f);
}

const char *drone_pattern_name(drone_pattern_t p)
{
    if (p >= DRONE_PAT_COUNT) return "?";
    return s_pattern_names[p];
}

const char *drone_follow_name(drone_follow_t f)
{
    static const char *const s_follow_names[DRONE_FOLLOW_COUNT] = {
        [DRONE_FOLLOW_OFF]   = "OFF",
        [DRONE_FOLLOW_ROOT]  = "ROOT",
        [DRONE_FOLLOW_CHORD] = "CHORD",
    };
    if (f >= DRONE_FOLLOW_COUNT) return "?";
    return s_follow_names[f];
}

/* ── Per-target amplitude trim (graph editor amp mode) ──────────────────── */

void drone_set_amp_trim(float v)
{
    v = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
    if (fabsf(s_d.vp.amp_trim - v) < 0.001f) return;
    s_d.vp.amp_trim = v;
    /* The coalesced rebuild re-reads s_amp_peak_lin(), picking up the new
     * effective peak. */
    if (s_d.source == DRONE_SRC_WAVE) drone_mark_rebuild();
}

float drone_get_amp_trim(void) { return s_d.vp.amp_trim; }
