/* drone_std_core.c - normal (free-running) drone synth.
 *
 * The stutter drone's sibling: chord + sine sub voicing on its own AMY slots,
 * sustained output, and the standard per-voice toolset (free filter, LFO,
 * graph-editor envelopes, distortion) instead of the stutter machinery.
 * Design split: drone_std_core.h; shared concurrency rationale: drone_core.c.
 *
 * AMY access rules as in drone_core.c; callers are FreeRTOS tasks, never
 * ISRs. */

#include "custompatches/drone_std_core.h"
#include "custompatches/drone_common.h"
#include "synth_ui.h"          /* seq_get_bpm() */
#include "sequencer_core.h"    /* shared apply path, envelope/filter pushes */
#include "seq_clamp.h"
#include "amy.h"
#include "amy_helpers.h"
#include "voice_config.h"
#include "lfo_bank.h"          /* shared LFO synths (lfo_bank_bind) */
#include "lfo_targets.h"       /* lfo_targets_of_patch() */
#include "custompatches/bass_presets.h"   /* bass_preset_osc_level() */
#include "custompatches/fm_voice.h"       /* s_fm_voice, fm_voice_push */
#include "custompatches/additive_voice.h" /* s_additive_voice, additive_voice_push_live */
#include "sdkconfig.h"
#include "esp_log.h"

#include <math.h>
#include <string.h>

static const char *TAG = "drone_std";

/* ── Synth slots: DRONE_STD_SYNTH_MAIN/SUB from synth_slots.h's static pool ── */
#include "synth_slots.h"
#define DRONE_STD_SUB_VOICES  1

