/* drone_core.c - standalone "stutter house drone" synth.
 * Design notes: drone_core.h.
 *
 * Race safety: every AMY interaction goes through the amy_helpers ingress seam
 * (amy_helpers.h), never direct synth[] access - the render path walks synth[]
 * under the queue lock, so all config/notes are queued deltas. All callers here
 * are FreeRTOS tasks, never ISRs. */

#include "custompatches/drone_core.h"
#include "custompatches/drone_common.h"
#include "synth_ui.h"      /* seq_get_bpm() (live global BPM) */
#include "sequencer_core.h"    /* shared apply path, envelope pushes, tick time */
#include "seq_clamp.h"
#include "chord_types.h"   /* chord_type_t, chord_type_name() */
#include "lfo_targets.h"   /* lfo_targets_of_patch() */
#include "lfo_bank.h"      /* shared LFO synths (lfo_bank_bind) */
#include "custompatches/bass_presets.h"   /* bass_preset_osc_level() */
#include "custompatches/fm_voice.h"       /* s_fm_voice, fm_voice_push */
#include "custompatches/additive_voice.h" /* s_additive_voice, additive_voice_push_live */
#include "amy.h"
#include "amy_helpers.h"
#include "voice_config.h"  /* voice_params_init_defaults */
#include "sdkconfig.h"
#include "esp_log.h"

#include <math.h>
#include <string.h>

static const char *TAG = "drone_core";

/* ── Synth slots: DRONE_SYNTH_MAIN/SUB from synth_slots.h's static pool ── */
#include "synth_slots.h"
#define DRONE_SUB_VOICES   1     /* sub is always a single tone (root)        */

#define DRONE_ROOT_DEFAULT 45    /* A2 — matches the former Am7 voicing */

/* Limits */
#define DRONE_BLIP_MIN_HZ  100.0f  /* floor of the blip's downward zap */
#define DRONE_GATE_MIN     0.05f   /* gate open time, fraction of a step */
#define DRONE_GATE_MAX     0.95f
#define DRONE_SWING_MAX    66      /* percent of one subdivision, applied to odd steps */
#define DRONE_BLIP_MAX     1.0f    /* per-step downward filter-zap depth (0 = off)     */
#define DRONE_PAT_STEPS    8       /* steps in a pattern mask, one per subdivision     */

/* Gate look-ahead: two 50 ms UI frames, so every edge is queued at least one
 * frame before it is due. Each queued edge costs one delta per voice; do not
 * raise without recomputing that footprint. */
#define DRONE_GATE_AHEAD_US 100000u

/* Chord intervals come from the shared quantizer_chord_intervals(chord_type_t)
 * table (quantizer.h). */

/* ── Step patterns ── 8-bit masks (bit0 = step0, LSB-first). A 0 bit
 * means that stutter subdivision is skipped (the gate stays ducked). FULL =
 * legacy. */
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
    chord_type_t   chord;       /* chord preset (shared chord_type_t) */
    uint8_t        root_note;   /* drone-local root (DRONE_ROOT_MIN..MAX) */
    drone_follow_t follow;      /* chord progression follow mode */
    drone_notes_t  notes;       /* sounding set, kept by drone_reconcile() */
    float          amp_peak;    /* 0..1 on-beat level knob (linear: peak_lin = amp_peak) */
    float          amp_duck;    /* 0..1 duck depth knob  (duck_db = amp_duck * 40 dB)   */
    note_div_t     rate;
    uint16_t       patch;       /* SEQ_PATCH_* id, drone_stutter_patch_excluded() domain */
    uint8_t        voices;      /* main voices as built: the chord is truncated
                                   to this many tones */
    uint16_t       mask;        /* main's gate/filter oscs (drone_voice_mask) */
    bool           sub_enabled;
    int8_t         sub_interval;/* semitones below the main          */
    /* stutter-house controls */
    float          gate_len;    /* gate open time: 0.05..0.95 of a step   */
    uint8_t        swing_pct;   /* 0..66 swing on the gate/pattern grid   */
    float          blip_depth;  /* 0..1 per-step downward filter-zap depth    */
    drone_pattern_t pattern;    /* 8-step on/off mask                         */
    float          filter_hz;   /* cutoff CONST of the filter last pushed
                                   (stored or previewed); 0 = no filter of the
                                   drone's own, so no blip */
    float          last_blip_cutoff; /* to avoid redundant cutoff re-sends    */
    /* Gate scheduler: every edge at a tick below sched_until_tick has been
     * sent, the last of them due at AMY time sched_until_ms; gate_running
     * while the scheduler owns the level. */
    uint32_t       sched_until_tick;
    uint32_t       sched_until_ms;
    bool           gate_running;
    voice_params_t vp;          /* env/env1 (graph editor), filter (filter
                                   editor), lfo (LFO editor), dist, each with
                                   its authored flag, plus amp_trim (folded
                                   into amp_peak by s_amp_peak_lin()). */
} drone_state_t;

