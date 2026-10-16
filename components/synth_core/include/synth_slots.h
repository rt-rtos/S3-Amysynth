#pragma once

#include "sdkconfig.h"
#include "seq_model.h"   /* SEQ_TRACKS: width of the drum block */

/* ── AMY synth slot map - single source of truth ─────────────────────────
 * Static consumers pack the bottom of the slot space; the melodic sequencer
 * is an open-ended arena on top. Growing melodic capacity is one edit:
 * raise SEQ_MEL_SLOT_COUNT (the real ceiling is AMY's osc pool, max_oscs).
 *
 * Contract:
 * - Slot 0 is reserved as a sentinel: sequencer row lookups use synth id 0
 *   for "no such row" (seq_core_synth.c). Never assign it to a consumer.
 * - Static slots (1..SEQ_MEL_SYNTH_BASE-1) are compile-time fixed, never
 *   overlap, and are identical across targets.
 * - Melodic allocation runs [SEQ_MEL_SYNTH_BASE, SEQ_MAX_SYNTH]; the
 *   static-vs-melodic predicate is a single `slot >= SEQ_MEL_SYNTH_BASE`.
 * - amy_cfg.max_synths (main.c) must be AMY_SYNTH_COUNT: the slot map plus
 *   the shared LFO synths above it (LFO_SYNTH_BASE..AMY_SYNTH_COUNT-1),
 *   which belong to the LFO bank and are no consumer's slot. */

#define SEQ_ARP_SYNTH          1    /* standalone arpeggiator                */
#define DRONE_SYNTH_MAIN       2    /* stutter-house drone, chord voices     */
#define DRONE_SYNTH_SUB        3    /* stutter-house drone, sub tone         */
#define DRONE_STD_SYNTH_MAIN   4    /* normal (free-running) drone, chord    */
#define DRONE_STD_SYNTH_SUB    5    /* normal drone, sub tone                */
#define SEQ_DRUM_SYNTH_BASE    6    /* SEQ_TRACKS drum tracks: 6..10         */
#define LIVE_SYNTH             (SEQ_DRUM_SYNTH_BASE + SEQ_TRACKS)   /* live-play (BLE/USB MIDI) voice: 11 */
#define CLIP_SYNTH_BASE        (LIVE_SYNTH + 1)   /* bounce clip players: 12..12+CLIP_SLOT_COUNT-1 */
#define CLIP_SLOT_COUNT        2    /* clip slots; the melodic arena is unaffected */
#define DROP_SYNTH             (CLIP_SYNTH_BASE + CLIP_SLOT_COUNT)   /* ball drop, impact voice: 14 */
#define DROP_SUB_SYNTH         (DROP_SYNTH + 1)   /* ball drop, jackpot (sub) voice: 15 */
#define SEQ_MEL_SYNTH_BASE     (DROP_SUB_SYNTH + 1)   /* first melodic slot; arena from here up */

#define SEQ_MEL_SLOT_COUNT     CONFIG_SEQ_MEL_SLOT_COUNT   /* melodic arena size; THE polyphony knob */
#define SYNTH_SLOT_COUNT       (SEQ_MEL_SYNTH_BASE + SEQ_MEL_SLOT_COUNT)   /* end of the slot map */
#define SEQ_MAX_SYNTH          (SYNTH_SLOT_COUNT - 1)   /* melodic ceiling   */

#if CONFIG_SEQ_LFO_SHARED
#define LFO_SYNTH_MAX          CONFIG_LFO_SYNTH_MAX
#else
#define LFO_SYNTH_MAX          0
#endif
#define LFO_SYNTH_BASE         SYNTH_SLOT_COUNT   /* first shared LFO synth */
#define AMY_SYNTH_COUNT        (SYNTH_SLOT_COUNT + LFO_SYNTH_MAX)   /* == amy_cfg.max_synths */
_Static_assert(AMY_SYNTH_COUNT <= 256, "AMY synth numbers are 8-bit (amy_event.synth)");
