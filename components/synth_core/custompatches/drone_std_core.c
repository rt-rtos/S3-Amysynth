/* drone_std_core.c - normal (free-running) drone synth.
 *
 * The stutter drone's sibling: chord + mono sub voicing on its own AMY slots,
 * sustained output, and the standard per-voice toolset (free filter, native
 * LFO, graph-editor envelopes) instead of the stutter machinery. Design split:
 * drone_std_core.h; shared concurrency rationale: drone_core.c.
 *
 * AMY access rules as in drone_core.c; callers are FreeRTOS tasks, never
 * ISRs. */

#include "custompatches/wavetable_bank.h"
#include "custompatches/drone_std_core.h"
#include "synth_ui.h"          /* seq_get_bpm() */
#include "amy_fx.h"            /* synth_ui_fx_reassert() */
#include "sequencer_core.h"    /* sequencer_core_push_envelope/filter */
#include "seq_clamp.h"
#include "quantizer.h"         /* quantizer_chord_intervals() */
#include "amy.h"
#include "amy_helpers.h"
#include "voice_config.h"
#include "sdkconfig.h"
#include "esp_log.h"

#include <math.h>
#include <string.h>

static const char *TAG = "drone_std";

/* ── Synth slots: DRONE_STD_SYNTH_MAIN/SUB from synth_slots.h's static pool ── */
#include "synth_slots.h"
#define DRONE_STD_OSCS_PER_VC 3     /* osc0 = carrier, osc1 = native LFO carrier,
                                       osc2 = wobble modulator (chained mod)   */
#define DRONE_STD_SUB_VOICES  1

#define DRONE_STD_GATE_VEL    1.0f

/* ── State ── */
typedef struct {
    bool           enabled;
    bool           solo_muted;   /* silenced because a sequencer track is soloed;
                                    separate from `enabled` so the user's own
                                    on/off survives a solo round-trip        */
    bool           paused;       /* silenced by a stopped transport, likewise */
    drone_source_t source;
    uint16_t       wave;         /* AMY wave constant for the carrier      */
    chord_type_t   chord;
    uint8_t        root_note;    /* DRONE_ROOT range shared with stutter   */
    drone_follow_t follow;       /* chord progression follow mode          */
    /* Sounding-set bookkeeping, as in drone_core.c. */
    uint8_t        sounding[DRONE_CHORD_MAX_NOTES];
    uint8_t        sounding_n;
    int16_t        sounding_sub;  /* -1 = none */
    uint32_t       last_sched_ms; /* latest amy_event.time sent for a note */
    float          level;       /* 0..1 linear output level (amp CONST)   */
    uint16_t       patch;
    bool           sub_enabled;
    int8_t         sub_interval; /* semitones below the root               */
    float          last_lfo_bpm; /* to avoid redundant LFO re-applies      */
    voice_params_t vp;           /* env/env1 (graph editor), filter (full
                                    filter editor), lfo (LFO editor), each
                                    with its authored flag, plus amp_trim. */
} drone_std_state_t;

static drone_std_state_t s_ds;

/* Coalesced rebuild/re-voice flags, drained once per frame by
 * drone_std_core_service() (drone_core.c s_d_dirty/s_d_rebuild). */
static volatile bool s_ds_dirty = false;
static volatile bool s_ds_rebuild = false;
static inline void drone_std_mark_dirty(void) { s_ds_dirty = true; }
static inline void drone_std_mark_rebuild(void) { s_ds_rebuild = true; }

#define DRONE_STD_ROOT_MIN 24
#define DRONE_STD_ROOT_MAX 72

static inline float drone_std_level_lin(void)
{
    float v = s_ds.level * s_ds.vp.amp_trim;
    return SEQ_CLAMP_F32(v, 0.0f, 1.0f);
}

static inline bool drone_std_lfo_on(void)
{
    return s_ds.vp.lfo_authored && s_ds.vp.lfo.enabled;
}

static uint8_t drone_std_chord_note_count(chord_type_t chord)
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

/* Push the stored filter to a synth's osc0 (bypass when disabled). */
static void drone_std_push_filter(uint8_t synth, const seq_filter_t *f)
{
    sequencer_core_push_filter(synth, f, false);
}

