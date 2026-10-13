#include "lfo_bank.h"
#include "voice_config.h"   /* voice_lfo_source_apply / _set_rate */
#include "amy.h"            /* amy_sysclock, AMY_TIME_GEQ */
#include "amy_helpers.h"
#include "synth_slots.h"    /* LFO_SYNTH_BASE, LFO_SYNTH_MAX */
#include "sequencer_core.h" /* bar grid, tick -> AMY time */
#include "sequencer.h"      /* sequencer_ticks */
#include "note_div.h"       /* note_div_ticks */
#include "sdkconfig.h"
#include <stdbool.h>
#include <string.h>

#if CONFIG_SEQ_LFO_SHARED

_Static_assert(AMY_SYNTH_COUNT <= LFO_BANK_NONE,
               "LFO synth numbers must stay below LFO_BANK_NONE");

typedef struct {
    bool      used;
    bool      pending;       /* refs reached 0; freed once free_after passes */
    uint16_t  refs;
    seq_lfo_t src;           /* key fields only; the rest zero */
    /* AMY time of this synth's latest phase warp. A synth-addressed event
     * binds to oscs when AMY parses it, so a future-dated warp for a freed
     * synth would land on its osc's next owner: freeing waits past it. The
     * wrap-relative compare is only meaningful for a warp near now (warps go
     * at most one frame ahead), so it is gated on `warped`. */
    bool      warped;
    uint32_t  last_warp_ms;
    uint32_t  free_after;
} lfo_bank_entry_t;

static lfo_bank_entry_t s_bank[LFO_SYNTH_MAX];

/* The bar line the last round of bar warps was scheduled for. */
static uint32_t s_warped_bar;

/* Bar-warp look-ahead: two 50 ms UI frames, so every bar line is seen by at
 * least one lfo_bank_service() before it passes. */
#define LFO_BANK_WARP_AHEAD_US 100000u

/* Not lockable to the bar grid: a carrier whose rate the wobble swings
 * drifts off any grid by design, and one held at the native rate cap
 * (lfo_rate_to_hz below the note division's own frequency at this BPM) runs
 * off its note-division period, so a bar warp would jump it. */
static bool lfo_off_grid(const seq_lfo_t *src, uint16_t bpm)
{
    if (src->wob_depth > 0u && src->wob_reach != WOB_REACH_DEPTH) return true;
    return note_div_hz(src->rate, (float)bpm) > lfo_rate_to_hz(src->rate, bpm);
}

/* The carrier phase (0..1) that `tick` has on the bar grid: phase 0 at the
 * bar baseline and every LFO period (note_div_ticks) from it. */
static float lfo_phase_at(uint32_t tick, note_div_t rate)
{
    int32_t period = (int32_t)note_div_ticks(rate);
    if (period <= 0) return 0.0f;
    int32_t m = (int32_t)(tick - sequencer_core_bar_baseline_tick()) % period;
    if (m < 0) m += period;
    return (float)m / (float)period;
}

/* One phase warp to the carrier (osc 0) of LFO synth n; time 0 = now. */
static void lfo_warp_send(uint8_t n, float phase, uint32_t time)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth      = n;
    e->osc        = 0;
    e->phase_warp = phase;
    if (time != 0u) e->time = time;
    amy_helpers_event_send(e);
}

static void lfo_key_of(const seq_lfo_t *lfo, seq_lfo_t *key)
{
    memset(key, 0, sizeof(*key));
    key->enabled   = true;
    key->wave      = lfo->wave;
    key->rate      = lfo->rate;
    key->wob_rate  = lfo->wob_rate;
    key->wob_depth = lfo->wob_depth;
    key->wob_reach = lfo->wob_reach;
}

static bool lfo_key_eq(const seq_lfo_t *a, const seq_lfo_t *b)
{
    return a->wave == b->wave && a->rate == b->rate &&
           a->wob_rate == b->wob_rate && a->wob_depth == b->wob_depth &&
           a->wob_reach == b->wob_reach;
}

