#pragma once
#include <stdatomic.h>

/* Public and standard headers — order mirrors original sequencer_core.c includes */
#include "sequencer_core.h"
#include "custompatches/bass_presets.h"
#include "custompatches/fm_presets.h"
#include "custompatches/fm_voice.h"
#include "custompatches/additive_presets.h"
#include "custompatches/additive_voice.h"
#include "arp_core.h"
#include "amy.h"
#include "amy_helpers.h"
#include "sequencer.h"
#include "quantizer.h"
#include "seq_defaults.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "diag_heap.h"
#include <string.h>
#include <math.h>
#include "freertos/semphr.h"
#include "amy_fx.h"   /* synth_ui_fx_reassert() - avoids u8g2/display headers */
#include "patch_cycle.h"

/* Private config — included after sdkconfig.h so CONFIG_* are resolved first */
#include "seq_core_config.h"

/* ── Logging tag — static per TU, one copy each, no ODR conflict ────── */
static const char * const TAG = "seq_core";

/* ── External dependency ─────────────────────────────────────────────── */
extern uint32_t sequencer_ticks(void);

/* ── Shared step math (single owners; see architecture/sequencer-core.md §3.1,§3.3) ── */

/* Which step the playhead sits on for `layer` at `ticks`: position within the
 * bar divided by ticks-per-step. Returns 0 for an empty (num_steps == 0) layer;
 * callers that must SKIP such a layer keep their own num_steps guard. */
static inline uint8_t seq_playhead_step(const seq_layer_t *layer, uint32_t ticks)
{
    uint32_t bar_ticks = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
    if (bar_ticks == 0) return 0;
    return (uint8_t)((ticks % bar_ticks) / SEQ_TICKS_PER_STEP);
}

/* Swing: delay ODD 16th-steps by swing_pct% of one step so off-beats land late.
 * Pure function of step index + swing_pct, expressed in ticks, so the schedule
 * stays beat-locked and tempo-independent (same as the drone stutter-grid swing
 * in drone_core.c). Integer math only: shared by the Core-0 plain emit path
 * (sequencer_emit_step) and the render-task trig service.
 * swing_pct is clamped to SEQ_SWING_MAX (<100), so the offset is always a
 * fraction of one step and never crosses into the next. */
static inline uint32_t sequencer_step_swing_offset(const seq_layer_t *layer,
                                                   uint8_t step)
{
    if ((step & 1u) == 0u || layer->swing_pct == 0) return 0;
    return SEQ_SWING_TICKS(layer->swing_pct);
}

/* Note-hold in ticks for the plain (non-subdivided) trig of `step`. Off-beat
 * 8ths are shortened a touch so accented downbeats feel legato while
 * in-between notes detach. Only ever shortens, so the note-off always lands
 * before the next step's note-on. Shared by sequencer_emit_step() and the
 * ratchet n==1 path so the two cannot drift. */
static inline uint16_t seq_step_gate(const seq_layer_t *layer, uint8_t step)
{
    if (layer->type == SEQ_LAYER_DRUM) return SEQ_GATE_DRUM;

    /* Melodic note-hold is a per-layer % of the step (the NoteFX GATE control),
     * rounded pct->ticks; 100% is a full-step legato hold. */
    uint16_t gate = (uint16_t)(((uint32_t)SEQ_TICKS_PER_STEP * layer->gate_pct
                                + 50u) / 100u);
    if ((step % 2) == 1 && gate > 2) {
        gate -= 2;
    }
    if (gate < 1) gate = 1;   /* never zero — the note must sound */
    return gate;
}

/* Row whose entry a per-row default table (sized SEQ_TRACKS_DEFAULT) supplies
 * for `track`: its own, or the last one for the rows above the default
 * count. */
static inline uint8_t seq_default_row(uint8_t track)
{
    return (track < SEQ_TRACKS_DEFAULT) ? track : (uint8_t)(SEQ_TRACKS_DEFAULT - 1);
}

/* ── Private types ───────────────────────────────────────────────────── */