/* ── Synth configuration (WAVE mode) ──
 * Shared skeleton + free filter + native LFO. No amp MOD coupling: under the
 * dB combine model a plain CONST level is exactly level_lin (EG0 = 1). */
static void drone_std_configure_wave_synth(uint8_t synth, uint8_t voices,
                                           uint16_t wave, int16_t wt_preset)
{
    voice_wave_cfg_t cfg = {
        .synth                = synth,
        .num_voices           = voices,
        .oscs_per_voice       = DRONE_STD_OSCS_PER_VC,
        .wave                 = wave,
        .osc0_amp_const       = drone_std_level_lin(),
        .osc0_amp_vel         = 0.0f,
        .ks_feedback_authored = false,
        .ks_feedback          = 0.0f,
        .ks_duty_ofs          = 0.0f,
        .wt_preset            = wt_preset,
    };
    voice_build_wave(&cfg);

    drone_std_push_filter(synth, &s_ds.vp.filter);

    /* ALWAYS applied (NULL clears): re-sending the same voice count does not
     * reset the osc pool, so a stale COEF_MOD from a prior LFO target would
     * keep modulating - the stale-AMP case rails (voice_config.h). */
    voice_apply_native_lfo(synth, drone_std_lfo_on() ? &s_ds.vp.lfo : NULL,
                           seq_get_bpm());
}

/* ── Synth configuration (PATCH mode) ──
 * Real AMY patch: it owns its osc topology, so no native LFO is layered on. */
static void drone_std_configure_patch_synth(uint8_t synth, uint8_t voices)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth        = synth;
    e->num_voices   = voices;
    e->patch_number = s_ds.patch;
    amy_helpers_event_send(e);

    /* Patch strings carry global EQ/chorus commands; keep them per-synth. */
    synth_ui_fx_reassert(synth);

    drone_std_push_filter(synth, &s_ds.vp.filter);
}

/* (Re)build both synths for the current source/params. Main always holds
 * DRONE_CHORD_MAX_NOTES voices, so a chord change re-voices without a
 * rebuild; the sub is always a single voice. */
static void drone_std_rebuild(void)
{
    const uint8_t chord_n = DRONE_CHORD_MAX_NOTES;

    if (s_ds.source == DRONE_SRC_WAVE) {
        drone_std_configure_wave_synth(DRONE_STD_SYNTH_MAIN, chord_n, s_ds.wave, -1);
        if (s_ds.sub_enabled) {
            drone_std_configure_wave_synth(DRONE_STD_SYNTH_SUB, DRONE_STD_SUB_VOICES,
                                           s_ds.wave, -1);
        }
    } else if (s_ds.source == DRONE_SRC_PATCH && s_ds.patch >= SEQ_PATCH_WAVE_BASE) {
        /* Virtual wave/wavetable patches are not real AMY patches: intercept
         * and build as a wave synth, same mapping as stutter drone/melodic. */
        static const uint16_t s_wave_for_patch[] = {
            SINE, SAW_DOWN, SAW_UP, PULSE, TRIANGLE,
        };
        uint16_t wave = SAW_DOWN;
        int16_t  wt_preset = -1;
#if CONFIG_AMY_WAVETABLE
        if (sequencer_core_is_wavetable_patch(s_ds.patch)) {
            wave = WAVETABLE;
            wt_preset = (int16_t)wavetable_bank_preset_for_patch(s_ds.patch);
        } else
#endif
        {
            uint16_t widx = s_ds.patch - SEQ_PATCH_WAVE_BASE;
            wave = (widx < 5) ? s_wave_for_patch[widx] : SAW_DOWN;
        }
        drone_std_configure_wave_synth(DRONE_STD_SYNTH_MAIN, chord_n, wave, wt_preset);
        if (s_ds.sub_enabled) {
            drone_std_configure_wave_synth(DRONE_STD_SYNTH_SUB, DRONE_STD_SUB_VOICES,
                                           wave, wt_preset);
        }
    } else {
        drone_std_configure_patch_synth(DRONE_STD_SYNTH_MAIN, chord_n);
        if (s_ds.sub_enabled) {
            drone_std_configure_patch_synth(DRONE_STD_SYNTH_SUB, DRONE_STD_SUB_VOICES);
        }
    }

    /* Per-osc distortion does not survive a rebuild either. */
    drone_std_reapply_dist();

    /* Deferred authority: re-impose the user's envelopes after any rebuild. */
    if (s_ds.vp.env_authored) {
        sequencer_core_push_envelope(DRONE_STD_SYNTH_MAIN, &s_ds.vp.env);
        if (s_ds.sub_enabled) {
            sequencer_core_push_envelope(DRONE_STD_SYNTH_SUB, &s_ds.vp.env);
        }
    }
    if (s_ds.vp.env1_authored) {
        sequencer_core_push_envelope_eg1(DRONE_STD_SYNTH_MAIN, 0, &s_ds.vp.env1);
        if (s_ds.sub_enabled) {
            sequencer_core_push_envelope_eg1(DRONE_STD_SYNTH_SUB, 0, &s_ds.vp.env1);
        }
    }
    s_ds.last_lfo_bpm = (float)seq_get_bpm();
}