/* ── State ── */
typedef struct {
    bool           enabled;
    bool           solo_muted;   /* silenced because a sequencer track is soloed;
                                    separate from `enabled` so the user's own
                                    on/off survives a solo round-trip        */
    bool           paused;       /* silenced by a stopped transport, likewise */
    chord_type_t   chord;
    uint8_t        root_note;    /* DRONE_ROOT range shared with stutter   */
    drone_follow_t follow;       /* chord progression follow mode          */
    drone_notes_t  notes;        /* sounding set (drone_common.h)          */
    float          level;       /* 0..1 linear output level (amp CONST)   */
    uint16_t       patch;        /* SEQ_PATCH_* id, drone_patch_excluded() domain */
    uint8_t        voices;       /* main voices as built: the chord is
                                    truncated to this many tones          */
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

static inline float drone_std_level_lin(void)
{
    float v = s_ds.level * s_ds.vp.amp_trim;
    return SEQ_CLAMP_F32(v, 0.0f, 1.0f);
}

static inline bool drone_std_lfo_on(void)
{
    return s_ds.vp.lfo_authored && s_ds.vp.lfo.enabled;
}

#if CONFIG_SEQ_LFO_SHARED
/* The shared LFO synth the main synth holds (lfo_bank.h); LFO_BANK_NONE
 * while it has no LFO. Kept outside s_ds, which init clears. The sub holds
 * none. s_ds_lfo2_ref is LFO2's (mod slot 0). */
static uint8_t s_ds_lfo_ref = LFO_BANK_NONE;
static uint8_t s_ds_lfo2_ref = LFO_BANK_NONE;

/* The oscs the main synth's shared LFO synths reach: a wave layout's coupled
 * oscs or a patch string's lfo_targets map. False for an unmapped patch. */
static bool drone_std_lfo_target_map(lfo_target_map_t *map)
{
    uint8_t coupled;
    if (sequencer_core_lfo_wave_layout(s_ds.patch, &coupled)) {
        *map = (lfo_target_map_t){ .voice_mask = coupled, .pitch_mask = coupled,
                                   .scan_mask  = coupled };
        return true;
    }
    return lfo_targets_of_patch(s_ds.patch, map);
}
#endif

/* Apply the drone's LFO to the main synth as currently built, as
 * live_play.c's live_apply_lfo does: with CONFIG_SEQ_LFO_SHARED a shared LFO
 * synth over the wave layout's coupled oscs or a patch string's lfo_targets
 * map; when the bank is full, the LFO is off or the patch has no map, the
 * main has no LFO (drones have no stepper). Without it the native carriers
 * on a wave layout (NULL clears: re-sending the same voice count does not
 * reset the osc pool, so a stale COEF_MOD would keep modulating -
 * voice_config.h); a patch string has no LFO. */
static void drone_std_apply_lfo(void)
{
    const seq_lfo_t *lfo = drone_std_lfo_on() ? &s_ds.vp.lfo : NULL;
    uint16_t bpm = seq_get_bpm();
#if CONFIG_SEQ_LFO_SHARED
    lfo_target_map_t map;
    if (drone_std_lfo_target_map(&map))
        (void)lfo_bank_bind(DRONE_STD_SYNTH_MAIN, VOICE_LFO_SLOT_LFO1, &map, lfo, bpm, &s_ds_lfo_ref);
    else
        lfo_bank_unbind(&s_ds_lfo_ref);   /* the patch re-apply unwired it */
#else
    uint8_t carrier, coupled;
    if (!sequencer_core_lfo_native_layout(s_ds.patch, &carrier, &coupled)) return;
    voice_apply_native_lfo_topo(DRONE_STD_SYNTH_MAIN, lfo, bpm, carrier,
                                coupled, coupled);
#endif
}

bool drone_std_lfo_bound(void)
{
#if CONFIG_SEQ_LFO_SHARED
    return s_ds_lfo_ref != LFO_BANK_NONE;
#else
    return false;
#endif
}

/* Send `lfo` (NULL = unwire) to the main synth's mod slot 0 where the patch
 * has a target map; drop LFO2's reference otherwise. */
static void drone_std_wire_lfo2(const seq_lfo_t *lfo)
{
#if CONFIG_SEQ_LFO_SHARED
    lfo_target_map_t map;
    if (drone_std_lfo_target_map(&map))
        (void)lfo_bank_bind(DRONE_STD_SYNTH_MAIN, VOICE_LFO_SLOT_LFO2, &map, lfo,
                            seq_get_bpm(), &s_ds_lfo2_ref);
    else
        lfo_bank_unbind(&s_ds_lfo2_ref);
#else
    (void)lfo;
#endif
}

/* LFO2 by the slot-0 rule (ENGINE-SEMANTICS.md, "LFO: native carrier vs
 * software stepper"): open, wired or unwired with the stored LFO2; collapsed,
 * no send, so a Juno/DX7 patch keeps its own LFO. Main only. */
static void drone_std_apply_lfo2(void)
{
    if (s_ds.vp.lfo2_authored) {
        drone_std_wire_lfo2(&s_ds.vp.lfo2);
        return;
    }
#if CONFIG_SEQ_LFO_SHARED
    lfo_bank_unbind(&s_ds_lfo2_ref);
#endif
}

bool drone_std_lfo2_bound(void)
{
#if CONFIG_SEQ_LFO_SHARED
    return s_ds_lfo2_ref != LFO_BANK_NONE;
#else
    return false;
#endif
}

/* Push a filter to the main synth plus its envelope routing matrix (all
 * eight slots once the filter block is authored), the SCAN rails on a
 * wavetable, and the EG1 breakpoints whenever an EG1 rail is live
 * (sequencer_core_push_envelope_eg1()). Mirrors live_play.c's
 * live_apply_filter. The sub has no filter. */
static void drone_std_push_filter(const seq_filter_t *f)
{
    sequencer_core_push_filter(DRONE_STD_SYNTH_MAIN, f, false);
    sequencer_core_push_eg_depths(DRONE_STD_SYNTH_MAIN,
                                  sequencer_core_patch_voice_osc(s_ds.patch), f,
                                  s_ds.vp.filter_authored);
    if (sequencer_core_is_wavetable_patch(s_ds.patch)) {
        uint8_t coupled;
        uint8_t mask = sequencer_core_lfo_wave_layout(s_ds.patch, &coupled)
                     ? coupled : 0x01u;
        sequencer_core_push_eg_scan(DRONE_STD_SYNTH_MAIN, mask, f,
                                    s_ds.vp.filter_authored);
    }
    if (seq_filter_eg1_live(f)) {
        sequencer_core_push_envelope_eg1(DRONE_STD_SYNTH_MAIN, 0, &s_ds.vp.env1);
    }
}

/* Level x trim as amp CONST on a synth built from `patch`. A wave patch: osc
 * 0, with velocity off, where the shared wave build leaves CONST 1 and VEL 1;
 * under the dB combine model a plain CONST level is exactly level_lin
 * (EG0 = 1). Any other patch: the oscs of its lfo_targets map's voice mask
 * (Juno: the SILENT head; DX7/FM: the ALGO control osc; additive: the
 * partials' parent; bass: both oscs, each scaled by the preset's own
 * balance), CONST only, replacing the head's own level and leaving the
 * patch's VEL and EG0 coefs alone. A patch with no map (the piano) keeps its
 * own level. */
static void drone_std_push_level(uint8_t synth, uint16_t patch)
{
    float level = drone_std_level_lin();
    if (level < DRONE_AMP_MUTE) level = DRONE_AMP_MUTE;   /* 0 would read as full */
    if (sequencer_core_is_wave_patch(patch)) {
        amy_event *e = amy_helpers_event_begin();
        e->synth                 = synth;
        e->osc                   = 0;
        e->amp_coefs[COEF_CONST] = level;
        e->amp_coefs[COEF_VEL]   = 0.0f;
        amy_helpers_event_send(e);
        return;
    }
    lfo_target_map_t map;
    if (!lfo_targets_of_patch(patch, &map)) return;
    for (uint8_t o = 0; o < 16u; o++) {
        if (!(map.voice_mask & (1u << o))) continue;
        /* A bass preset keeps its own balance between its two oscs. */
        float l = level * bass_preset_osc_level(patch, o);
        amy_event *e = amy_helpers_event_begin();
        e->synth                 = synth;
        e->osc                   = o;
        e->amp_coefs[COEF_CONST] = (l < DRONE_AMP_MUTE) ? DRONE_AMP_MUTE : l;
        amy_helpers_event_send(e);
    }
}

/* (Re)build both synths. Main takes the patch through the shared apply path
 * (sequencer_core_configure_synth_slot) at as many voices as the patch's osc
 * budget allows, up to DRONE_CHORD_MAX_NOTES, so a chord change re-voices
 * without a rebuild; then the authored groups go back on as
 * live_apply_authored() puts them. The sub is a plain one-voice sine with the
 * level and EG0 only. */
static void drone_std_rebuild(void)
{
    uint16_t patch = s_ds.patch;
    s_ds.voices = drone_voices_for(patch);
    sequencer_core_configure_synth_slot(DRONE_STD_SYNTH_MAIN, patch, s_ds.voices, NULL);
    drone_std_push_level(DRONE_STD_SYNTH_MAIN, patch);
    if (s_ds.vp.env_authored)
        sequencer_core_push_envelope_osc(DRONE_STD_SYNTH_MAIN,
                                         sequencer_core_patch_voice_osc(patch),
                                         &s_ds.vp.env);
    if (s_ds.vp.env1_authored)
        sequencer_core_push_envelope_eg1(DRONE_STD_SYNTH_MAIN, 0, &s_ds.vp.env1);
    if (s_ds.vp.filter_authored) drone_std_push_filter(&s_ds.vp.filter);
    drone_std_reapply_dist();
    drone_std_apply_lfo();
    drone_std_apply_lfo2();

    if (s_ds.sub_enabled) {
        sequencer_core_configure_synth_slot(DRONE_STD_SYNTH_SUB, SEQ_PATCH_SINE,
                                            DRONE_STD_SUB_VOICES, NULL);
        drone_std_push_level(DRONE_STD_SYNTH_SUB, SEQ_PATCH_SINE);
        if (s_ds.vp.env_authored)
            sequencer_core_push_envelope(DRONE_STD_SYNTH_SUB, &s_ds.vp.env);
    }
    s_ds.last_lfo_bpm = (float)seq_get_bpm();
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
    if (eff < DRONE_ROOT_MIN) eff += 12;
    if (eff > DRONE_ROOT_MAX) eff -= 12;
    *root = (uint8_t)eff;
    if (s_ds.follow == DRONE_FOLLOW_CHORD) *chord = type;
}

/* Bring the sounding set to the current voicing (drone_apply_enabled): the
 * chord tones that fit the main's built voices, and the sub note. */
static void drone_std_reconcile(uint32_t land_ms)
{
    drone_voicing_t v = {
        .main_synth   = DRONE_STD_SYNTH_MAIN,
        .sub_synth    = DRONE_STD_SYNTH_SUB,
        .sounding     = drone_std_sounding(),
        .voices       = s_ds.voices,
        .sub          = s_ds.sub_enabled,
        .sub_interval = s_ds.sub_interval,
    };
    if (v.sounding) drone_std_effective(&v.root, &v.chord);
    drone_apply_enabled(&s_ds.notes, &v, land_ms);
}

/* Note-off the whole sounding set and forget it (ahead of a rebuild). */
static void drone_std_release_all(uint32_t land_ms)
{
    drone_release_all(&s_ds.notes, DRONE_STD_SYNTH_MAIN, DRONE_STD_SYNTH_SUB, land_ms);
}

/* amy_sysclock() ms at which a progression change due on land_tick sounds;
 * now when the tick is 0 or already reached. */
static uint32_t drone_std_land_ms(uint32_t land_tick)
{
    return sequencer_core_tick_to_amy_ms(land_tick);
}

/* ── Public API ── */

void drone_std_core_init(void)
{
    amy_helpers_init();

    memset(&s_ds, 0, sizeof(s_ds));
    voice_params_init_defaults(&s_ds.vp);
    s_ds.enabled      = false;
    s_ds.chord        = CHORD_OFF;   /* root note only until a chord is chosen */
    s_ds.root_note    = 45;          /* A2, same default as the stutter drone */
    s_ds.follow       = DRONE_FOLLOW_OFF;
    drone_notes_init(&s_ds.notes);
    s_ds.level        = 0.5f;
    s_ds.patch        = SEQ_PATCH_SAW_DOWN;
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
    s_ds.vp.lfo.rate    = NOTE_DIV_1_1;
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

void drone_std_core_fm_voice_changed(uint8_t what)
{
#if CONFIG_SYNTH_CUSTOM_FM
    /* Operators and routing only: osc 0's amp CONST (the gate / Level) is
     * untouched. */
    if (s_ds.patch == SEQ_PATCH_FM_CUSTOM) fm_voice_push(DRONE_STD_SYNTH_MAIN, &s_fm_voice, what);
#else
    (void)what;
#endif
}

void drone_std_core_additive_voice_changed(void)
{
#if CONFIG_SYNTH_ADDITIVE
    /* Partial ratios and levels only: the parent's amp CONST (the gate /
     * Level) is untouched. */
    if (s_ds.patch == SEQ_PATCH_ADDITIVE_CUSTOM)
        additive_voice_push_live(DRONE_STD_SYNTH_MAIN, &s_additive_voice);
#endif
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
    /* With CONFIG_SEQ_LFO_SHARED: no per-voice carriers; lfo_bank_set_bpm()
     * retunes the shared synths. */
#if !CONFIG_SEQ_LFO_SHARED
    if (!drone_std_lfo_on()) return;
    float bpm = (float)seq_get_bpm();
    if (fabsf(bpm - s_ds.last_lfo_bpm) < 0.5f) return;
    s_ds.last_lfo_bpm = bpm;
    drone_std_apply_lfo();   /* no-op on a patch string */
#endif
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

void drone_std_set_chord(chord_type_t chord)
{
    if (chord >= CHORD_TYPE_COUNT) return;
    if (s_ds.chord == chord) return;
    s_ds.chord = chord;
    drone_std_mark_dirty();
}

void drone_std_set_root_note(uint8_t note)
{
    uint8_t clamped = (uint8_t)SEQ_CLAMP_INT((int)note, DRONE_ROOT_MIN, DRONE_ROOT_MAX);
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
    drone_std_push_level(DRONE_STD_SYNTH_MAIN, s_ds.patch);
    if (s_ds.sub_enabled) drone_std_push_level(DRONE_STD_SYNTH_SUB, SEQ_PATCH_SINE);
}

void drone_std_set_patch(uint16_t patch)
{
    /* Shared exclusion predicate with the stutter drone (same voice model). */
    if (drone_patch_excluded(patch)) patch = SEQ_PATCH_TRIANGLE;
    if (s_ds.patch == patch) return;
    s_ds.patch = patch;
    drone_std_mark_rebuild();
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
    int v = SEQ_CLAMP_INT((int)st, DRONE_SUB_INTERVAL_MIN, DRONE_SUB_INTERVAL_MAX);
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

/* EG0 reaches main (at the patch's voice osc) and the sub; EG1 only main,
 * the sub having nothing routed to it. */
static void drone_std_push_eg0(const seq_env_t *env)
{
    sequencer_core_push_envelope_osc(DRONE_STD_SYNTH_MAIN,
                                     sequencer_core_patch_voice_osc(s_ds.patch), env);
    if (s_ds.sub_enabled) {
        sequencer_core_push_envelope(DRONE_STD_SYNTH_SUB, env);
    }
}

void drone_std_get_envelope(seq_env_t *out)
{
    drone_env_get(&s_ds.vp, false, out);
}

void drone_std_set_envelope(const seq_env_t *env)
{
    if (drone_env_set(&s_ds.vp, false, env)) drone_std_push_eg0(&s_ds.vp.env);
}

void drone_std_get_envelope2(seq_env_t *out)
{
    drone_env_get(&s_ds.vp, true, out);
}

void drone_std_set_envelope2(const seq_env_t *env)
{
    if (drone_env_set(&s_ds.vp, true, env))
        sequencer_core_push_envelope_eg1(DRONE_STD_SYNTH_MAIN, 0, &s_ds.vp.env1);
}

void drone_std_preview_envelope(const seq_env_t *env)
{
    if (env) drone_std_push_eg0(env);
}

void drone_std_preview_envelope2(const seq_env_t *env)
{
    if (env) sequencer_core_push_envelope_eg1(DRONE_STD_SYNTH_MAIN, 0, env);
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
    drone_std_push_filter(&s_ds.vp.filter);
}

void drone_std_preview_filter(const seq_filter_t *f)
{
    if (f) drone_std_push_filter(f);
}

/* ── Distortion ── (main only: the sub is a plain sine) */
void drone_std_get_dist(seq_dist_t *out)
{
    if (out) *out = s_ds.vp.dist;
}

static void drone_std_push_dist(const seq_dist_t *d)
{
    voice_apply_dist(DRONE_STD_SYNTH_MAIN, d);
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

/* ── Free LFO ── (main only) */

void drone_std_get_lfo(seq_lfo_t *out)
{
    if (out) *out = s_ds.vp.lfo;
}

void drone_std_set_lfo(const seq_lfo_t *lfo)
{
    if (!lfo) return;
    s_ds.vp.lfo = *lfo;
    s_ds.vp.lfo_authored = true;
    /* One LFO per target (seq_model.h): LFO1 keeps a shared one. */
    uint8_t shared = s_ds.vp.lfo2.targets & s_ds.vp.lfo.targets;
    s_ds.vp.lfo2.targets &= (uint8_t)~shared;
    if (shared) drone_std_apply_lfo2();
    drone_std_apply_lfo();
}

bool drone_std_get_lfo2(seq_lfo_t *out)
{
    if (out) *out = s_ds.vp.lfo2;
    return s_ds.vp.lfo2_authored;
}

void drone_std_set_lfo2(const seq_lfo_t *lfo, bool open)
{
    if (!lfo) return;
    bool collapse = s_ds.vp.lfo2_authored && !open;
    s_ds.vp.lfo2 = *lfo;
    s_ds.vp.lfo2.targets &= (uint8_t)~s_ds.vp.lfo.targets;   /* one LFO per target */
    if (collapse && lfo_targets_patch_owns_slot0(s_ds.patch)) {
        /* Only a patch re-apply brings the patch's own LFO back: the rebuild
         * drain (drone_std_core_service()), which re-voices the chord. */
        s_ds.vp.lfo2_authored = false;
        drone_std_mark_rebuild();
        return;
    }
    if (collapse) drone_std_wire_lfo2(NULL);   /* plain unwire: the patch leaves slot 0 free */
    s_ds.vp.lfo2_authored = open;
    drone_std_apply_lfo2();
}

void drone_std_get_voice_params(voice_params_t *out)
{
    if (out) *out = s_ds.vp;
}

void drone_std_set_voice_params(const voice_params_t *vp)
{
    if (!vp) return;
    s_ds.vp = *vp;
    s_ds.vp.lfo2.targets &= (uint8_t)~s_ds.vp.lfo.targets;   /* one LFO per target */
    drone_env_floor_vp(&s_ds.vp);
    voice_dist_clamp(&s_ds.vp.dist);
    s_ds.vp.amp_trim = SEQ_CLAMP_F32(s_ds.vp.amp_trim, 0.0f, 1.0f);
    /* The rebuild resets every osc, then re-imposes only the authored
     * groups and rebinds the LFO. */
    drone_std_mark_rebuild();
}

/* ── Getters ── */
bool           drone_std_get_enabled(void)      { return s_ds.enabled; }
chord_type_t   drone_std_get_chord(void)        { return s_ds.chord; }
uint8_t        drone_std_get_root_note(void)    { return s_ds.root_note; }
drone_follow_t drone_std_get_follow(void)       { return s_ds.follow; }
float          drone_std_get_level(void)        { return s_ds.level; }
uint16_t       drone_std_get_patch(void)        { return s_ds.patch; }
bool           drone_std_get_sub_enabled(void)  { return s_ds.sub_enabled; }
int8_t         drone_std_get_sub_interval(void) { return s_ds.sub_interval; }
float          drone_std_get_amp_trim(void)     { return s_ds.vp.amp_trim; }
