#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "synth_slots.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Bounce clip players ──────────────────────────────────────────────────
 * One AMY synth per clip slot (CLIP_SYNTH_BASE + slot) on FX_BUS_CLIPS,
 * looping the slot's memory PCM preset (CLIP_PRESET_BASE + slot) forever at
 * exactly the rate it was recorded: no midi_note and no freq on the osc, so
 * render_pcm takes its exact-rate path. clip_bounce.c fills the preset; this
 * module owns the preset's existence, the synth definition, transport
 * start/stop and the per-slot level.
 *
 * Execution context: UI task (Core 0) for everything except
 * clip_player_render_service. Every call is an AMY event send (config or
 * tick-scheduled), never an apply.
 * Invariants: a slot's preset exists exactly while the slot is not EMPTY;
 * the pool byte count is the sum of the non-EMPTY slots' buffers. */

#define CLIP_PRESET_BASE 200u   /* clear of ROM, gamma (256..391) and sample_rec (100) */

typedef enum {
    CLIP_SLOT_EMPTY = 0,
    CLIP_SLOT_LOADED,
    CLIP_SLOT_CLEARING,   /* freed by the UI, preset unload pending on the render task */
} clip_slot_state_t;

/* What a clip does when the tempo is not the one it was recorded at. At the
 * recorded tempo both modes play the plain exact-rate path. */
typedef enum {
    CLIP_TEMPO_STRETCH = 0,   /* time-stretch to its bar count, pitch kept (AMY fit) */
    CLIP_TEMPO_VARI,          /* rate follows tempo, pitch follows rate */
} clip_tempo_mode_t;

void clip_player_init(void);

clip_slot_state_t clip_player_slot_state(uint8_t slot);
uint8_t  clip_player_slot_bars(uint8_t slot);
bool     clip_player_slot_stereo(uint8_t slot);
uint32_t clip_player_slot_bytes(uint8_t slot);
uint32_t clip_player_pool_used(void);        /* bytes across every non-EMPTY slot */

/* Play/mute is a gain on the clip synth: a muted clip keeps looping silently,
 * so unmuting is seamless and in phase. A slot unmuted after a transport
 * start joins at the next bar line. */
bool     clip_player_get_playing(uint8_t slot);
void     clip_player_set_playing(uint8_t slot, bool on);
uint8_t  clip_player_get_level(uint8_t slot);   /* percent, 0..100 */
void     clip_player_set_level(uint8_t slot, uint8_t pct);
clip_tempo_mode_t clip_player_get_tempo_mode(uint8_t slot);
/* Takes effect at the next bar line (a retrigger), or at once for a VARI
 * rate change. */
void     clip_player_set_tempo_mode(uint8_t slot, clip_tempo_mode_t mode);

/* Called by sequencer_core_set_bpm with the new BPM. Stretch slots retrigger
 * at the next bar line fitted to their bar count; VARI slots re-rate at once. */
void     clip_player_on_tempo_change(uint16_t bpm);

/* Free the slot. Cancels a bounce in flight into it. The synth is silenced
 * now; the preset is unloaded on the render task a few blocks later (the slot
 * reads CLEARING until then, and clip_bounce_arm refuses it). */
void     clip_player_clear(uint8_t slot);

/* Called by sequencer_core_set_playing. Stop resets every clip osc (silent,
 * preset kept); play restarts each playing slot at the next bar line from
 * the source position its recorded tick anchor implies there, so a clip
 * keeps the same relation to the absolute bar grid as when it was recorded
 * (the pattern's multi-bar tracks live on that grid too). */
void     clip_player_on_transport(bool playing);

/* Periodic UI-task service: retires the clip bus once every slot is EMPTY,
 * and re-anchors slots the drift guard flagged (a retrigger at the next bar
 * line from the position the tick grid implies). */
void     clip_player_service(void);

/* ── clip_bounce.c-facing ── */

/* Allocate the slot's preset (alloc_frames frames x channels of int16,
 * zeroed, looping 0..loop_frames) and define its synth, silent until a start
 * is scheduled. Obligations: slot EMPTY, the pool has room (the caller checks
 * with clip_bounce_fits). Returns the sample buffer, or NULL on OOM with no
 * state change. Activates the clip bus. */
int16_t *clip_player_slot_alloc(uint8_t slot, uint8_t bars, bool stereo,
                                uint32_t alloc_frames, uint32_t loop_frames,
                                uint32_t rec_us_per_tick);

/* Schedule the slot's note-on from frame 0 at an absolute sequencer tick,
 * which becomes the slot's grid anchor; a later call replaces the earlier
 * one. */
void     clip_player_schedule_start(uint8_t slot, uint32_t tick);

/* Cut a LOADED slot's preset down to alloc_frames (looping 0..loop_frames)
 * once an open-ended capture knows its length. In place under the lock (no
 * copy); the buffer beyond alloc_frames is gone on return. false leaves the
 * slot as it was. */
bool     clip_player_slot_trim(uint8_t slot, uint8_t bars, uint32_t alloc_frames, uint32_t loop_frames);

/* First EMPTY slot, or 0xFF when every slot holds or is clearing a clip. */
uint8_t  clip_player_first_empty_slot(void);

/* Render task (Core 1), amy_queue_lock released: runs the deferred preset
 * unloads. Briefly re-acquires the lock on the block that unloads. */
void     clip_player_render_service(void);

/* Render task, lock released, once per block: on a bar line, compares every
 * playing clip's source position with the one the tick grid implies and
 * flags a re-anchor when they differ by more than a block or two. Reads
 * only; never sends. */
void     clip_player_bar_check(void);

#ifdef __cplusplus
}
#endif