/* Fire one note-on/off on a synth (the instrument allocator picks a voice) at
 * land_ms, or at the latest time already sent if later (drone_core.c
 * drone_note). */
static void drone_std_note(uint8_t synth, bool on, float midi_note, uint32_t land_ms)
{
    uint32_t t = AMY_TIME_GEQ(land_ms, s_ds.last_sched_ms) ? land_ms : s_ds.last_sched_ms;
    amy_event *e = amy_helpers_event_begin();
    e->synth     = synth;
    e->midi_note = midi_note;
    e->velocity  = on ? DRONE_STD_GATE_VEL : 0.0f;
    e->time      = t;
    amy_helpers_event_send(e);
    s_ds.last_sched_ms = t;
}

/* Whether the drone should be making sound right now: the user's switch AND not
 * silenced by a solo elsewhere or a stopped transport. */
static inline bool drone_std_sounding(void)
{
    return s_ds.enabled && !s_ds.solo_muted && !s_ds.paused;
}

/* Root and chord the drone voices now (drone_follow_t). */
static void drone_std_effective(uint8_t *root, chord_type_t *chord)
{
    *root  = s_ds.root_note;
    *chord = s_ds.chord;
    if (s_ds.follow == DRONE_FOLLOW_OFF) return;
    uint8_t pc;
    chord_type_t type;
    if (!sequencer_core_progression_applied_chord(&pc, &type, NULL)) return;
    int d   = (((int)pc - (int)(s_ds.root_note % 12)) % 12 + 12 + 6) % 12 - 6;
    int eff = (int)s_ds.root_note + d;
    if (eff < DRONE_STD_ROOT_MIN) eff += 12;
    if (eff > DRONE_STD_ROOT_MAX) eff -= 12;
    *root = (uint8_t)eff;
    if (s_ds.follow == DRONE_FOLLOW_CHORD) *chord = type;
}

static bool drone_std_note_in(const uint8_t *set, uint8_t n, uint8_t note)
{
    for (uint8_t i = 0; i < n; i++) {
        if (set[i] == note) return true;
    }
    return false;
}

/* The single note emitter (drone_core.c drone_reconcile): one note per chord
 * note on the main (the root alone for an empty chord), a single
 * root+interval note on the sub; shared notes are held. */
static void drone_std_reconcile(uint32_t land_ms)
{
    uint8_t tgt[DRONE_CHORD_MAX_NOTES];
    uint8_t tn = 0;
    int16_t tsub = -1;
    if (drone_std_sounding()) {
        uint8_t root;
        chord_type_t chord;
        drone_std_effective(&root, &chord);
        const int8_t *formula = quantizer_chord_intervals(chord);
        uint8_t chord_n = drone_std_chord_note_count(chord);
        if (chord_n < 1) {
            tgt[tn++] = root;
        } else {
            for (uint8_t i = 0; i < chord_n; i++) {
                if (!formula || formula[i] < 0) break;
                tgt[tn++] = (uint8_t)SEQ_CLAMP_INT((int)root + (int)formula[i], 0, 127);
            }
        }
        if (s_ds.sub_enabled) {
            tsub = (int16_t)SEQ_CLAMP_INT((int)root + (int)s_ds.sub_interval, 12, 108);
        }
    }

    for (uint8_t i = 0; i < s_ds.sounding_n; i++) {
        if (!drone_std_note_in(tgt, tn, s_ds.sounding[i])) {
            drone_std_note(DRONE_STD_SYNTH_MAIN, false, (float)s_ds.sounding[i], land_ms);
        }
    }
    if (s_ds.sounding_sub >= 0 && s_ds.sounding_sub != tsub) {
        drone_std_note(DRONE_STD_SYNTH_SUB, false, (float)s_ds.sounding_sub, land_ms);
    }
    for (uint8_t i = 0; i < tn; i++) {
        if (!drone_std_note_in(s_ds.sounding, s_ds.sounding_n, tgt[i])) {
            drone_std_note(DRONE_STD_SYNTH_MAIN, true, (float)tgt[i], land_ms);
        }
    }
    if (tsub >= 0 && s_ds.sounding_sub != tsub) {
        drone_std_note(DRONE_STD_SYNTH_SUB, true, (float)tsub, land_ms);
    }

    memcpy(s_ds.sounding, tgt, tn);
    s_ds.sounding_n   = tn;
    s_ds.sounding_sub = tsub;
}

