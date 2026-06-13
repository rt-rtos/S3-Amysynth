/*
 * graph_popup_amy — AMY <-> graph_popup adapter.
 *
 * Converts between AMY envelope breakpoint arrays (times in milliseconds,
 * values 0..1) and the normalised point model used by the graph_popup widget.
 * This lives in sequencer_ui (which already REQUIRES amy) so the display
 * component (priv_i2c_u8g2) stays free of any AMY dependency.
 *
 * Normalisation:
 *   x = cumulative time / total time   (so the curve spans the full width)
 *   y = breakpoint value               (already 0..1)
 * An implicit origin point (0,0) is prepended so the curve starts at the
 * bottom-left of the plot, matching how an envelope rises from silence.
 */

#include "graph_popup.h"

#include <stdint.h>

static inline float clampf01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

uint8_t gpopup_points_from_envelope(gpopup_point_t *out, uint8_t max_out,
                                    const uint32_t *times_ms,
                                    const float    *values,
                                    uint8_t         num_bps)
{
    if (!out || max_out == 0) return 0;

    /* Total time = sum of all breakpoint times. Guard against zero. */
    uint32_t total = 0;
    for (uint8_t i = 0; i < num_bps; ++i) {
        total += times_ms ? times_ms[i] : 0;
    }
    if (total == 0) total = 1;  /* avoid divide-by-zero; degenerate flat line */

    uint8_t n = 0;

    /* Implicit origin at (0,0). */
    out[n].x = 0.0f;
    out[n].y = 0.0f;
    n++;

    uint32_t cumulative = 0;
    for (uint8_t i = 0; i < num_bps && n < max_out; ++i) {
        cumulative += times_ms ? times_ms[i] : 0;
        out[n].x = (float)cumulative / (float)total;
        out[n].y = values ? clampf01(values[i]) : 0.0f;
        n++;
    }
    return n;
}

uint8_t gpopup_points_to_envelope(const gpopup_point_t *pts, uint8_t n,
                                  uint32_t  total_ms,
                                  uint32_t *out_times_ms,
                                  float    *out_values)
{
    if (!pts || n == 0 || !out_times_ms || !out_values) return 0;

    /* Skip the implicit origin point at index 0; each subsequent point becomes
     * a breakpoint whose time is the delta from the previous point's x. */
    uint8_t bps = 0;
    float prev_x = pts[0].x;
    for (uint8_t i = 1; i < n; ++i) {
        float dx = pts[i].x - prev_x;
        if (dx < 0.0f) dx = 0.0f;
        out_times_ms[bps] = (uint32_t)(dx * (float)total_ms + 0.5f);
        out_values[bps]   = clampf01(pts[i].y);
        prev_x = pts[i].x;
        bps++;
    }
    return bps;
}
