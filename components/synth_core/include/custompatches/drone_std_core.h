#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "seq_model.h"     /* seq_env_t, seq_filter_t, seq_lfo_t */
#include "chord_types.h"   /* chord_type_t */
#include "custompatches/drone_core.h"   /* drone_source_t, drone_follow_t, drone_patch_excluded() */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Normal (free-running) drone ──────────────────────────────────────────
 * The stutter drone's sibling: same chord voicing and mono sub, but plain
 * sustained output - no stutter gate, no peak/duck amp coupling, no
 * pattern/blip/swing service, no tempo-locked filter sweep.
 *
 * In their place it exposes the standard per-voice toolset:
 *   - a free filter (seq_filter_t) edited in the filter graph editor and
 *     pushed via sequencer_core_push_filter;
 *   - a free AMY-native LFO (seq_lfo_t) via voice_apply_native_lfo, WAVE mode
 *     only - PATCH-mode instruments own their osc topology;
 *   - the shared ADSR/EG1 graph editor storage.
 *
 * Owns AMY synth slots 4/5 (DRONE_STD_SYNTH_MAIN/_SUB, just above the stutter
 * drone's 2/3) so both drones can sound together; the slot map is in
 * synth_slots.h. AMY access goes through amy_helpers.h. */

/* ── Lifecycle ── */
void drone_std_core_init(void);

/* Per-UI-frame service: drains the setters' pending rebuild and note changes
 * (the only place outside drone_std_core_follow_changed() that sends drone
 * notes). Call once per UI frame from synth_ui_task. Cheap no-op when nothing
 * changed. */
void drone_std_core_service(void);

/* Re-voice the drone after the applied progression chord changed. Same
 * contract as drone_core_follow_changed(): synth_ui_task only, called by the
 * progression service; no-op under DRONE_FOLLOW_OFF. */
void drone_std_core_follow_changed(void);

/* Re-push the native LFO carrier frequency after a BPM change (called from
 * sequencer_core_set_bpm, mirroring arp_core_refresh_lfo_freq). */
void drone_std_core_refresh_lfo_freq(void);

/* ── Parameter setters ──
 * As for the stutter drone (drone_core.h): setters that change which notes
 * sound or rebuild the synths only record, and the sound change lands at the
 * next drone_std_core_service() frame (<= 50 ms). */
void drone_std_set_enabled(bool on);           /* sustained note-on/off        */

/* Silence the drone without disturbing drone_std_set_enabled()'s state: used by
 * the sequencer solo hook, so releasing solo restores whatever the user had
 * set. */
void drone_std_set_solo_muted(bool muted);

/* Silence the drone while the transport is stopped; same contract as
 * drone_set_paused(). */
void drone_std_set_paused(bool paused);
void drone_std_set_source(drone_source_t src); /* WAVE <-> PATCH               */
void drone_std_set_wave(uint16_t amy_wave);    /* carrier wave (WAVE mode)     */
void drone_std_set_chord(chord_type_t chord);
void drone_std_set_root_note(uint8_t midi_note);
void drone_std_set_follow(drone_follow_t f);   /* invalid values ignored       */
void drone_std_set_level(float v);            /* 0..1 linear output level     */
void drone_std_set_patch(uint16_t patch);      /* PATCH-mode preset            */
void drone_std_set_sub_enabled(bool on);
void drone_std_set_sub_interval(int8_t st);    /* semitones, -36..0            */
void drone_std_set_amp_trim(float v);          /* graph-editor amp mode 0..1   */

/* ── Envelopes (shared graph editor; deferred authority) ── */
void drone_std_get_envelope(seq_env_t *out);
void drone_std_set_envelope(const seq_env_t *env);
void drone_std_get_envelope2(seq_env_t *out);
void drone_std_set_envelope2(const seq_env_t *env);
void drone_std_preview_envelope(const seq_env_t *env);
void drone_std_preview_envelope2(const seq_env_t *env);

/* ── Free filter (full filter editor) ── */
void drone_std_get_filter(seq_filter_t *out);
void drone_std_set_filter(const seq_filter_t *f);
void drone_std_preview_filter(const seq_filter_t *f);  /* AMY only, store untouched */

/* ── Distortion (shared distortion editor) ──
 * One block governs main + sub, as the filter does. reapply re-asserts after a
 * rebuild, which clears the per-osc stage. */
void drone_std_get_dist(seq_dist_t *out);
void drone_std_set_dist(const seq_dist_t *d);
void drone_std_preview_dist(const seq_dist_t *d);  /* AMY only, store untouched */
void drone_std_reapply_dist(void);

/* ── Free LFO (shared LFO editor; WAVE mode only takes effect) ── */
void drone_std_get_lfo(seq_lfo_t *out);
void drone_std_set_lfo(const seq_lfo_t *lfo);

/* ── Whole voice-params block (project store) ──
 * Same contract as drone_get/set_voice_params(): authored flags included,
 * clamped, applied by a rebuild at the next drone_std_core_service() frame. */
void drone_std_get_voice_params(voice_params_t *out);
void drone_std_set_voice_params(const voice_params_t *vp);

/* ── Getters (for the list UI) ── */
bool           drone_std_get_enabled(void);
drone_source_t drone_std_get_source(void);
uint16_t       drone_std_get_wave(void);
chord_type_t   drone_std_get_chord(void);
uint8_t        drone_std_get_root_note(void);
drone_follow_t drone_std_get_follow(void);
float          drone_std_get_level(void);
uint16_t       drone_std_get_patch(void);
bool           drone_std_get_sub_enabled(void);
int8_t         drone_std_get_sub_interval(void);
float          drone_std_get_amp_trim(void);

#ifdef __cplusplus
}
#endif
