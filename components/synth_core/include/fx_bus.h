#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Synth-group -> AMY bus routing table ────────────────────────────────
 * Which AMY bus a synth slot renders on. Pure bookkeeping: no AMY calls, no
 * allocation, no locking - every function here is total and callable from any
 * task. Acting on the table (re-tagging live synths, pushing a bus's FX) is
 * amy_fx.c's job.
 *
 * The four groups partition the slot map (synth_slots.h). MELODIC is the
 * open-ended arena plus the live-play voice, the arp and the sentinel slot;
 * it never moves, so bus 0 is always where the bulk of the mix renders.
 * DRUMS and DRONES each own one bus and are routed there only while their
 * Split flag is on - Split OFF puts them back on bus 0, which is what makes
 * the default configuration bit-identical to a single-bus build. CLIPS owns
 * the last bus outright (never on bus 0, no Split flag): a bounce sums every
 * bus below it, so a clip can never record itself.
 *
 * Obligations: slot < SYNTH_SLOT_COUNT, bus < FX_BUS_COUNT, g < FX_GROUP_COUNT.
 * Guarantees: every query returns a valid group/bus (< FX_BUS_COUNT); no call
 * can fail. */

typedef enum {
    FX_GROUP_MELODIC = 0,
    FX_GROUP_DRUMS,
    FX_GROUP_DRONES,
    FX_GROUP_CLIPS,
    FX_GROUP_COUNT
} fx_group_t;

#define FX_BUS_COUNT 4        /* == amy_cfg.max_buses (main.c) */
#define FX_BUS_HOME  0        /* the engine default bus; everything lands here
                               * unless its group is split off */
#define FX_BUS_CLIPS (FX_BUS_COUNT - 1)   /* the clip players' bus; every bus
                                           * below it is a bounce source */
/* Gain the clip bus carries on top of master x trim. A clip holds the
 * mixdown's own output: the bounce captures each source bus at the 0.1 x
 * volume the mixdown applies (amy_render_audio), so playing it back through
 * that same mixdown would apply the 0.1 twice. The inverse here puts a clip
 * at the level its sources had. */
#define FX_BUS_CLIPS_MAKEUP 10.0f

/* Slot -> group: 2..5 DRONES, 6..10 DRUMS, the CLIP_SYNTH_BASE block CLIPS,
 * everything else (sentinel, arp, live play, melodic arena) MELODIC. */
fx_group_t fx_group_of_slot(uint8_t slot);
/* The bus a group owns while split: MELODIC 0, DRUMS 1, DRONES 2, CLIPS 3. */
uint8_t    fx_group_own_bus(fx_group_t g);
/* Inverse of fx_group_own_bus for 1..3; bus 0 maps to MELODIC. */
fx_group_t fx_group_of_bus(uint8_t bus);

/* Split flag of a group. MELODIC is always false (it owns bus 0 outright),
 * CLIPS always true (it owns its bus outright). */
bool       fx_group_is_split(fx_group_t g);
/* Store a group's split flag. Returns true only when the flag actually
 * changed, so the caller can skip the re-tag/push work; MELODIC and CLIPS
 * always return false. Stores only - no engine state is touched here. */
bool       fx_bus_set_split(fx_group_t g, bool on);

/* Where a group renders right now: its own bus while split, else FX_BUS_HOME. */
uint8_t    fx_bus_of_group(fx_group_t g);
/* Where a slot renders right now. */
uint8_t    fx_bus_for_synth(uint8_t slot);
/* True when anything renders on this bus: bus 0 always, FX_BUS_CLIPS while a
 * clip is loaded, the rest only while their group is split. An inactive bus
 * must be left muted (amy_fx.c) - a reverb tail keeps costing render time on
 * a bus nothing feeds. */
bool       fx_bus_is_active(uint8_t bus);
/* Clip-bus liveness, stored by the clip player when the first slot loads and
 * the last one clears. Store only; the caller syncs the bus (amy_fx.c). */
void       fx_bus_set_clips_loaded(bool loaded);

/* Display names: the group's own name, and the bus's ("Main" for bus 0). */
const char *fx_group_name(fx_group_t g);
const char *fx_bus_label(uint8_t bus);

/* The group's fixed slots, written into out[]; returns the count (DRUMS 4,
 * DRONES 4, CLIPS CLIP_SLOT_COUNT, MELODIC 0 - the melodic arena is
 * open-ended and never re-tagged, since it stays on bus 0). out must hold 8
 * entries. */
uint8_t    fx_group_slots(fx_group_t g, uint8_t out[8]);

#ifdef __cplusplus
}
#endif
