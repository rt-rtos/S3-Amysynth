#pragma once

/* ── Transport stop machine: how Stop ends the sequenced sources ─────────
 * Four stop modes. RING is the plain transport stop (note-offs, tails ring
 * out). The others arm a stop tick X and, when it is reached, stop the
 * transport, ramp the sequenced synths to silence over about 100 ms, hard-
 * stop every note on them (amy.h RESET_NOTE_CUT, no release) and drain the
 * echo and reverb lines of the buses they feed (amy_fx_drain_begin):
 * - CUT: X = now.
 * - CUT_QUARTER: X = the next quarter tick.
 * - FADE: from the next quarter tick Q to X = Q + the fade length, the
 *   transport factor of synth_level (synth_level_set_transport) follows the
 *   mixer's fade curve from 1 to 0 (mixer_fade_pos / mixer_pos_gain).
 * The sequenced synths are every layer track's synth, the arp, both drop
 * voices, the four drone slots and the clip slots; never LIVE_SYNTH, never a
 * synth at or above SYNTH_SLOT_COUNT. Only those that are present (an
 * instrument behind them) get sends.
 *
 * Contract:
 * - Execution context: UI task (Core 0) only, every function. Sends go
 *   through amy_helpers; nothing allocates; the cut ramp's sends can wait on
 *   the ingest FIFO's back-pressure.
 * - The UI play mirror (seq_state.playing) equals the transport state at all
 *   times: while a stop is armed or fading it still shows playing; the cut
 *   sets both together.
 * - Horizon ownership: arming takes the periodic end horizon
 *   (sequencer_core_freeze_set(X)) only while the loop bounce is idle
 *   (clip_bounce_get_state() == CLIP_BOUNCE_IDLE); with a bounce armed or
 *   recording, Stop behaves as RING. The machine clears only a horizon it set
 *   (at the cut, a cancel or a reset). A bounce that leaves idle while a stop
 *   is armed takes the horizon over: the arm is dropped without clearing it.
 * - Invariant: the transport factor is 1 whenever no arm, fade, cut ramp or
 *   recovery ramp is in progress.
 * - Mode and fade index are persisted (project GLOB); transport_stop_defaults()
 *   is their project-format fallback and the boot state (no Kconfig). */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TSTOP_RING = 0,
    TSTOP_CUT,
    TSTOP_CUT_QUARTER,
    TSTOP_FADE,
    TSTOP_MODE_COUNT
} tstop_mode_t;

#define TSTOP_FADE_COUNT 4   /* fade lengths: 1/4, 1 bar, 2 bars, 4 bars */

typedef struct {
    uint8_t mode;       /* tstop_mode_t */
    uint8_t fade_idx;   /* 0..TSTOP_FADE_COUNT-1 */
} transport_stop_params_t;

/* Fallback values of the project format: RING, fade index 1 (1 bar). Pure;
 * any task; fills every field. */
void         transport_stop_defaults(transport_stop_params_t *out);

/* Setters clamp to the last valid value; they apply from the next press. */
tstop_mode_t transport_stop_get_mode(void);
void         transport_stop_set_mode(uint8_t mode);
uint8_t      transport_stop_get_fade_idx(void);
void         transport_stop_set_fade_idx(uint8_t idx);
/* Fade length of an index in sequencer ticks (clamped index). Pure. */
uint32_t     transport_stop_fade_ticks(uint8_t idx);
/* True while a stop waits for its tick or fades: the next press cancels it. */
bool         transport_stop_is_armed(void);

/* Stop pressed while playing. RING (or a bounce not idle): the plain stop,
 * at once. Otherwise arms the stop at X as above; CUT runs the cut at once.
 * A press while a stop is armed or fading cancels it, as Play does below
 * (the Play/Stop button is one toggle and still shows playing then).
 * Ignored during the cut ramp. */
void transport_stop_press(void);

/* Play pressed. While a stop is armed or fading: cancels it (releases the
 * owned horizon, ramps the factor back to 1 at the cut-ramp slope) and the
 * transport keeps playing. During the cut ramp: Play runs right after the
 * hard stop and the drain begin. Otherwise the plain start. */
void transport_stop_play_press(void);

/* Every 10 ms UI wake. Steps the fade, the cut ramp and the recovery ramp,
 * runs the cut once X is reached, and drops an arm whose horizon a bounce
 * took. Two compares while idle. */
void transport_stop_service(void);

/* Cancel an arm or fade (releasing an owned horizon), drop a pending Play,
 * factor back to 1 with the synths reasserted. A drain in progress
 * continues. Called by the project loader before it stops the transport. */
void transport_stop_reset(void);