void lfo_bank_init(void)
{
    memset(s_bank, 0, sizeof(s_bank));
}

uint8_t lfo_bank_acquire(const seq_lfo_t *lfo, uint16_t bpm)
{
    if (!lfo || !lfo->enabled) return LFO_BANK_NONE;
    seq_lfo_t key;
    lfo_key_of(lfo, &key);

    int free_i = -1;
    for (int i = 0; i < LFO_SYNTH_MAX; i++) {
        lfo_bank_entry_t *en = &s_bank[i];
        if (!en->used) {
            if (free_i < 0) free_i = i;
            continue;
        }
        if (lfo_key_eq(&en->src, &key)) {
            en->refs++;
            en->pending = false;   /* revive a released entry */
            return (uint8_t)(LFO_SYNTH_BASE + i);
        }
    }
    if (free_i < 0) return LFO_BANK_NONE;

    lfo_bank_entry_t *en = &s_bank[free_i];
    uint8_t n = (uint8_t)(LFO_SYNTH_BASE + free_i);
    /* bus named: amy_helpers routes a synth-creating event with no bus via
     * fx_bus_for_synth(), whose slot map ends below the bank. */
    amy_event *e = amy_helpers_event_begin();
    e->synth          = n;
    e->num_voices     = 1;
    e->oscs_per_voice = 2;
    e->bus            = 0;
    /* Advanced every block by AMY, not only while a consumer renders: a
     * gated row would otherwise slow its LFO by its silent fraction. */
    e->synth_flags    = SYNTH_FLAGS_FREE_RUN_MOD;
    amy_helpers_event_send(e);
    voice_lfo_source_apply(n, 0, 1, &key, bpm);

    memset(en, 0, sizeof(*en));
    en->used = true;
    en->refs = 1;
    en->src  = key;
    /* Bar lock from the first block while the transport plays; the bar
     * warps in lfo_bank_service() hold it there. */
    if (sequencer_core_next_bar_tick() != 0u && !lfo_off_grid(&key, bpm)) {
        lfo_warp_send(n, lfo_phase_at(sequencer_ticks(), key.rate), 0u);
        en->warped       = true;
        en->last_warp_ms = amy_sysclock();
    }
    return n;
}

void lfo_bank_release(uint8_t n)
{
    if (n == LFO_BANK_NONE || n < LFO_SYNTH_BASE ||
        n >= LFO_SYNTH_BASE + LFO_SYNTH_MAX) return;
    lfo_bank_entry_t *en = &s_bank[n - LFO_SYNTH_BASE];
    if (!en->used || en->refs == 0u) return;
    if (--en->refs != 0u) return;
    uint32_t now = amy_sysclock();
    en->free_after = now;
    if (en->warped && AMY_TIME_GEQ(en->last_warp_ms, now))
        en->free_after = en->last_warp_ms;
    en->pending    = true;
}

/* Bar lock: when the next bar line falls inside the look-ahead, warp every
 * live, non-pending, lockable carrier (lfo_off_grid) to its grid phase at
 * that bar's AMY time, once per bar line. Nothing while the transport is
 * stopped. */
static void lfo_bank_bar_warps(void)
{
    uint32_t next_bar = sequencer_core_next_bar_tick();
    if (next_bar == 0u || next_bar == s_warped_bar) return;
    uint32_t ahead = next_bar - sequencer_ticks();
    if ((uint64_t)ahead * amy_global.us_per_tick > LFO_BANK_WARP_AHEAD_US) return;
    uint32_t at  = sequencer_core_tick_to_amy_ms(next_bar);
    uint16_t bpm = sequencer_core_get_bpm();
    for (int i = 0; i < LFO_SYNTH_MAX; i++) {
        lfo_bank_entry_t *en = &s_bank[i];
        if (!en->used || en->pending || lfo_off_grid(&en->src, bpm)) continue;
        lfo_warp_send((uint8_t)(LFO_SYNTH_BASE + i),
                      lfo_phase_at(next_bar, en->src.rate), at);
        en->warped       = true;
        en->last_warp_ms = at;
    }
    s_warped_bar = next_bar;
}