/* Global chord progression (internal representation) */
typedef struct {
    uint8_t      root;           /* chromatic pitch class 0-11 */
    chord_type_t chord_type;
    uint8_t      duration_bars;  /* 1 / 2 / 4 / 8 / 16 */
} chord_prog_entry_t;

typedef struct {
    chord_prog_entry_t entries[CHORD_PROG_MAX_ENTRIES];
    uint8_t            count;
    uint8_t            current;
    uint32_t           entry_start_bar; /* bars_elapsed when current entry began */
    bool               enabled;
    /* Launch quantization: hold deferred chord applies until the next bar line
     * while playing (false = apply immediately). */
    bool               apply_at_bar;
} chord_progression_t;

/* ── Shared state — defined (non-static) in the owning .c file ───────── */

/* Owned by seq_core_state.c. Storage is behind a pointer so
 * CONFIG_SEQ_STATE_IN_PSRAM can place the table in PSRAM: static array when
 * off, allocated in sequencer_core_init() when on. Valid (non-NULL) from
 * sequencer_core_init() onward; before that s_num_layers == 0 keeps every
 * reader out of the table. Task context only - never ISR. */
extern seq_layer_t   *s_layers;
extern uint8_t        s_num_layers;
extern bool           s_playing;
/* Freeze horizon (sequencer_core_freeze_set): set from the UI task, read on
 * the render task by the trig engine. */
extern _Atomic uint32_t s_freeze_tick;
extern _Atomic bool     s_freeze_set;
static inline bool seq_freeze_blocks(uint32_t now_ticks)
{
    if (!atomic_load_explicit(&s_freeze_set, memory_order_acquire)) return false;
    return (int32_t)(now_ticks - atomic_load_explicit(&s_freeze_tick, memory_order_relaxed)) >= 0;
}
/* Pump-task side of sequencer_core_freeze_release_enqueue(). */
void sequencer_core_freeze_release_apply(void);
extern uint8_t        s_next_melodic_synth;
/* Global melodic patch DEFAULT, NOT an authoritative global. The patch setters
 * (layer and per-track) write it as a side-effect so the display fallback
 * tracks the last-touched layer. Readers must treat s_layers[i].track_patch[]
 * as the per-row truth - this is only the seed for new layers plus the display
 * fallback. */
extern uint16_t       s_melodic_patch;
extern volatile bool  s_layers_mutating;   /* guards delete_layer's compaction vs. the tick */

/* Owned by seq_core_engine.c */
extern uint8_t        s_cached_step[];
extern uint8_t        s_track_source_note[][SEQ_TRACKS];
extern uint32_t       s_bar_baseline;
/* Last PLAIN (non-sentinel) source note per track: the fallback when the chord
 * slot a track references is deleted. Runtime only; 0 = unset, in which case
 * the fallback is C4. */
extern uint8_t        s_track_prev_plain[][SEQ_TRACKS];

/* Owned by seq_core_synth.c */
extern seq_drum_engine_t s_drum_engine;
/* Boot-default drum pitch for a track: the boot bank's (808 ROM) ear-tuned
 * notes[]. Single source with the bank selector's re-seed path. */
uint8_t sequencer_drum_default_note(uint8_t track);
/* Per-track voice count last pushed to each melodic row's synth; shifted by
 * delete_layer alongside s_track_source_note. */
extern uint8_t        s_voices_applied[][SEQ_TRACKS];

/* Owned by seq_core_tempo.c */
extern uint16_t       s_bpm;
extern quantizer_state_t s_quantizer;

/* Owned by seq_core_editors.c */
extern float          s_lfo_phase[][SEQ_TRACKS];
extern float          s_lfo_hz[][SEQ_TRACKS];
extern float          s_lfo_rnd[][SEQ_TRACKS];
extern uint32_t       s_lfo_rng_state;

/* Owned by seq_core_progression.c */
extern chord_progression_t s_prog;
extern volatile bool  s_prog_apply_pending;
extern volatile bool  s_prog_apply_immediate;