/* Note-off the whole sounding set and forget it (ahead of a rebuild). */
static void drone_std_release_all(uint32_t land_ms)
{
    for (uint8_t i = 0; i < s_ds.sounding_n; i++) {
        drone_std_note(DRONE_STD_SYNTH_MAIN, false, (float)s_ds.sounding[i], land_ms);
    }
    if (s_ds.sounding_sub >= 0) {
        drone_std_note(DRONE_STD_SYNTH_SUB, false, (float)s_ds.sounding_sub, land_ms);
    }
    s_ds.sounding_n   = 0;
    s_ds.sounding_sub = -1;
}

/* amy_sysclock() ms at which a progression change due on land_tick sounds;
 * now when the tick is 0 or already reached. */
static uint32_t drone_std_land_ms(uint32_t land_tick)
{
    uint32_t now = amy_sysclock();
    if (land_tick == 0) return now;
    int32_t ahead = (int32_t)(land_tick - sequencer_ticks());
    if (ahead <= 0) return now;
    return now + (uint32_t)((uint64_t)ahead * amy_global.us_per_tick / 1000u);
}

/* ── Public API ── */

void drone_std_core_init(void)
{
    amy_helpers_init();

    memset(&s_ds, 0, sizeof(s_ds));
    voice_params_init_defaults(&s_ds.vp);
    s_ds.enabled      = false;
    s_ds.source       = DRONE_SRC_WAVE;
    s_ds.wave         = SAW_DOWN;
    s_ds.chord        = CHORD_OFF;   /* root note only until a chord is chosen */
    s_ds.root_note    = 45;          /* A2, same default as the stutter drone */
    s_ds.follow       = DRONE_FOLLOW_OFF;
    s_ds.sounding_sub = -1;
    s_ds.level        = 0.5f;
    s_ds.patch        = 25;
    s_ds.sub_enabled  = true;
    s_ds.sub_interval = -12;
    /* Default ADSR: slow swell (unauthored until the graph editor commits). */
    s_ds.vp.env.attack_ms   = 200;
    s_ds.vp.env.decay_ms    = 300;
    s_ds.vp.env.sustain_pct = 100;
    s_ds.vp.env.release_ms  = 600;
    s_ds.vp.env.eg_type     = 0;
    s_ds.vp.env1.attack_ms   = 15;
    s_ds.vp.env1.decay_ms    = 400;
    s_ds.vp.env1.sustain_pct = 25;
    s_ds.vp.env1.release_ms  = 400;
    s_ds.vp.env1.eg_type     = 0;
    /* Filter defaults: bypass, but a sensible LPF starting point for the
     * editor to seed from when the user first enables one. */
    s_ds.vp.filter.filter_type = SEQ_FILTER_LPF;
    s_ds.vp.filter.cutoff_hz   = 1200.0f;
    s_ds.vp.filter.resonance   = 1.0f;
    s_ds.vp.filter.enabled     = false;
    /* LFO defaults for the editor seed; inert until authored + enabled. */
    s_ds.vp.lfo.mode    = LFO_MODE_FREE;
    s_ds.vp.lfo.wave    = LFO_WAVE_SINE;
    s_ds.vp.lfo.rate    = LFO_RATE_1BAR;
    s_ds.vp.lfo.depth   = 50;
    s_ds.vp.lfo.targets = LFO_TGT_BIT(LFO_TARGET_FILTER);

    drone_std_rebuild();
    ESP_LOGI(TAG, "drone_std_core initialized (synths %u/%u)",
             DRONE_STD_SYNTH_MAIN, DRONE_STD_SYNTH_SUB);
}

