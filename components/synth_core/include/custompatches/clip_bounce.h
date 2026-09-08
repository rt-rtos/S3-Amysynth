#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "amy.h"   /* SAMPLE, AMY_NCHANS, AMY_BLOCK_SIZE */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Loop bounce: record the source buses into a clip ─────────────────────
 * Captures the post-FX, post-level sum of every source bus (every bus but
 * FX_BUS_CLIPS) into a clip slot's preset, which clip_player then loops in
 * sync so the sources can be muted and their voices reused. The capture is
 * at the mixdown's output level; the clip bus makes that up on playback
 * (FX_BUS_CLIPS_MAKEUP), so a clip sits where its sources sat.
 *
 * Flow:
 *   IDLE --start--> ARMED --next bar line--> RECORDING --end tick--> TAIL
 *   TAIL --tail folded--> READY --service--> IDLE; cancel from any non-IDLE.
 * The UI task owns start, stop, cancel and the READY commit; the render task
 * owns the other transitions and is the clip buffer's only writer from ARMED
 * to READY.
 *
 * Length: the capture is open-ended. Start reserves the shape's Max bars,
 * rounded up to a whole number of pattern periods
 * (sequencer_core_pattern_period_bars), and records until either the stop
 * lands or the reservation fills. Stop rounds up to the next pattern-period
 * boundary after the press, so a clip is always a whole number of periods
 * and never falls out of step with the pattern; the reservation is trimmed
 * in place to the real length at the stop. A press that lands within a few
 * ticks of a boundary rolls to the next one, so the stop can never race the
 * bar line.
 *
 * Timing: start_tick is the next bar line at start time. The loop is
 * loop_frames = round(ticks * us_per_tick * Fs / 1e6) frames from the first
 * block in which the tick clock reaches start_tick; the clip's note-on fires
 * at end_tick. Both land on block edges, so a clip sits within one block
 * (5.3 ms) of the pattern it was recorded against.
 *
 * Freeze: the sequencer freeze horizon sits at end_tick (moved earlier by a
 * stop), so no periodic note-on (tracks, arp) and no decorated one-shot
 * fires from the bar line on; the block that reaches end_tick queues a
 * release of every source voice. Their ring-out (releases, echo, reverb) is
 * the tail, folded into the clip head as it arrives, so the loop seam
 * carries it from pass 2. The commit then disables the arp and drones and,
 * per the shape's After, either mutes every track (the ordinary, visible
 * flags) or empties every track's pattern so the layers are ready for the
 * next part, then lifts the horizon. What the commit replaced - the flags,
 * and under Clear the layers themselves - is kept for one Undo. */
typedef enum {
    CLIP_BOUNCE_IDLE = 0,
    CLIP_BOUNCE_ARMED,
    CLIP_BOUNCE_RECORDING,
    CLIP_BOUNCE_TAIL,
    CLIP_BOUNCE_READY,
} clip_bounce_state_t;

/* PSRAM budget shared by every clip slot, in KB. Accounting over the heap,
 * not a separate allocator. */
#define CLIP_BOUNCE_POOL_KB 1536u
#define CLIP_BOUNCE_MAX_BARS 16u

/* Ring-out captured after end_tick and folded into the clip head. Capped at
 * half the loop. */
typedef enum {
    CLIP_TAIL_NONE = 0,
    CLIP_TAIL_BEAT,
    CLIP_TAIL_BAR,
} clip_tail_t;

/* What the commit does with the source tracks. Arp and drones are disabled
 * either way. */
typedef enum {
    CLIP_AFTER_MUTE = 0,   /* mute every track; the patterns stay behind the flags */
    CLIP_AFTER_CLEAR,      /* empty every track's pattern (sequencer_core_clear_track_pattern);
                              the tracks stay audible for the next part */
} clip_after_t;

/* The shape of the next bounce, as the BOUNCE page edits it and the project
 * snapshot persists it. */
typedef struct {
    uint8_t      max_bars;   /* 1, 2, 4, 8 or 16: the reservation, and the auto-stop */
    bool         stereo;
    clip_tail_t  tail;
    clip_after_t after;
} clip_bounce_shape_t;

#define CLIP_SLOT_AUTO 0xFFu   /* start: pick the first empty slot */