/* Owned by seq_core_trig.c */
extern uint32_t s_layer_loop_count[];              /* MAX_LAYERS   */
extern uint8_t  s_layer_last_step[];                /* MAX_LAYERS, 0xFF = unseen */
extern bool     s_track_last_played[][SEQ_TRACKS];  /* MAX_LAYERS x SEQ_TRACKS */

/* ── Private function declarations — all defined non-static in owning .c ─ */

/* From seq_core_engine.c */
void     sequencer_emit_step(uint8_t layer_idx, uint8_t track, uint8_t step);
void     sequencer_emit_clear_tag(uint32_t tag);
void     sequencer_resync_layer(uint8_t layer_idx);
void     sequencer_clear_layer_tags(uint8_t layer_idx);
/* Cancel one track's plain step pairs and its preview pairs (plain and chord
 * tones). The trig engine's one-shots: sequencer_core_trig_clear_track(). */
void     sequencer_clear_track_tags(uint8_t layer_idx, uint8_t track);
void     sequencer_refresh_melodic_layers(bool preview);
uint32_t sequencer_bars_elapsed(void);
/* Bars elapsed as they will read ahead_ticks from now. */
uint32_t sequencer_bars_elapsed_ahead(uint32_t ahead_ticks);

/* From seq_core_engine.c - chord expansion shared with seq_core_trig.c.
 * seq_track_fire_notes resolves what a stored (possibly sentinel) note fires:
 * 1 plain note, or n transposed and clamped chord tones. Returns the tone
 * count; 0 = undefined chord slot, fire nothing. sequencer_chord_transpose is
 * the progression offset: the layer chord root relative to entry 0's root while
 * the progression is enabled, else 0. The _root variants take the chord root
 * explicitly instead of the layer's live one (a decorated step resolved ahead
 * of a chord change); same obligations and guarantees otherwise. */
uint8_t seq_track_fire_notes(const seq_layer_t *layer, uint8_t stored_note,
                             uint8_t out[SEQ_CHORD_MAX_NOTES]);
uint8_t seq_track_fire_notes_root(const seq_layer_t *layer, uint8_t stored_note,
                                  uint8_t root, uint8_t out[SEQ_CHORD_MAX_NOTES]);
int     sequencer_chord_transpose(const seq_layer_t *layer);
int     sequencer_chord_transpose_root(const seq_layer_t *layer, uint8_t root);

/* From seq_core_synth.c */
void      sequencer_configure_synth(uint8_t layer_idx);
/* sequencer_configure_synth for the first `rows` rows instead of num_tracks:
 * builds a row before num_tracks exposes it to the tick. */
void      sequencer_configure_synth_rows(uint8_t layer_idx, uint8_t rows);
/* Drum row `dst` takes row `src`'s PCM preset and mode (core-side store). */
void      seq_drum_copy_row_sound(uint8_t layer_idx, uint8_t dst, uint8_t src);
void      sequencer_kill_synth_voices(uint8_t synth_id);
/* Free a synth slot's voices and oscs in AMY (num_voices = 0); the slot is
 * rebuilt by the next configure that covers it. */
void      sequencer_release_synth(uint8_t synth_id);
/* Chord-aware per-track voice count: layer->num_voices, widened to the chord
 * tone count while the track's base note is a chord sentinel. Every melodic
 * patch-apply site must consult it so the widened count survives reloads. */
uint8_t   seq_track_num_voices(const seq_layer_t *layer, uint8_t track);
/* True when any melodic track's needed voice count differs from what its
 * synth was last configured with (chord assigned/removed or slot resized). */
bool      sequencer_layer_voices_stale(uint8_t layer_idx);
/* Clear schedule -> configure synth -> resync: the patch-cycling path, reused
 * for chord voice-count changes under the same ringing discipline. */
void      sequencer_reconfigure_layer_paused(uint8_t layer_idx);
/* Push the layer's glide (portamento_ms) to every row synth. AMY clears
 * portamento_alpha on osc reset, so reassert on every voice rebuild. */