void drone_std_core_service(void)
{
    /* A param changed while off still rebuilds, so it is live on the next
     * enable. */
    if (s_ds_rebuild) {
        s_ds_rebuild = false;
        s_ds_dirty = false;
        uint32_t now = amy_sysclock();
        drone_std_release_all(now);
        drone_std_rebuild();
        drone_std_reconcile(now);
    } else if (s_ds_dirty) {
        s_ds_dirty = false;
        drone_std_reconcile(amy_sysclock());
    }
}

void drone_std_core_follow_changed(void)
{
    if (s_ds.follow == DRONE_FOLLOW_OFF) return;
    uint32_t land_tick = 0;
    (void)sequencer_core_progression_applied_chord(NULL, NULL, &land_tick);
    /* Synchronous: drone_std_core_service() already ran this frame. */
    drone_std_reconcile(drone_std_land_ms(land_tick));
}

void drone_std_core_refresh_lfo_freq(void)
{
    if (!drone_std_lfo_on() || s_ds.source != DRONE_SRC_WAVE) return;
    float bpm = (float)seq_get_bpm();
    if (fabsf(bpm - s_ds.last_lfo_bpm) < 0.5f) return;
    s_ds.last_lfo_bpm = bpm;
    voice_apply_native_lfo(DRONE_STD_SYNTH_MAIN, &s_ds.vp.lfo, (uint16_t)bpm);
    if (s_ds.sub_enabled) {
        voice_apply_native_lfo(DRONE_STD_SYNTH_SUB, &s_ds.vp.lfo, (uint16_t)bpm);
    }
}

void drone_std_set_enabled(bool on)
{
    if (s_ds.enabled == on) return;
    s_ds.enabled = on;
    if (on) drone_std_mark_rebuild();   /* fresh enable reflects current params */
    else    drone_std_mark_dirty();
    ESP_LOGI(TAG, "drone_std %s", on ? "ON" : "OFF");
}

void drone_std_set_solo_muted(bool muted)
{
    if (s_ds.solo_muted == muted) return;
    s_ds.solo_muted = muted;
    if (!s_ds.enabled) return;      /* nothing sounding either way */
    if (!muted) drone_std_mark_rebuild();  /* mirror the fresh-enable path */
    else        drone_std_mark_dirty();
}

void drone_std_set_paused(bool paused)
{
    if (s_ds.paused == paused) return;
    s_ds.paused = paused;
    if (!s_ds.enabled) return;
    if (!paused) drone_std_mark_rebuild();
    else         drone_std_mark_dirty();
}

void drone_std_set_source(drone_source_t src)
{
    if (src != DRONE_SRC_WAVE && src != DRONE_SRC_PATCH) return;
    if (s_ds.source == src) return;
    s_ds.source = src;
    drone_std_mark_rebuild();
}

void drone_std_set_wave(uint16_t amy_wave)
{
    if (amy_wave == NOISE || amy_wave == KS) amy_wave = SAW_DOWN;
    if (s_ds.wave == amy_wave) return;
    s_ds.wave = amy_wave;
    if (s_ds.source == DRONE_SRC_WAVE) drone_std_mark_rebuild();
}

void drone_std_set_chord(chord_type_t chord)
{
    if (chord >= CHORD_TYPE_COUNT) return;
    if (s_ds.chord == chord) return;
    s_ds.chord = chord;
    drone_std_mark_dirty();
}

void drone_std_set_root_note(uint8_t note)
{
    uint8_t clamped = (uint8_t)SEQ_CLAMP_INT((int)note, DRONE_STD_ROOT_MIN,
                                             DRONE_STD_ROOT_MAX);
    if (s_ds.root_note == clamped) return;
    s_ds.root_note = clamped;
    drone_std_mark_dirty();
}

void drone_std_set_follow(drone_follow_t f)
{
    if (f >= DRONE_FOLLOW_COUNT) return;
    if (s_ds.follow == f) return;
    s_ds.follow = f;
    drone_std_mark_dirty();
}

