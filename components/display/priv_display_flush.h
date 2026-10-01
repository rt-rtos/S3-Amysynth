#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Changed-tile run finder behind display_flush (display_flush.h). Pure C with
 * no ESP-IDF or u8g2 dependency, so it also compiles on the host. */

typedef struct {
    uint8_t tx;     /* first tile column of the run */
    uint8_t tw;     /* run width in tiles */
} display_flush_run_t;

/* With tiles <= 16 and single-tile gaps merged, at most 6 runs can occur. */
#define DISPLAY_FLUSH_MAX_RUNS 8

/* cur/prev: one tile row, tiles*8 bytes each. Writes runs of tiles whose 8 bytes differ,
 * ascending tx; two runs separated by exactly one unchanged tile are merged into one.
 * Returns the run count (0 when the rows are equal). tiles <= 16. */
uint8_t display_flush_row_runs(const uint8_t *cur, const uint8_t *prev, uint8_t tiles,
                               display_flush_run_t out[DISPLAY_FLUSH_MAX_RUNS]);

#ifdef __cplusplus
}
#endif