void lfo_bank_service(void)
{
    lfo_bank_bar_warps();
    uint32_t now = amy_sysclock();
    for (int i = 0; i < LFO_SYNTH_MAX; i++) {
        lfo_bank_entry_t *en = &s_bank[i];
        if (!en->used || !en->pending || AMY_TIME_GEQ(en->free_after, now))
            continue;
        /* bus named, as on create: fx_bus_for_synth()'s slot map ends below
         * the bank. */
        amy_event *e = amy_helpers_event_begin();
        e->synth      = (uint8_t)(LFO_SYNTH_BASE + i);
        e->num_voices = 0;
        e->bus        = 0;
        amy_helpers_event_send(e);
        memset(en, 0, sizeof(*en));
    }
}

void lfo_bank_set_bpm(uint16_t bpm)
{
    for (int i = 0; i < LFO_SYNTH_MAX; i++) {
        if (!s_bank[i].used) continue;
        voice_lfo_source_set_rate((uint8_t)(LFO_SYNTH_BASE + i), 0, 1,
                                  &s_bank[i].src, bpm);
    }
}

lfo_bind_t lfo_bank_bind(uint8_t synth, const lfo_target_map_t *map,
                         const seq_lfo_t *lfo, uint16_t bpm, uint8_t *ref)
{
    lfo_bind_t r = LFO_BIND_OFF;
    if (lfo && lfo->enabled && lfo->targets) {
        uint8_t n = lfo_bank_acquire(lfo, bpm);
        if (n != LFO_BANK_NONE) {
            voice_lfo_wire(synth, map->voice_mask, map->pitch_mask,
                           map->scan_mask, lfo, n);
            lfo_bank_release(*ref);   /* after the acquire: see lfo_bank.h */
            *ref = n;
            return LFO_BIND_BANK;
        }
        r = LFO_BIND_FULL;
    }
    voice_lfo_wire(synth, map->voice_mask, map->pitch_mask, map->scan_mask,
                   NULL, LFO_BANK_NONE);
    lfo_bank_unbind(ref);
    return r;
}

void lfo_bank_unbind(uint8_t *ref)
{
    lfo_bank_release(*ref);
    *ref = LFO_BANK_NONE;
}

uint8_t lfo_bank_info(uint8_t i, uint16_t *refs, bool *pending, seq_lfo_t *key)
{
    if (i >= LFO_SYNTH_MAX || !s_bank[i].used) return LFO_BANK_NONE;
    if (refs)    *refs    = s_bank[i].refs;
    if (pending) *pending = s_bank[i].pending;
    if (key)     *key     = s_bank[i].src;
    return (uint8_t)(LFO_SYNTH_BASE + i);
}

#else  /* !CONFIG_SEQ_LFO_SHARED: no bank, every consumer on its stepper */

void lfo_bank_init(void) {}

uint8_t lfo_bank_acquire(const seq_lfo_t *lfo, uint16_t bpm)
{
    (void)lfo; (void)bpm;
    return LFO_BANK_NONE;
}

void lfo_bank_release(uint8_t n) { (void)n; }

void lfo_bank_service(void) {}

void lfo_bank_set_bpm(uint16_t bpm) { (void)bpm; }

lfo_bind_t lfo_bank_bind(uint8_t synth, const lfo_target_map_t *map,
                         const seq_lfo_t *lfo, uint16_t bpm, uint8_t *ref)
{
    (void)synth; (void)map; (void)lfo; (void)bpm;
    *ref = LFO_BANK_NONE;
    return LFO_BIND_FULL;
}

void lfo_bank_unbind(uint8_t *ref) { *ref = LFO_BANK_NONE; }

uint8_t lfo_bank_info(uint8_t i, uint16_t *refs, bool *pending, seq_lfo_t *key)
{
    (void)i; (void)refs; (void)pending; (void)key;
    return LFO_BANK_NONE;
}

#endif