static drone_state_t s_d;

#if CONFIG_SEQ_LFO_SHARED
/* The shared LFO synth the main synth holds (lfo_bank.h); LFO_BANK_NONE
 * while it has no LFO. Kept outside s_d, which init clears. The sub holds
 * none. */
static uint8_t s_d_lfo_ref = LFO_BANK_NONE;
#endif

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

/* ── Peak/Duck amp levels ──────────────────────────────────────────────────
 * SINGLE source of truth for the gate levels - never inline the formula.
 * peak_lin = amp_peak x amp_trim is the on level; the off level sits duck_db
 * = amp_duck * 40 below it: peak_lin * 10^(-duck_db/20). Both are written as
 * amp CONST; AMY hard-mutes a level below AMP_THRESH_PLUS. */
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

static inline float s_amp_off_lin(void)
{
    return s_amp_peak_lin() * powf(10.0f, -s_amp_duck_db() / 20.0f);
}

/* The oscs of a `patch` voice the gate and the filter address (bit n = osc
 * n): the coupled oscs of a wave layout (osc 0 on a wave voice), else a
 * patch string's lfo_targets voice mask (the SILENT head on a Juno, its VCA;
 * the ALGO control osc on DX7/FM). False for a patch with neither, which the
 * stutter drone cannot gate. Not sequencer_core_patch_amp_osc(): on a Juno
 * that is every osc, and its patch sets each osc's level through amp CONST,
 * so a gate write there would switch silent oscs on. mask may be NULL. */
static bool drone_voice_mask(uint16_t patch, uint16_t *mask)
{
    uint8_t coupled;
    if (sequencer_core_lfo_wave_layout(patch, &coupled)) {
        if (mask) *mask = coupled;
        return true;
    }
    lfo_target_map_t map;
    if (!lfo_targets_of_patch(patch, &map)) return false;
    if (mask) *mask = map.voice_mask;
    return true;
}

/* One amp CONST write to every osc in `mask` of `synth`'s voices, built from
 * `patch`, at AMY time `time` (0 = now). A bass preset's two oscs each take
 * the level scaled by the preset's own balance (bass_preset_osc_level). */
static void drone_push_amp(uint8_t synth, uint16_t patch, uint16_t mask, float level,
                           uint32_t time)
{
    for (uint8_t o = 0; o < 16u; o++) {
        if (!(mask & (1u << o))) continue;
        float l = level * bass_preset_osc_level(patch, o);
        amy_event *e = amy_helpers_event_begin();
        e->synth                 = synth;
        e->osc                   = o;
        e->amp_coefs[COEF_CONST] = (l < DRONE_AMP_MUTE) ? DRONE_AMP_MUTE : l;
        if (time != 0u) e->time  = time;
        amy_helpers_event_send(e);
    }
}

/* One gate edge on main and, when enabled, the sub's osc 0. */
static void drone_gate_edge(float level, uint32_t time)
{
    drone_push_amp(DRONE_SYNTH_MAIN, s_d.patch, s_d.mask, level, time);
    if (s_d.sub_enabled)
        drone_push_amp(DRONE_SYNTH_SUB, SEQ_PATCH_SINE, 0x01u, level, time);
}

/* Push a filter to main's gate oscs (on a Juno the head alone: a filter on
 * every chained osc would stack one per osc): type, resonance and cutoff
 * CONST, then its envelope routing matrix (all eight slots once the filter
 * block is authored), the SCAN rails on a wavetable, and the EG1
 * breakpoints whenever an EG1 rail is live (sequencer_core_push_envelope_eg1()).
 * The blip then works around this cutoff. The sub has no filter. */
static void drone_push_filter(const seq_filter_t *f)
{
    float hz  = sequencer_core_filter_const_hz(f);
    bool  own = s_d.vp.filter_authored;
    for (uint8_t o = 0; o < 16u; o++) {
        if (!(s_d.mask & (1u << o))) continue;
        amy_event *e = amy_helpers_event_begin();
        e->synth = DRONE_SYNTH_MAIN;
        e->osc   = o;
        if (f->enabled) {
            e->filter_type = f->filter_type;
            e->filter_freq_coefs[COEF_CONST] = hz;
            e->resonance = f->resonance;
        } else {
            e->filter_type = FILTER_NONE;
        }
        amy_helpers_event_send(e);
        sequencer_core_push_eg_depths(DRONE_SYNTH_MAIN, (int)o, f, own);
        if (seq_filter_eg1_live(f))
            sequencer_core_push_envelope_eg1(DRONE_SYNTH_MAIN, o, &s_d.vp.env1);
    }
    if (sequencer_core_is_wavetable_patch(s_d.patch))
        sequencer_core_push_eg_scan(DRONE_SYNTH_MAIN, (uint8_t)s_d.mask, f, own);
    s_d.filter_hz        = f->enabled ? hz : 0.0f;
    s_d.last_blip_cutoff = s_d.filter_hz;
}