void      sequencer_core_push_melodic_portamento(uint8_t layer_idx);
seq_env_t *seq_layer_env(uint8_t layer_idx, uint8_t track);
seq_env_t *seq_layer_env1(uint8_t layer_idx, uint8_t track);
/* The voice block (layer, track) currently reads AND edits: vp_layer when the
 * row's vp_src is LAYER, else its own vp[track]. Every push, service and
 * editor path resolves through here; only amp_trim (always the row's own),
 * the drum bank seeding (drum layers are always TRACK) and the project store
 * touch the blocks directly. Out-of-range indices clamp to 0. */
voice_params_t *seq_track_vp(uint8_t layer_idx, uint8_t track);
typedef struct {
    uint8_t carrier;     /* LFO carrier osc; wobble = carrier + 1 */
    uint8_t pitch_mask;  /* oscs whose freq/duty MOD rails the LFO drives */
    uint8_t voice_mask;  /* oscs whose filter/amp/pan/dist MOD rails the LFO drives */
    uint8_t heads_mask;  /* SILENT-head oscs; 0 = no heads: filter pushes
                            broadcast and dist pushes address osc 0 as before */
} seq_voice_layout_t;

/* Native-LFO / push layout for one melodic track: the patch-only predicate
 * (sequencer_core_lfo_native_layout) lifted over the row's AS-BUILT unison
 * fan. Every per-track osc-addressed consumer (LFO apply, BPM refresh, filter
 * and dist pushes) resolves through this. Returns false for non-native rows. */
bool seq_track_voice_layout(uint8_t layer_idx, uint8_t track,
                            seq_voice_layout_t *out);
/* Effective unison copy count the track's synth is built with (1 = none). */
uint8_t seq_track_unison_copies(uint8_t layer_idx, uint8_t track);

/* From seq_core_editors.c */
void sequencer_configure_melodic_envelope_track(uint8_t layer_idx, uint8_t track);
void sequencer_configure_melodic_envelope1_track(uint8_t layer_idx, uint8_t track);
void sequencer_configure_melodic_filter_track(uint8_t layer_idx, uint8_t track);
void sequencer_configure_melodic_dist_track(uint8_t layer_idx, uint8_t track);
void sequencer_configure_melodic_lfo(uint8_t layer_idx, uint8_t rows);
void melodic_lfo_refresh_native_freq(void);

/* From seq_core_tempo.c */
uint16_t sequencer_clamp_bpm(uint16_t b);
void     sequencer_push_tempo(uint16_t b);
/* lfo_rate_to_hz is already declared in the public sequencer_core.h */

/* LFO Hz for the 20 Hz software fallback stepper: lfo_rate_to_hz capped to
 * SEQ_LFO_SW_MAX_HZ, since the fast end of the rate range needs >= 4 stepper
 * samples per LFO cycle. */
static inline float seq_lfo_sw_hz(lfo_rate_t rate, uint16_t bpm)
{
    float hz = lfo_rate_to_hz(rate, bpm);
    return (hz > SEQ_LFO_SW_MAX_HZ) ? SEQ_LFO_SW_MAX_HZ : hz;
}
float    lfo_next_rand(void);
void     lfo_push_target_neutral(uint8_t synth_id, lfo_target_t target);

/* From seq_core_progression.c */
void chord_progression_apply_current(void);
/* True when a published chord change is due at or before fire_tick; then
 * *root and *type are the chord that will be sounding at fire_tick. The record
 * is published by sequencer_core_progression_service() on synth_ui_task each
 * tick while the progression is enabled, non-empty and playing, and is invalid
 * otherwise. Pump-task safe (seqlock read; a torn read reads as "no pending
 * change" and leaves the outputs unwritten), no side effects. */
bool sequencer_core_progression_chord_at(uint32_t fire_tick, uint8_t *root,
                                         chord_type_t *type);

/* From seq_core_engine.c - shared with seq_core_trig.c so ratchet sub-hits use
 * the same accent/jitter velocity curve as the plain periodic path. */
float sequencer_step_velocity(const seq_layer_t *layer, uint8_t track, uint8_t step);

/* From seq_core_engine.c - shared with seq_core_trig.c so the decorated-step
 * ratchet path respects mute/solo exactly as the plain path does. */
bool sequencer_track_audible(const seq_layer_t *layer, uint8_t track);

