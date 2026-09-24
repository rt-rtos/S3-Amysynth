#pragma once
#include <stdint.h>
#include "sequencer_core.h"   /* seq_env_t / seq_filter_t / seq_lfo_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Live-play voice: a dedicated AMY synth slot played directly from a MIDI
 * transport (BLE MIDI in Phase 1), independent of the sequencer tracks.
 * Compiled only under CONFIG_SYNTH_WIRELESS.
 *
 * The note entry points match midi_sink_t (wireless/midi_core) so main can
 * wire them up without an adapter; they run on the transport task and emit
 * untagged apply-now AMY events through amy_helpers, like the drone. */

/* Lazy first configure of the live slot (patch load). Heavy: call from the
 * synth_ui task (radio session_start hook), not from input/transport tasks. */
void live_play_ensure_ready(void);

/* midi_sink_t-compatible note entry points. Channel is ignored (omni). */
void live_play_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void live_play_note_off(uint8_t channel, uint8_t note);

/* Release every held note (session stop / central disconnect - stuck-note
 * safety). Cross-task safe. */
void live_play_all_notes_off(void);

/* AMY synth slot owned by the live voice, so UI code addressing its voices
 * (the filter overlay reads their modulated cutoff) never hard-codes it. */
uint8_t  live_play_synth_slot(void);

/* Voices on the live slot, for sizing shared pools (amy_cfg.ks_oscs). */
uint8_t  live_play_num_voices(void);

/* Patch selection (Wireless menu page; same catalog as melodic/arp). */
uint16_t live_play_get_patch(void);
void     live_play_set_patch(uint16_t patch_number);

/* Browse-group toggle (Wireless page "Source" row): WAVE cycles the raw
 * wave / bass / wavetable block (257..271), PATCH everything else. Switching
 * restores the group's last-used patch (arp source model). */
bool live_play_get_wave_mode(void);
void live_play_set_wave_mode(bool wave);

/* Glide (AMY-native portamento) between note pitches, ms, 0 = off. Range
 * matches the arp / melodic NoteFX Glide: 1 ms per encoder detent. */
#define LIVE_PLAY_GLIDE_MAX_MS 100u
uint16_t live_play_get_glide_ms(void);
void     live_play_set_glide_ms(uint16_t ms);

/* True when the current patch has a reserved LFO carrier pair, so
 * live_play_set_lfo() applies natively. Patch strings own their osc layout and
 * fall back to the software stepper below - the melodic/arp split. */
bool live_play_lfo_native_eligible(void);

/* Retune the BPM-synced LFO carriers after a tempo change; called from
 * sequencer_core_set_bpm() alongside the arp/melodic/drone equivalents. */
void live_play_refresh_lfo_freq(void);

/* Software LFO stepper for non-native patches. Call once per UI frame from
 * the synth_ui task, like arp_core_service(); cheap no-op when the live LFO
 * is off, native, or the slot is not ready. */
void live_play_lfo_service(void);

/* ── Runtime-editable voice params (shared ADSR / filter / LFO editors) ──
 * Same contract as the arp's block: one voice, no layer/track scope, "patch
 * owns it until the user commits, then our copy wins" via the authored flags,
 * with everything re-applied after a patch change.
 *
 * Session-only: nothing here is written to a project, like the live slot's
 * patch number.
 *
 * Core 0 / synth_ui task only - these push through amy_helpers, never under
 * amy_queue_lock. */
void live_play_get_envelope(seq_env_t *out);
void live_play_set_envelope(const seq_env_t *env);
void live_play_get_envelope2(seq_env_t *out);
void live_play_set_envelope2(const seq_env_t *env);

void live_play_get_filter(seq_filter_t *out);
void live_play_set_filter(const seq_filter_t *f);

void live_play_get_dist(seq_dist_t *out);
void live_play_set_dist(const seq_dist_t *d);
void live_play_preview_dist(const seq_dist_t *d);
void live_play_reapply_dist(void);

void live_play_get_lfo(seq_lfo_t *out);
void live_play_set_lfo(const seq_lfo_t *lfo);

float live_play_get_amp_scale(void);
void  live_play_set_amp_scale(float v);

/* Editor live-preview: AMY only, store + authored flags untouched (cancel
 * re-pushes the stored values). */
void live_play_preview_envelope(const seq_env_t *env);
void live_play_preview_envelope2(const seq_env_t *env);
void live_play_preview_filter(const seq_filter_t *f);

#ifdef __cplusplus
}
#endif