/* One filter cutoff CONST (the blip) on main's gate oscs. */
static void drone_push_cutoff(float cutoff)
{
    for (uint8_t o = 0; o < 16u; o++) {
        if (!(s_d.mask & (1u << o))) continue;
        amy_event *e = amy_helpers_event_begin();
        e->synth = DRONE_SYNTH_MAIN;
        e->osc   = o;
        e->filter_freq_coefs[COEF_CONST] = cutoff;
        amy_helpers_event_send(e);
    }
}

static inline bool drone_lfo_on(void)
{
    return s_d.vp.lfo_authored && s_d.vp.lfo.enabled;
}

/* Apply the drone's LFO to the main synth as currently built, as the
 * standard drone does (drone_std_core.c): with CONFIG_SEQ_LFO_SHARED a
 * shared LFO synth over the wave layout's coupled oscs or a patch string's
 * lfo_targets map; when the bank is full or the LFO is off the main has no
 * LFO (drones have no stepper). Without it the native carriers on a wave
 * layout (NULL clears a stale COEF_MOD - voice_config.h); a patch string has
 * no LFO. The LFO rides COEF_MOD1, apart from the gate's amp CONST. */
static void drone_apply_lfo(void)
{
    const seq_lfo_t *lfo = drone_lfo_on() ? &s_d.vp.lfo : NULL;
    uint16_t bpm = seq_get_bpm();
#if CONFIG_SEQ_LFO_SHARED
    lfo_target_map_t map;
    uint8_t coupled;
    bool mapped;
    if (sequencer_core_lfo_wave_layout(s_d.patch, &coupled)) {
        map = (lfo_target_map_t){ .voice_mask = coupled, .pitch_mask = coupled,
                                  .scan_mask  = coupled };
        mapped = true;
    } else {
        mapped = lfo_targets_of_patch(s_d.patch, &map);
    }
    if (mapped)
        (void)lfo_bank_bind(DRONE_SYNTH_MAIN, &map, lfo, bpm, &s_d_lfo_ref);
    else
        lfo_bank_unbind(&s_d_lfo_ref);   /* the patch re-apply unwired it */
#else
    uint8_t carrier, coupled;
    if (!sequencer_core_lfo_native_layout(s_d.patch, &carrier, &coupled)) return;
    voice_apply_native_lfo_topo(DRONE_SYNTH_MAIN, lfo, bpm, carrier,
                                coupled, coupled);
#endif
}

static void drone_push_dist(const seq_dist_t *d)
{
    voice_apply_dist(DRONE_SYNTH_MAIN, d);
}

/* (Re)build both synths. Main takes the patch through the shared apply path
 * (sequencer_core_configure_synth_slot) at as many voices as the patch's osc
 * budget allows, up to DRONE_CHORD_MAX_NOTES; then the authored groups go
 * back on (envelopes, filter, distortion), the LFO is bound, and the gate's
 * current level is sent at once (the rebuild reset amp CONST). The sub is a
 * one-voice sine with EG0 and the gate, no filter. Caller: the rebuild drain
 * in drone_core_service(), or init. */