void drone_std_set_level(float v)
{
    v = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
    if (fabsf(s_ds.level - v) < 0.001f) return;
    s_ds.level = v;
    drone_std_mark_rebuild();
}

void drone_std_set_patch(uint16_t patch)
{
    /* Shared exclusion predicate with the stutter drone (same voice model). */
    if (drone_patch_excluded(patch)) patch = SEQ_PATCH_TRIANGLE;
    if (s_ds.patch == patch) return;
    s_ds.patch = patch;
    if (s_ds.source == DRONE_SRC_PATCH) drone_std_mark_rebuild();
}

void drone_std_set_sub_enabled(bool on)
{
    if (s_ds.sub_enabled == on) return;
    s_ds.sub_enabled = on;
    /* On configures the sub synth; off only releases its note. */
    if (on) drone_std_mark_rebuild();
    else    drone_std_mark_dirty();
}

void drone_std_set_sub_interval(int8_t st)
{
    int v = SEQ_CLAMP_INT((int)st, -36, 0);
    if (s_ds.sub_interval == (int8_t)v) return;
    s_ds.sub_interval = (int8_t)v;
    drone_std_mark_dirty();
}

void drone_std_set_amp_trim(float v)
{
    v = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
    if (fabsf(s_ds.vp.amp_trim - v) < 0.001f) return;
    s_ds.vp.amp_trim = v;
    drone_std_mark_rebuild();
}

/* ── Envelopes ── */

void drone_std_get_envelope(seq_env_t *out)
{
    if (out) *out = s_ds.vp.env;
}

void drone_std_set_envelope(const seq_env_t *env)
{
    if (!env) return;
    s_ds.vp.env = *env;
    if (s_ds.vp.env.attack_ms < 2)  s_ds.vp.env.attack_ms = 2;
    if (s_ds.vp.env.release_ms < 5) s_ds.vp.env.release_ms = 5;
    s_ds.vp.env_authored = true;
    sequencer_core_push_envelope(DRONE_STD_SYNTH_MAIN, &s_ds.vp.env);
    if (s_ds.sub_enabled) {
        sequencer_core_push_envelope(DRONE_STD_SYNTH_SUB, &s_ds.vp.env);
    }
}

void drone_std_get_envelope2(seq_env_t *out)
{
    if (out) *out = s_ds.vp.env1;
}

void drone_std_set_envelope2(const seq_env_t *env)
{
    if (!env) return;
    s_ds.vp.env1 = *env;
    if (s_ds.vp.env1.attack_ms < 2)  s_ds.vp.env1.attack_ms = 2;
    if (s_ds.vp.env1.release_ms < 5) s_ds.vp.env1.release_ms = 5;
    s_ds.vp.env1_authored = true;
    sequencer_core_push_envelope_eg1(DRONE_STD_SYNTH_MAIN, 0, &s_ds.vp.env1);
    if (s_ds.sub_enabled) {
        sequencer_core_push_envelope_eg1(DRONE_STD_SYNTH_SUB, 0, &s_ds.vp.env1);
    }
}

void drone_std_preview_envelope(const seq_env_t *env)
{
    if (!env) return;
    sequencer_core_push_envelope(DRONE_STD_SYNTH_MAIN, env);
    if (s_ds.sub_enabled) {
        sequencer_core_push_envelope(DRONE_STD_SYNTH_SUB, env);
    }
}

void drone_std_preview_envelope2(const seq_env_t *env)
{
    if (!env) return;
    sequencer_core_push_envelope_eg1(DRONE_STD_SYNTH_MAIN, 0, env);
    if (s_ds.sub_enabled) {
        sequencer_core_push_envelope_eg1(DRONE_STD_SYNTH_SUB, 0, env);
    }
}

/* ── Free filter ── */

void drone_std_get_filter(seq_filter_t *out)
{
    if (out) *out = s_ds.vp.filter;
}

void drone_std_set_filter(const seq_filter_t *f)
{
    if (!f) return;
    s_ds.vp.filter = *f;
    s_ds.vp.filter_authored = true;
    drone_std_push_filter(DRONE_STD_SYNTH_MAIN, &s_ds.vp.filter);
    if (s_ds.sub_enabled) {
        drone_std_push_filter(DRONE_STD_SYNTH_SUB, &s_ds.vp.filter);
    }
}

