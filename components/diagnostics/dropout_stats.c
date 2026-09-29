#include "dropout_stats.h"

#if CONFIG_AMYSYNTH_DROPOUT_TS
#include "esp_timer.h"
#endif

/* Single-writer-per-counter (see header contract), read cross-core by the UI
 * task and the idle diagnostics loop: volatile keeps every increment a real
 * store, and aligned 32-bit loads are atomic on Xtensa, so readers may lag by
 * one update but never tear. Always compiled - four rare-branch increments
 * cost nothing, and the DEV-menu dropout bar must work in every build. */
static volatile uint32_t s_wire_zlp;
static volatile uint32_t s_ring_underrun;
static volatile uint32_t s_ring_overrun;
static volatile uint32_t s_render_overrun;
static volatile uint32_t s_chunk_drop;

#if CONFIG_AMYSYNTH_DROPOUT_TS
/* Written only by the wire_zlp writer (TinyUSB task). volatile on the slot
 * array as well as seq keeps the slot store ordered before the seq publish,
 * so a reader that saw seq finds every index below it fully written; the
 * snapshot's seq re-read catches the one remaining hazard (slot reuse while
 * copying). */
static volatile int64_t s_zlp_ts[DROPOUT_TS_RING];
static volatile uint32_t s_zlp_ts_seq;
#endif

void dropout_count_wire_zlp(void)
{
#if CONFIG_AMYSYNTH_DROPOUT_TS
    s_zlp_ts[s_zlp_ts_seq % DROPOUT_TS_RING] = esp_timer_get_time();
    s_zlp_ts_seq = s_zlp_ts_seq + 1;
#endif
    s_wire_zlp++;
}

void dropout_count_ring_overrun(void)
{
    s_ring_overrun++;
}

void dropout_count_render_overrun(uint32_t missed_ticks)
{
    s_render_overrun += missed_ticks;
}

void dropout_stats_get(dropout_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->wire_zlp       = s_wire_zlp;
    out->ring_underrun  = s_ring_underrun;
    out->ring_overrun   = s_ring_overrun;
    out->render_overrun = s_render_overrun;
    out->chunk_drop     = s_chunk_drop;
}

#if CONFIG_AMYSYNTH_DROPOUT_TS
uint32_t dropout_ts_snapshot(int64_t out[DROPOUT_TS_RING], uint32_t *seq_out)
{
    uint32_t seq, again, n;
    do {
        seq = s_zlp_ts_seq;
        n = seq < DROPOUT_TS_RING ? seq : DROPOUT_TS_RING;
        for (uint32_t i = 0; i < n; i++) {
            out[i] = s_zlp_ts[(seq - n + i) % DROPOUT_TS_RING];
        }
        again = s_zlp_ts_seq;
    } while (seq != again);     /* writer advanced mid-copy (<=1 kHz): retry */
    if (seq_out != NULL) {
        *seq_out = seq;
    }
    return n;
}
#endif
