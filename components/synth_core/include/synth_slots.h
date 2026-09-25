#pragma once

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
 * - amy_cfg.max_synths (main.c) must be SYNTH_SLOT_COUNT - it is, by
 *   construction, the only other place this map surfaces. */

#define SEQ_ARP_SYNTH          1    /* standalone arpeggiator                */
#define DRONE_SYNTH_MAIN       2    /* stutter-house drone, chord voices     */
#define DRONE_SYNTH_SUB        3    /* stutter-house drone, sub tone         */
#define DRONE_STD_SYNTH_MAIN   4    /* normal (free-running) drone, chord    */
#define DRONE_STD_SYNTH_SUB    5    /* normal drone, sub tone                */
#define SEQ_DRUM_SYNTH_BASE    6    /* SEQ_TRACKS drum tracks: 6..10         */
#define LIVE_SYNTH             (SEQ_DRUM_SYNTH_BASE + SEQ_TRACKS)   /* live-play (BLE/USB MIDI) voice: 11 */
#define CLIP_SYNTH_BASE        (LIVE_SYNTH + 1)   /* bounce clip players: 12..12+CLIP_SLOT_COUNT-1 */
#define CLIP_SLOT_COUNT        2    /* clip slots; the melodic arena is unaffected */
#define SEQ_MEL_SYNTH_BASE     (CLIP_SYNTH_BASE + CLIP_SLOT_COUNT)   /* first melodic slot; arena from here up */

#define SEQ_MEL_SLOT_COUNT     52   /* melodic arena size; THE polyphony knob */
#define SYNTH_SLOT_COUNT       (SEQ_MEL_SYNTH_BASE + SEQ_MEL_SLOT_COUNT)   /* == amy_cfg.max_synths */
#define SEQ_MAX_SYNTH          (SYNTH_SLOT_COUNT - 1)   /* melodic ceiling   */