static void drone_rebuild(void)
{
    uint16_t patch = s_d.patch;
    if (!drone_voice_mask(patch, &s_d.mask)) s_d.mask = 0x01u;
    s_d.voices = drone_voices_for(patch);
    sequencer_core_configure_synth_slot(DRONE_SYNTH_MAIN, patch, s_d.voices, NULL);
    if (s_d.vp.env_authored)
        sequencer_core_push_envelope_osc(DRONE_SYNTH_MAIN,
                                         sequencer_core_patch_voice_osc(patch),
                                         &s_d.vp.env);
    if (s_d.vp.env1_authored)
        sequencer_core_push_envelope_eg1(DRONE_SYNTH_MAIN, 0, &s_d.vp.env1);
    s_d.filter_hz = 0.0f;
    if (s_d.vp.filter_authored) drone_push_filter(&s_d.vp.filter);
    drone_reapply_dist();
    drone_apply_lfo();

    if (s_d.sub_enabled) {
        sequencer_core_configure_synth_slot(DRONE_SYNTH_SUB, SEQ_PATCH_SINE,
                                            DRONE_SUB_VOICES, NULL);
        if (s_d.vp.env_authored)
            sequencer_core_push_envelope(DRONE_SYNTH_SUB, &s_d.vp.env);
    }

    /* The current gate level, now. A wave voice also drops the shared wave
     * build's velocity term (VEL 1), as the drone's own voice never had one;
     * a patch string keeps its VEL and EG0 coefs. */
    if (sequencer_core_is_wave_patch(patch)) {
        amy_event *e = amy_helpers_event_begin();
        e->synth                 = DRONE_SYNTH_MAIN;
        e->osc                   = 0;
        e->amp_coefs[COEF_VEL]   = 0.0f;
        amy_helpers_event_send(e);
    }
    if (s_d.sub_enabled) {
        amy_event *e = amy_helpers_event_begin();
        e->synth                 = DRONE_SYNTH_SUB;
        e->osc                   = 0;
        e->amp_coefs[COEF_VEL]   = 0.0f;
        amy_helpers_event_send(e);
    }
    drone_gate_edge(s_amp_peak_lin(), 0u);
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

/* Bring the sounding set to the current voicing (drone_apply_enabled): the
 * chord tones that fit the main's built voices, and the sub note. */
static void drone_reconcile(uint32_t land_ms)
{
    drone_voicing_t v = {
        .main_synth   = DRONE_SYNTH_MAIN,
        .sub_synth    = DRONE_SYNTH_SUB,
        .sounding     = drone_sounding(),
        .voices       = s_d.voices,
        .sub          = s_d.sub_enabled,
        .sub_interval = s_d.sub_interval,
    };
    if (v.sounding) drone_effective(&v.root, &v.chord);
    drone_apply_enabled(&s_d.notes, &v, land_ms);
}

/* Note-off the whole sounding set and forget it (ahead of a rebuild). */
static void drone_release_all_notes(uint32_t land_ms)
{
    drone_release_all(&s_d.notes, DRONE_SYNTH_MAIN, DRONE_SYNTH_SUB, land_ms);
}

/* amy_sysclock() ms at which a progression change due on land_tick sounds;
 * now when the tick is 0 or already reached. */
static uint32_t drone_land_ms(uint32_t land_tick)
{
    return sequencer_core_tick_to_amy_ms(land_tick);
}

/* ── Gate scheduler ──
 * Step k of the stutter grid starts at k * T (T = the rate's ticks, grid
 * anchored at tick 0, pattern step k % DRONE_PAT_STEPS). An open step's on
 * edge sits at its start, odd steps delayed by swing_pct of a step; its off
 * edge gate_len of a step later, clamped to one tick before the next step's
 * on edge (that step's unswung start when it is pattern-off). A pattern-off
 * step sends no on edge, only an off edge at its unswung start, so it sits
 * at the duck level whatever came before. */

static inline bool drone_step_open(uint32_t k)
{
    return (s_pattern_mask[s_d.pattern] >> (k % DRONE_PAT_STEPS)) & 1u;
}

/* Tick of step k's on edge: its start, odd steps swung. */
static uint32_t drone_step_on_tick(uint32_t k, uint32_t T)
{
    uint32_t t = k * T;
    if ((k & 1u) && s_d.swing_pct > 0)
        t += (uint32_t)((float)T * (float)s_d.swing_pct / 100.0f);
    return t;
}

/* Tick of step k's off edge (k open). */
static uint32_t drone_step_off_tick(uint32_t k, uint32_t T)
{
    uint32_t on      = drone_step_on_tick(k, T);
    uint32_t next_on = drone_step_open(k + 1u) ? drone_step_on_tick(k + 1u, T)
                                               : (k + 1u) * T;
    uint32_t len     = (uint32_t)(s_d.gate_len * (float)T);
    uint32_t off     = on + ((len > 0u) ? len : 1u);
    return (off < next_on) ? off : next_on - 1u;
}

/* The gate look-ahead in ticks at AMY's current tick length. */
static uint32_t drone_gate_ahead_ticks(void)
{
    uint32_t us = amy_global.us_per_tick;
    return (us > 0u) ? DRONE_GATE_AHEAD_US / us : 0u;
}

/* Send every edge in [sched_until_tick, now + look-ahead) with its AMY time
 * (sequencer_core_tick_to_amy_ms() clamps a missed edge to now, so a late
 * frame sends a missed step's on then off - FIFO - and it collapses to the
 * duck level). At most one look-ahead of missed edges is replayed. */
static void drone_gate_schedule(void)
{
    uint32_t now   = sequencer_ticks();
    uint32_t ahead = drone_gate_ahead_ticks();
    uint32_t end   = now + ahead;
    if ((int32_t)(now - s_d.sched_until_tick) > (int32_t)ahead)
        s_d.sched_until_tick = now - ahead;
    uint32_t from = s_d.sched_until_tick;
    if ((int32_t)(end - from) <= 0) return;

    uint32_t T = note_div_ticks(s_d.rate);
    if (T == 0u) return;
    float on_lvl  = s_amp_peak_lin();
    float off_lvl = s_amp_off_lin();
    /* From the step before `from`'s: a swung step's off edge can fall in the
     * next step's grid span. */
    uint32_t k0 = from / T;
    if (k0 > 0u) k0--;
    for (uint32_t k = k0; (int32_t)(k * T - end) < 0; k++) {
        if (!drone_step_open(k)) {
            uint32_t t = k * T;
            if ((int32_t)(t - from) >= 0 && (int32_t)(t - end) < 0)
                drone_gate_edge(off_lvl, sequencer_core_tick_to_amy_ms(t));
            continue;
        }
        uint32_t on  = drone_step_on_tick(k, T);
        uint32_t off = drone_step_off_tick(k, T);
        if ((int32_t)(on - from) >= 0 && (int32_t)(on - end) < 0)
            drone_gate_edge(on_lvl, sequencer_core_tick_to_amy_ms(on));
        if ((int32_t)(off - from) >= 0 && (int32_t)(off - end) < 0)
            drone_gate_edge(off_lvl, sequencer_core_tick_to_amy_ms(off));
    }
    s_d.sched_until_tick = end;
    s_d.sched_until_ms   = sequencer_core_tick_to_amy_ms(end);
}

/* ── Public API ── */

void drone_core_init(void)
{
    amy_helpers_init();

    memset(&s_d, 0, sizeof(s_d));
    voice_params_init_defaults(&s_d.vp);   /* unauthored, amp trim at unity */
    s_d.enabled      = false;
    s_d.chord        = CHORD_OFF;   /* root note only until a chord is chosen */
    s_d.root_note    = DRONE_ROOT_DEFAULT;
    s_d.follow       = DRONE_FOLLOW_OFF;
    drone_notes_init(&s_d.notes);
    s_d.amp_peak     = 0.5f;     /* on-beat level (linear; 0.5 = -6 dB)   */
    s_d.amp_duck     = 0.5f;     /* duck depth knob (0.5 -> 20 dB duck)   */
    s_d.rate         = NOTE_DIV_1_16;
    s_d.patch        = SEQ_PATCH_SAW_DOWN;
    s_d.sub_enabled  = true;
    s_d.sub_interval = -12;      /* one octave below */
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
    /* EG1 timing for the filter editor's envelope routing; nothing routes to
     * it until a depth is set. */
    s_d.vp.env1.attack_ms   = 15;
    s_d.vp.env1.decay_ms    = 400;
    s_d.vp.env1.sustain_pct = 25;
    s_d.vp.env1.release_ms  = 400;
    s_d.vp.env1.eg_type     = 0;   /* ENVELOPE_NORMAL */
    /* Authored filter and LFO: an LPF24 around ~1100 Hz swept +/-1 octave by
     * a 4-bar sine, close to the former 600-2000 Hz, 4-bar sweep. */
    s_d.vp.filter.filter_type = SEQ_FILTER_LPF24;
    s_d.vp.filter.cutoff_hz   = 1100.0f;
    s_d.vp.filter.resonance   = 1.5f;
    s_d.vp.filter.enabled     = true;
    s_d.vp.filter_authored    = true;
    s_d.vp.lfo.enabled   = true;
    s_d.vp.lfo.mode      = LFO_MODE_FREE;
    s_d.vp.lfo.wave      = LFO_WAVE_SINE;
    s_d.vp.lfo.rate      = NOTE_DIV_4BAR;
    s_d.vp.lfo.depth     = 50;
    s_d.vp.lfo.targets   = LFO_TGT_BIT(LFO_TARGET_FILTER);
    s_d.vp.lfo.flt_oct_q = 4;      /* +/-1 octave */
    s_d.vp.lfo_authored  = true;

    drone_rebuild();
    ESP_LOGI(TAG, "drone_core initialized (synths %u/%u)",
             DRONE_SYNTH_MAIN, DRONE_SYNTH_SUB);
}

void drone_core_service(void)
{
    /* Rebuild drain: a synth-addressed event binds to absolute oscs when AMY
     * parses it and a configure frees the old oscs at its own time, so queued
     * gate edges would fire into the oscs' next owner. While a rebuild is
     * pending no new edges go out, and the rebuild waits until every queued
     * edge is due - in AMY time, which the edges are stamped in: the tick
     * counter stops with the transport and runs ahead of the stamps after a
     * BPM rise. The drain runs BEFORE the enabled gate: a param changed while
     * off must still rebuild, and a disable must release its notes. */
    if (s_d_rebuild) {
        if (!AMY_TIME_GEQ(amy_sysclock(), s_d.sched_until_ms)) return;
        s_d_rebuild = false;
        s_d_dirty = false;
        uint32_t now = amy_sysclock();
        drone_release_all_notes(now);
        drone_rebuild();
        drone_reconcile(now);
        s_d.sched_until_tick = sequencer_ticks();
    } else if (s_d_dirty) {
        s_d_dirty = false;
        drone_reconcile(amy_sysclock());
    }

    /* The gate runs while the drone sounds. Every way back to sounding
     * (enable, solo release, transport play) is a rebuild, which re-sends
     * the on level, so a stop leaves the level where the queued edges put it. */
    if (drone_sounding()) {
        if (!s_d.gate_running) s_d.sched_until_tick = sequencer_ticks();
        s_d.gate_running = true;
        drone_gate_schedule();
    } else {
        s_d.gate_running = false;
    }

    if (!s_d.enabled) return;

    /* The blip, a pure function of the global musical clock, NOT of how often
     * this service runs: a downward zap at the start of each open step,
     * decaying across the step, around the cutoff of the filter last pushed.
     * AMY's logfreq downward slew-limit smooths the attack edge into
     * something musical. The pattern acts on the gate only. */
    if (s_d.filter_hz <= 0.0f) return;
    uint32_t now = sequencer_ticks();
    uint32_t ticks_per_sub = note_div_ticks(s_d.rate);
    uint32_t pat_ticks = ticks_per_sub * DRONE_PAT_STEPS;

    /* Swing: push ODD subdivisions later by swing_pct% of one subdivision, by
     * offsetting the clock used for the sub-phase calc. */
    uint32_t pos_in_pat = now % pat_ticks;
    uint32_t sub_index_raw = pos_in_pat / ticks_per_sub;
    uint32_t swing_off = 0;
    if ((sub_index_raw & 1u) && s_d.swing_pct > 0) {
        swing_off = (uint32_t)((float)ticks_per_sub * (float)s_d.swing_pct / 100.0f);
    }
    /* effective position within the current subdivision (0..1), swing-shifted */
    uint32_t swung = (now + pat_ticks - swing_off) % pat_ticks; /* avoid underflow */
    uint32_t sub_index = (swung / ticks_per_sub) % DRONE_PAT_STEPS;
    float frac_in_sub = (float)(swung % ticks_per_sub) / (float)ticks_per_sub;

    bool step_on = (s_pattern_mask[s_d.pattern] >> sub_index) & 1u;
    float cutoff = s_d.filter_hz;
    if (step_on && s_d.blip_depth > 0.0001f) {
        float env = expf(-4.0f * frac_in_sub);          /* 1 -> ~0 across the step */
        cutoff = s_d.filter_hz * (1.0f - s_d.blip_depth * env);
        if (cutoff < DRONE_BLIP_MIN_HZ) cutoff = DRONE_BLIP_MIN_HZ;
    }

    /* Skip near-identical pushes: with BLIP at 0 this settles on the cutoff
     * and sends nothing. */
    if (fabsf(cutoff - s_d.last_blip_cutoff) > 1.0f) {
        drone_push_cutoff(cutoff);
        s_d.last_blip_cutoff = cutoff;
    }
}

void drone_core_refresh_lfo_freq(void)
{
    /* With CONFIG_SEQ_LFO_SHARED: no per-voice carriers; lfo_bank_set_bpm()
     * retunes the shared synths. */
#if !CONFIG_SEQ_LFO_SHARED
    if (drone_lfo_on()) drone_apply_lfo();   /* no-op on a patch string */
#endif
}

void drone_core_fm_voice_changed(uint8_t what)
{
#if CONFIG_SYNTH_CUSTOM_FM
    /* Operators and routing only: osc 0's amp CONST (the gate / Level) is
     * untouched. */
    if (s_d.patch == SEQ_PATCH_FM_CUSTOM) fm_voice_push(DRONE_SYNTH_MAIN, &s_fm_voice, what);
#else
    (void)what;
#endif
}

void drone_core_additive_voice_changed(void)
{
#if CONFIG_SYNTH_ADDITIVE
    /* Partial ratios and levels only: the parent's amp CONST (the gate /
     * Level) is untouched. */
    if (s_d.patch == SEQ_PATCH_ADDITIVE_CUSTOM)
        additive_voice_push_live(DRONE_SYNTH_MAIN, &s_additive_voice);
#endif
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
        /* Rebuild so a fresh enable reflects the current params. The LFO
         * stays on the bar grid (lfo_bank.h). */
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

void drone_set_amp_peak(float c)
{
    c = SEQ_CLAMP_F32(c, 0.0f, 1.0f);
    if (fabsf(s_d.amp_peak - c) < 0.001f) return;
    s_d.amp_peak = c;   /* the gate's next edges carry it */
}

void drone_set_amp_duck(float m)
{
    /* Full range: duck_knob=1 -> -40 dB floor, not silence, so no cap needed. */
    m = SEQ_CLAMP_F32(m, 0.0f, 1.0f);
    if (fabsf(s_d.amp_duck - m) < 0.001f) return;
    s_d.amp_duck = m;   /* the gate's next edges carry it */
}

void drone_set_rate(note_div_t rate)
{
    if (rate >= NOTE_DIV_STD_COUNT) return;
    if (s_d.rate == rate) return;
    s_d.rate = rate;   /* the gate schedules on the new grid from the next frame */
}

bool drone_patch_excluded(uint16_t patch)
{
    /* The Kconfig-gated ranges (wavetables without AMY_WAVETABLE, FM,
     * additive, the custom table) through the single compile-awareness
     * point; then NOISE and KS: a held chord of noise or plucks is not a
     * drone. Everything else in the routable catalog plays. */
    if (sequencer_core_patch_compiled_out(patch)) return true;
    if (patch == SEQ_PATCH_NOISE || patch == SEQ_PATCH_KS) return true;
    return patch > SEQ_PATCH_ROUTABLE_MAX;
}

bool drone_stutter_patch_excluded(uint16_t patch)
{
    return drone_patch_excluded(patch) || !drone_voice_mask(patch, NULL);
}

void drone_set_patch(uint16_t patch)
{
    /* Cycling never offers an excluded patch; this snap defends direct
     * programmatic callers. */
    if (drone_stutter_patch_excluded(patch)) patch = SEQ_PATCH_TRIANGLE;
    if (s_d.patch == patch) return;
    s_d.patch = patch;
    drone_mark_rebuild();
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
    int v = SEQ_CLAMP_INT((int)st, DRONE_SUB_INTERVAL_MIN, DRONE_SUB_INTERVAL_MAX);
    if (s_d.sub_interval == (int8_t)v) return;
    s_d.sub_interval = (int8_t)v;
    if (s_d.sub_enabled) drone_mark_dirty();
}

void drone_set_gate_len(float frac)
{
    frac = SEQ_CLAMP_F32(frac, DRONE_GATE_MIN, DRONE_GATE_MAX);
    if (fabsf(s_d.gate_len - frac) < 0.001f) return;
    s_d.gate_len = frac;   /* the gate's next edges carry it */
}

void drone_set_swing(uint8_t pct)
{
    s_d.swing_pct = SEQ_CLAMP_U8((int)pct, 0, DRONE_SWING_MAX);
    /* Consumed by the gate and the blip; force a cutoff re-push next frame. */
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

/* EG0 reaches main (at the patch's voice osc) and the sub; EG1 only main,
 * the sub having nothing routed to it. */
static void drone_push_eg0(const seq_env_t *env)
{
    sequencer_core_push_envelope_osc(DRONE_SYNTH_MAIN,
                                     sequencer_core_patch_voice_osc(s_d.patch), env);
    if (s_d.sub_enabled) {
        sequencer_core_push_envelope(DRONE_SYNTH_SUB, env);
    }
}

void drone_get_envelope(seq_env_t *out)
{
    drone_env_get(&s_d.vp, false, out);
}

void drone_set_envelope(const seq_env_t *env)
{
    if (!drone_env_set(&s_d.vp, false, env)) return;
    drone_push_eg0(&s_d.vp.env);
    ESP_LOGI(TAG, "drone env -> A%u D%u S%u%% R%u",
             (unsigned)s_d.vp.env.attack_ms, (unsigned)s_d.vp.env.decay_ms,
             (unsigned)s_d.vp.env.sustain_pct, (unsigned)s_d.vp.env.release_ms);
}

/* ── Editor live-preview (AMY only; drone store + authored flags untouched) ── */

void drone_preview_envelope(const seq_env_t *env)
{
    if (env) drone_push_eg0(env);
}

void drone_preview_envelope2(const seq_env_t *env)
{
    if (env) sequencer_core_push_envelope_eg1(DRONE_SYNTH_MAIN, 0, env);
}

void drone_get_envelope2(seq_env_t *out)
{
    drone_env_get(&s_d.vp, true, out);
}

void drone_set_envelope2(const seq_env_t *env)
{
    if (!drone_env_set(&s_d.vp, true, env)) return;
    sequencer_core_push_envelope_eg1(DRONE_SYNTH_MAIN, 0, &s_d.vp.env1);
    ESP_LOGI(TAG, "drone env1 -> A%u D%u S%u%% R%u",
             (unsigned)s_d.vp.env1.attack_ms, (unsigned)s_d.vp.env1.decay_ms,
             (unsigned)s_d.vp.env1.sustain_pct, (unsigned)s_d.vp.env1.release_ms);
}

/* ── Filter (filter editor) ── */

void drone_get_filter(seq_filter_t *out)
{
    if (out) *out = s_d.vp.filter;
}

void drone_set_filter(const seq_filter_t *f)
{
    if (!f) return;
    s_d.vp.filter = *f;
    s_d.vp.filter_authored = true;
    drone_push_filter(&s_d.vp.filter);
}

void drone_preview_filter(const seq_filter_t *f)
{
    if (f) drone_push_filter(f);
}

/* ── Distortion ── (main only: the sub is a plain sine) */

void drone_get_dist(seq_dist_t *out)
{
    if (out) *out = s_d.vp.dist;
}

void drone_set_dist(const seq_dist_t *d)
{
    if (!d) return;
    s_d.vp.dist = *d;
    voice_dist_clamp(&s_d.vp.dist);
    s_d.vp.dist_authored = true;
    drone_push_dist(&s_d.vp.dist);
}

void drone_preview_dist(const seq_dist_t *d)
{
    if (d) drone_push_dist(d);
}

void drone_reapply_dist(void)
{
    if (s_d.vp.dist_authored) drone_push_dist(&s_d.vp.dist);
}

/* ── LFO ── (main only) */

void drone_get_lfo(seq_lfo_t *out)
{
    if (out) *out = s_d.vp.lfo;
}

void drone_set_lfo(const seq_lfo_t *lfo)
{
    if (!lfo) return;
    s_d.vp.lfo = *lfo;
    s_d.vp.lfo_authored = true;
    drone_apply_lfo();
}

bool drone_lfo_bound(void)
{
#if CONFIG_SEQ_LFO_SHARED
    return s_d_lfo_ref != LFO_BANK_NONE;
#else
    return false;
#endif
}

void drone_get_voice_params(voice_params_t *out)
{
    if (out) *out = s_d.vp;
}

void drone_set_voice_params(const voice_params_t *vp)
{
    if (!vp) return;
    s_d.vp = *vp;
    drone_env_floor_vp(&s_d.vp);
    voice_dist_clamp(&s_d.vp.dist);
    s_d.vp.amp_trim = SEQ_CLAMP_F32(s_d.vp.amp_trim, 0.0f, 1.0f);
    /* The rebuild resets every osc, then re-imposes only the authored
     * groups, rebinds the LFO and re-sends the gate level. */
    drone_mark_rebuild();
}

/* ── Getters ── */
bool           drone_get_enabled(void)      { return s_d.enabled; }
chord_type_t   drone_get_chord(void)        { return s_d.chord; }
uint8_t        drone_get_root_note(void)    { return s_d.root_note; }
drone_follow_t drone_get_follow(void)       { return s_d.follow; }
float          drone_get_amp_peak(void)     { return s_d.amp_peak; }
float          drone_get_amp_duck(void)     { return s_d.amp_duck; }
note_div_t     drone_get_rate(void)         { return s_d.rate; }
uint16_t       drone_get_patch(void)        { return s_d.patch; }
bool           drone_get_sub_enabled(void)  { return s_d.sub_enabled; }
int8_t         drone_get_sub_interval(void) { return s_d.sub_interval; }
float          drone_get_gate_len(void)     { return s_d.gate_len; }
uint8_t        drone_get_swing(void)        { return s_d.swing_pct; }
float          drone_get_blip(void)         { return s_d.blip_depth; }
drone_pattern_t drone_get_pattern(void)     { return s_d.pattern; }

const char *drone_chord_name(chord_type_t chord)
{
    return chord_type_name(chord);
}

uint8_t drone_pattern_mask(drone_pattern_t p)
{
    return (p < DRONE_PAT_COUNT) ? s_pattern_mask[p] : 0u;
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
    s_d.vp.amp_trim = v;   /* the gate's next edges carry s_amp_peak_lin() */
}

float drone_get_amp_trim(void) { return s_d.vp.amp_trim; }