void clip_bounce_init(void);
clip_bounce_state_t clip_bounce_get_state(void);

/* UI task. set clamps: max_bars to the 1/2/4/8/16 list (else 4), tail and
 * after to their enums. Does not touch a bounce in flight (start copies the
 * shape). */
void clip_bounce_get_shape(clip_bounce_shape_t *out);
void clip_bounce_set_shape(const clip_bounce_shape_t *shape);

/* ── UI-task (Core 0) control surface (also called from the button task) ── */

/* Bytes a bounce of this shape reserves at the current tempo and pattern
 * period, and whether the pool has room for it now. */
uint32_t clip_bounce_bytes_for(const clip_bounce_shape_t *shape);
bool     clip_bounce_fits(const clip_bounce_shape_t *shape);

/* IDLE -> ARMED. slot is a slot index or CLIP_SLOT_AUTO. Obligations: the
 * slot EMPTY (auto: one exists), transport playing, the pool has room.
 * Reserves the slot's preset, fixes start_tick at the next bar line and the
 * auto-stop end, schedules the clip's note-on there and sets the freeze
 * horizon. false on OOM or a violated obligation, with no state change. */
bool     clip_bounce_start(uint8_t slot);

/* RECORDING only: end the take at the next pattern-period boundary (see the
 * length rules above); the reservation is trimmed then. true when a stop is
 * now set (also when the auto-stop already covers it); false otherwise. */
bool     clip_bounce_stop(void);

/* Any non-IDLE state -> IDLE; frees the target slot (clip_player_clear
 * rules) and lifts the freeze. Sources that were already released resume
 * at their next due tick. No-op when IDLE. */
void     clip_bounce_cancel(void);

/* The one-button transport: IDLE starts (auto slot), ARMED cancels (nothing
 * recorded yet), RECORDING stops - or discards when long is set. TAIL and
 * READY ignore it. */
void     clip_bounce_chord(bool long_press);

/* Undo the last committed bounce: restore the mute and enable flags the
 * commit replaced and, after a Clear, the layers as they were (import per
 * layer; one whose type changed since is skipped), then clear its slot. One
 * level; consumed by the next commit. Applier task (layer import). */
bool     clip_bounce_undo_available(void);
void     clip_bounce_undo(void);

uint8_t  clip_bounce_slot(void);        /* target slot; meaningful while not IDLE */
uint8_t  clip_bounce_bars_total(void);  /* current target length (auto-stop, or the stop's) */
uint8_t  clip_bounce_bars_done(void);   /* whole bars captured so far */
bool     clip_bounce_stop_set(void);    /* a stop press has fixed the end */

/* Periodic UI-task service: commits a READY bounce (mute every track or
 * empty every pattern per the shape's After, disable arp and drones, lift
 * the freeze, state -> IDLE) and cancels a bounce the render task flagged as
 * broken (tempo changed mid-capture). A Clear whose undo copy does not fit
 * in PSRAM mutes instead. */
void     clip_bounce_service(void);

/* ── Render-task (Core 1) entry points ── */

/* AMY's amy_external_bus_postprocess_hook: called under amy_queue_lock once
 * per active bus per block, after that bus's FX and before the mixdown. buf
 * is AMY_NCHANS sequential channel blocks of len SAMPLEs. Moves ARMED to
 * RECORDING when the tick clock reaches start_tick and RECORDING to TAIL
 * (queueing the source release) when it reaches end_tick, then adds every
 * bus except FX_BUS_CLIPS into the block accumulator at the mixdown's own
 * per-bus scale. Alloc-free, log-free, no AMY calls beyond the non-blocking
 * pump enqueue, O(len). No-op unless a bounce is running. */
void clip_bounce_bus_hook(uint16_t bus, SAMPLE *buf, uint16_t len);

/* Call once per rendered block, immediately after amy_update() returns
 * (next to sample_rec_render_tick), i.e. with amy_queue_lock released.
 * Converts the accumulator into the clip buffer (loop frames, then the tail
 * fold) and runs the clip player's deferred unloads and drift guard.
 * Alloc-free; bounded work per block. */
void clip_bounce_render_tick(const int16_t *interleaved_stereo, uint16_t frames);

#ifdef __cplusplus
}
#endif
