#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "seq_model.h"     /* seq_env_t, seq_filter_t, seq_lfo_t */
#include "chord_types.h"   /* chord_type_t */
#include "custompatches/drone_core.h"   /* drone_follow_t, drone_patch_excluded() */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Normal (free-running) drone ──────────────────────────────────────────
 * The stutter drone's sibling: same chord voicing, plain sustained output -
 * no stutter gate, no peak/duck amp coupling, no pattern/blip/swing service,
 * no tempo-locked filter sweep.
 *
 * The sound is one SEQ_PATCH_* id from the unified patch list (the
 * drone_patch_excluded() domain), applied through the shared apply path
 * (sequencer_core_configure_synth_slot) at the voice count the patch's osc
 * budget allows (drone_voices_for), up to DRONE_CHORD_MAX_NOTES. When fewer
 * voices than chord tones fit, the chord is truncated (drone_chord_tones).
 * Level (x amp trim) is the amp CONST of a wave patch's osc 0, or of any
 * other patch's lfo_targets voice-mask oscs (the Juno SILENT head, the
 * DX7/FM ALGO osc, the additive parent, both bass oscs scaled by the preset's
 * balance), replacing the head's own level; an unmapped patch (the piano)
 * keeps its own. drone_std_set_level()
 * pushes it at once. The sub is a fixed one-voice sine:
 * level and EG0, no filter, LFO or distortion.
 *
 * On the main synth it exposes the standard per-voice toolset, re-imposed
 * after every rebuild where authored:
 *   - a free filter (seq_filter_t) with its envelope routing matrix, edited
 *     in the filter and graph editors;
 *   - a free LFO (seq_lfo_t): with CONFIG_SEQ_LFO_SHARED a shared LFO synth
 *     (lfo_bank.h) over a wave patch's layout or a mapped patch string, none
 *     when the bank is full; without it the AMY-native carriers on wave
 *     patches only - patch strings own their osc topology;
 *   - the shared ADSR/EG1 graph editor storage and distortion.
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
 * sequencer_core_set_bpm, mirroring arp_core_refresh_lfo_freq). A no-op with
 * CONFIG_SEQ_LFO_SHARED (no per-voice carriers). */
void drone_std_core_refresh_lfo_freq(void);

/* Live custom-voice edits: re-push s_fm_voice (FM_PUSH_* scope or operator
 * index `what`) / s_additive_voice to the main synth when the drone plays
 * SEQ_PATCH_FM_CUSTOM / SEQ_PATCH_ADDITIVE_CUSTOM; no-op otherwise. No
 * rebuild, so held notes keep sounding. Called from
 * sequencer_core_fm_voice_changed() / sequencer_core_additive_voice_changed(),
 * like the arp's hooks. UI task. */
void drone_std_core_fm_voice_changed(uint8_t what);
void drone_std_core_additive_voice_changed(void);

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
void drone_std_set_chord(chord_type_t chord);
void drone_std_set_root_note(uint8_t midi_note);
void drone_std_set_follow(drone_follow_t f);   /* invalid values ignored       */
void drone_std_set_level(float v);            /* 0..1 linear output level     */
void drone_std_set_patch(uint16_t patch);      /* excluded ids -> TRIANGLE      */
void drone_std_set_sub_enabled(bool on);
void drone_std_set_sub_interval(int8_t st);    /* semitones, DRONE_SUB_INTERVAL_MIN..MAX */
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
 * Main only. reapply re-asserts after a rebuild, which clears the per-osc
 * stage. */
void drone_std_get_dist(seq_dist_t *out);
void drone_std_set_dist(const seq_dist_t *d);
void drone_std_preview_dist(const seq_dist_t *d);  /* AMY only, store untouched */
void drone_std_reapply_dist(void);

/* ── Free LFO (shared LFO editor; without CONFIG_SEQ_LFO_SHARED only wave
 * patches take it) ── */
void drone_std_get_lfo(seq_lfo_t *out);
void drone_std_set_lfo(const seq_lfo_t *lfo);
/* True while the main synth's LFO runs on a shared LFO synth - the path on
 * which WOBBLE exists. Always false without CONFIG_SEQ_LFO_SHARED. UI task. */
bool drone_std_lfo_bound(void);
/* LFO2, main only: the arp's contract (arp_core.h, arp_set_lfo2), except that
 * the collapse which hands slot 0 back to the patch rebuilds at the next
 * drone_std_core_service() frame. UI task. */
bool drone_std_get_lfo2(seq_lfo_t *out);
void drone_std_set_lfo2(const seq_lfo_t *lfo, bool open);
bool drone_std_lfo2_bound(void);

/* ── Whole voice-params block (project store) ──
 * Same contract as drone_get/set_voice_params(): authored flags included,
 * clamped, applied by a rebuild at the next drone_std_core_service() frame. */
void drone_std_get_voice_params(voice_params_t *out);
void drone_std_set_voice_params(const voice_params_t *vp);

/* ── Getters (for the list UI) ── */
bool           drone_std_get_enabled(void);
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
