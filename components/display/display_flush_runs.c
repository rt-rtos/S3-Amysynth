#include <stdint.h>
#include <string.h>
#include "priv_display_flush.h"

uint8_t display_flush_row_runs(const uint8_t *cur, const uint8_t *prev, uint8_t tiles,
                               display_flush_run_t out[DISPLAY_FLUSH_MAX_RUNS])
{
    uint8_t n = 0;
    for (uint8_t t = 0; t < tiles; t++) {
        if (memcmp(cur + t * 8u, prev + t * 8u, 8) == 0) continue;
        if (n > 0) {
            display_flush_run_t *last = &out[n - 1];
            uint8_t end = (uint8_t)(last->tx + last->tw);
            /* Adjacent, or one unchanged tile apart: one wider transfer costs
             * less than a second addressing sequence. Past the run cap (not
             * reachable for tiles <= 16) the last run absorbs the rest. */
            if (t - end <= 1 || n == DISPLAY_FLUSH_MAX_RUNS) {
                last->tw = (uint8_t)(t - last->tx + 1);
                continue;
            }
        }
        out[n].tx = t;
        out[n].tw = 1;
        n++;
    }
    return n;
}
