#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "fx_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Per-bus FX state ───────────────────────────────────────────────────────
 * Cached mirror of the values last pushed to ONE of AMY's effect buses.
 * AMY exposes no getters, so we keep our own copy for menu display; there is
 * one fx_state_t per bus (s_fx[], indexed 0..FX_BUS_COUNT-1) and the routing
 * table in fx_bus.h decides which synths feed which.
 * All pushes go through the shared amy_helpers mutex (amy_event is ~800 B;
 * never allocate it on a task stack). */

/* Sentinel for the extended FX params. While a field holds it, fx_push_*
 * leaves the matching amy_event field at AMY_UNSET so AMY keeps its factory
 * default. Chosen outside every valid range, incl. negative echo tone. */
#define FX_PARAM_UNSET  INT16_MIN

typedef struct {
    int8_t  eq_low_db;     /* -15..+15 dB */
    int8_t  eq_mid_db;
    int8_t  eq_high_db;
    uint8_t echo_level;    /* 0..100 -> 0..1 */
    uint8_t chorus_level;  /* 0..100 -> 0..1 */
    uint8_t reverb_level;  /* 0..100 -> 0..1 */
    /* Extended FX params, FX_PARAM_UNSET until dialed in. Percent
     * fields map /100 to AMY's 0..1 float; others as noted. */
    int16_t echo_delay_ms;   /* 0..743 ms;   unset -> AMY 500 ms          */
    int16_t echo_feedback;   /* 0..99 (%);   unset -> AMY 0 (one repeat)  */
    int16_t echo_tone;       /* -99..99 (% filter coef); unset -> AMY 0   */
    int16_t reverb_liveness; /* 0..100 (%);  unset -> AMY 0.85            */
    int16_t reverb_damping;  /* 0..100 (%);  unset -> AMY 0.5             */
    int16_t reverb_xover_hz; /* 500..8000 Hz; unset -> AMY 3000 Hz        */
    int16_t chorus_rate;     /* centi-Hz (0.01 Hz); unset -> AMY 0.5 Hz   */
    int16_t chorus_depth;    /* 0..100 (%);  unset -> AMY 0.5             */
    /* Bus distortion. Concrete defaults, no sentinels: bus_reset()'s values
     * are known, unlike the factory FX above. */
    uint8_t bus_dist_type;   /* stage mask: bit0 CLIP, bit1 FOLD, bit2 CRUSH;
                                0 = OFF; mapped onto AMY's enables on push */
    uint8_t bus_dist_drive;  /* 1..16 pre-gain (fold depth for FOLD)       */
    uint8_t bus_dist_bits;   /* 1..24 CRUSH bit depth; 24 = no-op          */
    uint8_t bus_dist_rate;   /* 1..64 CRUSH sample-hold length in samples  */
    uint8_t bus_dist_mix;    /* 0..100 -> 0..1 wet/dry                     */
    uint8_t level;           /* 0..200 (%) of master volume, this bus only */
} fx_state_t;

/* Live FX caches, one per bus: the FX menu updates fields directly, then calls
 * the matching fx_push_* below with that bus. */
extern fx_state_t s_fx[FX_BUS_COUNT];

/* False: loading a patch must NOT change the FX of the bus it loads onto.
 * Every built-in Juno patch string ends with `x<eq>k<chorus>` commands writing
 * the loading synth's bus FX, so without this guard a preset change on any
 * synth re-skins that whole bus. Patch-load sites call synth_ui_fx_reassert(),
 * which re-imposes the cached values right after the patch's FX deltas, making
 * presets timbre-only.
 * True: the most-recently-loaded preset's FX applies to its bus. */
extern bool s_fx_presets_alter_global;

/* Push individual effect bands to one AMY bus. Each names the bus explicitly:
 * an FX event carries no osc, so even a spelled-out bus 0 stays bus-scope and
 * emits no per-osc deltas.
 *
 * RULE: the engine state of a bus is always `active ? cache : muted`, whichever
 * path pushed it. An inactive bus (fx_bus_is_active() false) gets the muted
 * shape - flat EQ, zero echo/chorus/reverb, dist off - not the cache. That is
 * what makes "Split OFF costs nothing" true: a reverb left armed on a bus
 * nothing feeds still costs render time every block. */
void fx_push_eq(uint8_t bus);
void fx_push_echo(uint8_t bus);
void fx_push_chorus(uint8_t bus);
void fx_push_reverb(uint8_t bus);
void fx_push_dist(uint8_t bus);

/* All five pushes for one bus. */
void fx_bus_sync(uint8_t bus);

/* Split a group onto its own bus (or fold it back onto FX_BUS_HOME): re-tags
 * the group's existing synths and syncs the group's own bus. No-op when the
 * flag is unchanged or g is MELODIC. Bus 0's FX are never touched. */
void amy_fx_set_bus_split(fx_group_t g, bool on);

/* Re-tag every movable group's synths toward the bus the routing table
 * currently names, then sync all buses. The project loader calls this after
 * restoring the split flags and the FX caches. */
void amy_fx_apply_routing(void);

/* Re-impose a bus's cached FX after a patch load, since every built-in Juno
 * patch ends with EQ/chorus commands that would re-skin the bus the loading
 * synth renders on. The sequencer/arp/drone patch-load paths call this with
 * the slot they configured, right after loading; it is a no-op while the
 * "Preset FX" toggle is on. */
void synth_ui_fx_reassert(uint8_t slot);
/* Same, for every bus at once (re-arming the Preset FX guard). */
void synth_ui_fx_reassert_all(void);

/* Master output volume (0..2.0, unity=1.0), scaled per bus by that bus's
 * level and written to amy_global.volume[]. The 2x headroom allows boosting
 * quiet sources. */
void  amy_fx_set_master_volume(float v);   /* clamps 0..2 and pushes to AMY */
float amy_fx_get_master_volume(void);      /* returns current cached value    */

/* Per-bus trim, 0..200 percent of the master volume. */
void  amy_fx_set_bus_level(uint8_t bus, uint8_t pct);

#ifdef __cplusplus
}
#endif