/* From seq_core_engine.c - shared with seq_core_trig.c so a per-step transform
 * re-snaps through the same chord/scale quantizer the plain per-track resolve
 * uses. resolve applies the snap; clamp only bounds the note (used when
 * step_quant_bypass is set). */
uint8_t sequencer_clamp_layer_note(const seq_layer_t *layer, uint8_t note);
uint8_t sequencer_resolve_track_note(const seq_layer_t *layer, uint8_t source_note);
/* sequencer_resolve_track_note with the chord given explicitly: in chord mode
 * the snap uses (root, chord_type) instead of the layer's live chord;
 * everything else resolves identically. */
uint8_t sequencer_resolve_track_note_chord(const seq_layer_t *layer,
                                           uint8_t source_note, uint8_t root,
                                           chord_type_t chord_type);
/* Resolve every row of a layer from its source notes (s_track_source_note).
 * For a melodic layer in chord mode, the plain-note rows are voiced together
 * through quantizer_voice_chord with (root, type) and clamped; chord preset
 * rows pass through untouched and take no part. Other layers resolve each row
 * as sequencer_resolve_track_note does, ignoring root/type. Obligations:
 * layer_idx < s_num_layers. Guarantees: out[t] written for t < num_tracks,
 * rows above left untouched; no state written, no AMY events. Called from
 * synth_ui_task (row refresh) and the AMY ingest pump task (decorated steps);
 * never from the render task (the voicing search is up to 3125 assignments). */
void seq_resolve_layer_rows(uint8_t layer_idx, uint8_t root,
                            chord_type_t type, uint8_t out[SEQ_TRACKS]);

/* From seq_core_trig.c - per-step probability/ratchet/conditional-trig engine.
 * sequencer_emit_step() consults sequencer_core_step_is_decorated() to decide
 * whether a step keeps the plain always-on periodic AMY tag or is left cleared
 * for the trig engine to one-shot schedule. */
bool sequencer_core_step_is_decorated(const seq_layer_t *layer, uint8_t track, uint8_t step);
void sequencer_core_trig_reset(uint8_t layer_idx);   /* called on play-start, one layer   */
void sequencer_core_trig_clear_all(uint8_t layer_idx); /* called on pause, one layer       */
void sequencer_core_trig_reset_all(void);            /* called on layer add/delete        */
/* Clear one track's pending chord-tone one-shot tags. Called when a track's
 * resolved note moves away from a chord, so no scheduled extra tone survives
 * the transition. */
void sequencer_core_trig_clear_track_chord(uint8_t layer_idx, uint8_t track);
/* Clear one track's pending ratchet and chord-tone one-shot tags. */
void sequencer_core_trig_clear_track(uint8_t layer_idx, uint8_t track);
/* One-shot schedule step_ratchet sub-hits for a decorated fire: velocity,
 * pitch/chord/transform resolve, tag emission via amy_helpers_note_send().
 * grid_tick is the step's absolute grid boundary; sub-hit k lands at
 * grid_tick + 1 + swing + nudge + k*sub_ticks, the same law as the plain path.
 * MUST NOT be called from the render task - see seq_trig_pump.c. */
void trig_schedule_ratchets(uint8_t layer_idx, const seq_layer_t *layer,
                            uint8_t track, uint8_t step, uint32_t grid_tick);

/* From seq_trig_pump.c - moves trig_schedule_ratchets() off the render task.
 * sequencer_core_service_tick() runs on amy_usb_render_task and must never
 * block or call into the amy_helpers ingress seam directly (that's what
 * trips amy_helpers_event_begin's render-task assert); it instead hands a
 * tiny job descriptor to the AMY ingest pump's urgent source via a
 * non-blocking enqueue + doorbell. All decision-making (edge detection,
 * condition eval, probability roll, the trig RNG) stays on the render task
 * unchanged - only the AMY-facing emission tail moves. */
void sequencer_core_trig_pump_init(void);
void sequencer_core_trig_enqueue(uint8_t layer_idx, uint8_t track, uint8_t step,
                                 uint32_t grid_tick);