void drone_std_preview_filter(const seq_filter_t *f)
{
    if (!f) return;
    drone_std_push_filter(DRONE_STD_SYNTH_MAIN, f);
    if (s_ds.sub_enabled) {
        drone_std_push_filter(DRONE_STD_SYNTH_SUB, f);
    }
}

/* ── Free LFO ── */

/* Distortion reaches both drone slots, like the filter: main and sub are one
 * instrument to the player, so a single editor block governs the pair. */
void drone_std_get_dist(seq_dist_t *out)
{
    if (out) *out = s_ds.vp.dist;
}

static void drone_std_push_dist(const seq_dist_t *d)
{
    voice_apply_dist(DRONE_STD_SYNTH_MAIN, d);
    if (s_ds.sub_enabled) voice_apply_dist(DRONE_STD_SYNTH_SUB, d);
}

void drone_std_set_dist(const seq_dist_t *d)
{
    if (!d) return;
    s_ds.vp.dist = *d;
    voice_dist_clamp(&s_ds.vp.dist);
    s_ds.vp.dist_authored = true;
    drone_std_push_dist(&s_ds.vp.dist);
}

void drone_std_preview_dist(const seq_dist_t *d)
{
    if (d) drone_std_push_dist(d);
}

void drone_std_reapply_dist(void)
{
    if (s_ds.vp.dist_authored) drone_std_push_dist(&s_ds.vp.dist);
}

void drone_std_get_lfo(seq_lfo_t *out)
{
    if (out) *out = s_ds.vp.lfo;
}

void drone_std_set_lfo(const seq_lfo_t *lfo)
{
    if (!lfo) return;
    s_ds.vp.lfo = *lfo;
    s_ds.vp.lfo_authored = true;
    if (s_ds.source != DRONE_SRC_WAVE) return;  /* patch owns its topology */
    bool on = drone_std_lfo_on();
    voice_apply_native_lfo(DRONE_STD_SYNTH_MAIN, on ? &s_ds.vp.lfo : NULL,
                           seq_get_bpm());
    if (s_ds.sub_enabled) {
        voice_apply_native_lfo(DRONE_STD_SYNTH_SUB, on ? &s_ds.vp.lfo : NULL,
                               seq_get_bpm());
    }
}

void drone_std_get_voice_params(voice_params_t *out)
{
    if (out) *out = s_ds.vp;
}

void drone_std_set_voice_params(const voice_params_t *vp)
{
    if (!vp) return;
    s_ds.vp = *vp;
    if (s_ds.vp.env.attack_ms < 2)   s_ds.vp.env.attack_ms = 2;
    if (s_ds.vp.env.release_ms < 5)  s_ds.vp.env.release_ms = 5;
    if (s_ds.vp.env1.attack_ms < 2)  s_ds.vp.env1.attack_ms = 2;
    if (s_ds.vp.env1.release_ms < 5) s_ds.vp.env1.release_ms = 5;
    voice_dist_clamp(&s_ds.vp.dist);
    s_ds.vp.amp_trim = SEQ_CLAMP_F32(s_ds.vp.amp_trim, 0.0f, 1.0f);
    /* The rebuild resets every osc, then pushes the filter and LFO and
     * re-imposes only the authored envelopes and distortion. */
    drone_std_mark_rebuild();
}

/* ── Getters ── */
bool           drone_std_get_enabled(void)      { return s_ds.enabled; }
drone_source_t drone_std_get_source(void)       { return s_ds.source; }
uint16_t       drone_std_get_wave(void)         { return s_ds.wave; }
chord_type_t   drone_std_get_chord(void)        { return s_ds.chord; }
uint8_t        drone_std_get_root_note(void)    { return s_ds.root_note; }
drone_follow_t drone_std_get_follow(void)       { return s_ds.follow; }
float          drone_std_get_level(void)        { return s_ds.level; }
uint16_t       drone_std_get_patch(void)        { return s_ds.patch; }
bool           drone_std_get_sub_enabled(void)  { return s_ds.sub_enabled; }
int8_t         drone_std_get_sub_interval(void) { return s_ds.sub_interval; }
float          drone_std_get_amp_trim(void)     { return s_ds.vp.amp_trim; }
