#include "display_drone.h"
#include "seq_clamp.h"
#include <stdio.h>

/* Layout mirrors the menu overlay: yellow header bar (rows 0..15) with the
 * title, then up to 3 rows in the blue region below the 16px seam. */
#define DRONE_TITLE_Y    8
#define DRONE_ROW_H      12
#define DRONE_FIRST_ROW  25
#define DRONE_VIS_ROWS   3

void display_drone_draw_frame_titled(u8g2_t *u8g2, const char *title,
                                     const drone_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);

    /* Title bar. */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(u8g2, 2, DRONE_TITLE_Y, title ? title : "DRONE");
    u8g2_DrawHLine(u8g2, 0, 15, 128);

    if (view == NULL || view->count == 0) {
        return;
    }

    /* Compute the scroll window so the cursor is always visible. */
    uint8_t first = 0;
    if (view->cursor >= DRONE_VIS_ROWS) {
        first = (uint8_t)(view->cursor - (DRONE_VIS_ROWS - 1));
    }
    uint8_t last = (uint8_t)(first + DRONE_VIS_ROWS);
    if (last > view->count) last = view->count;

    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    for (uint8_t i = first; i < last; i++) {
        const drone_row_view_t *r = &view->rows[i];
        uint8_t row = (uint8_t)(i - first);
        uint8_t y   = (uint8_t)(DRONE_FIRST_ROW + row * DRONE_ROW_H);
        bool selected = (i == view->cursor);

        if (selected) {
            u8g2_DrawBox(u8g2, 0, (uint8_t)(y - 9), 128, DRONE_ROW_H);
            u8g2_SetDrawColor(u8g2, 0);
        }

        u8g2_DrawStr(u8g2, 2, y, r->label);

        if (r->value[0] != '\0') {
            uint8_t vw = (uint8_t)u8g2_GetStrWidth(u8g2, r->value);
            uint8_t vx = (vw < 124) ? (uint8_t)(126 - vw) : 2;
            if (selected && view->editing) {
                u8g2_DrawFrame(u8g2, (uint8_t)(vx - 2), (uint8_t)(y - 9),
                               (uint8_t)(vw + 4), DRONE_ROW_H);
            }
            u8g2_DrawStr(u8g2, vx, y, r->value);
        }

        if (selected) {
            u8g2_SetDrawColor(u8g2, 1);
        }
    }

    /* Scroll arrows in the yellow header, clear of the seam and hint strip. */
    if (first > 0) {
        u8g2_DrawTriangle(u8g2, 112, 2, 108, 8, 116, 8);
    }
    if (last < view->count) {
        u8g2_DrawTriangle(u8g2, 118, 2, 126, 2, 122, 8);
    }
}

/* ── Drone visualiser overlay ───────────────────────────────────────────────
 *
 * 128x64 dual-colour panel (rows 0..15 yellow, 16..63 blue). No bottom hint
 * strip here, so the blue region runs to row 63. Every section sits below the
 * 16px seam; the Q needle rises at most to row 16.
 *
 *  Y  0- 8  Title "DRONE VIS"   +   rate + sweep speed right-aligned  (yellow)
 *  Y 15     Divider line on the yellow/blue seam
 *           [Q needle 0–5px above the sweep bar at its midpoint, top ≥ row 16]
 *  Y 21-28  SWEEP  [lo────▓▓▓▓▓▓▓────hi]   filled=range, notch=mid
 *  Y 34-41  AMP    [░░░░|████████|░░░░░]   floor|active range|headroom
 *                         ↑DC centre tick;  "!" at right if clip
 *  Y 45-60  PAT    8 step squares; gate fill height per active step
 */

/* Clamp to [0,1] via seq_clamp_f32 so a NaN degrades to 0.0f instead of
 * propagating into the pixel arithmetic. */
static inline float vis_clamp01(float v)
{
    return SEQ_CLAMP_F32(v, 0.0f, 1.0f);
}

void display_drone_vis_draw(u8g2_t *u8g2, const drone_vis_t *vis)
{
    if (!u8g2 || !vis) return;

    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);

    /* ── Title bar ── */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(u8g2, 2, 8, "DRONE VIS");

    /* Rate + sweep-speed label, right-aligned in the title bar. */
    {
        char info[12];
        const char *rn = vis->rate_name ? vis->rate_name : "?";
        snprintf(info, sizeof(info), "%s %ub", rn, (unsigned)vis->sweep_bars);
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        uint8_t iw = (uint8_t)u8g2_GetStrWidth(u8g2, info);
        u8g2_DrawStr(u8g2, (uint8_t)(126 - iw), 8, info);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    }

    u8g2_DrawHLine(u8g2, 0, 15, 128);

    /* ── Shared bar geometry ── */
    const uint8_t BAR_X = 38;
    const uint8_t BAR_W = 88;
    const uint8_t BAR_H = 7;

    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);

    /* ── SWEEP section ── */
    {
        const uint8_t BY = 21;   /* top of sweep bar (needle rises to row 16) */

        u8g2_DrawStr(u8g2, 0, 28, "SWEEP");
        u8g2_DrawFrame(u8g2, BAR_X, BY, BAR_W, BAR_H);

        float lo = vis_clamp01(vis->sweep_lo_norm);
        float hi = vis_clamp01(vis->sweep_hi_norm);

        /* Filled region = sweep range */
        if (hi > lo) {
            uint8_t fx = (uint8_t)(BAR_X + 1 + lo * (float)(BAR_W - 2));
            uint8_t fw = (uint8_t)((hi - lo) * (float)(BAR_W - 2));
            if (fw < 1) fw = 1;
            u8g2_DrawBox(u8g2, fx, (uint8_t)(BY + 1), fw, (uint8_t)(BAR_H - 2));
        }

        /* Inverted notch at sweep midpoint */
        float mid = (lo + hi) * 0.5f;
        uint8_t mx = (uint8_t)(BAR_X + 1 + mid * (float)(BAR_W - 2));
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawVLine(u8g2, mx, (uint8_t)(BY + 1), (uint8_t)(BAR_H - 2));
        u8g2_SetDrawColor(u8g2, 1);

        /* Resonance needle: solid box above the bar at the midpoint.
         * Height 0..5px proportional to Q (0.1..8.0). */
        uint8_t q_h = (uint8_t)(vis->resonance / 8.0f * 5.0f + 0.5f);
        if (q_h > 5) q_h = 5;
        if (q_h > 0) {
            uint8_t nx = (mx > 0) ? (uint8_t)(mx - 1) : 0;
            u8g2_DrawBox(u8g2, nx, (uint8_t)(BY - q_h), 3, q_h);
        }
    }

    /* ── AMP section ── */
    {
        const uint8_t BY = 34;   /* top of amp bar */

        if (vis->wave_mode) {
            u8g2_DrawStr(u8g2, 0, 41, "AMP");
            u8g2_DrawFrame(u8g2, BAR_X, BY, BAR_W, BAR_H);

            float fl = vis_clamp01(vis->amp_floor_norm);
            float cl = vis_clamp01(vis->amp_ceil_norm);
            float dc = vis_clamp01(vis->amp_const);

            /* Solid fill: floor → ceiling (the LFO sweeps through this range) */
            if (cl > fl) {
                uint8_t fx = (uint8_t)(BAR_X + 1 + fl * (float)(BAR_W - 2));
                uint8_t fw = (uint8_t)((cl - fl) * (float)(BAR_W - 2));
                if (fw < 1) fw = 1;
                u8g2_DrawBox(u8g2, fx, (uint8_t)(BY + 1), fw, (uint8_t)(BAR_H - 2));
            }

            /* DC centre tick (inverted column) at amp_const position */
            uint8_t dcx = (uint8_t)(BAR_X + 1 + dc * (float)(BAR_W - 2));
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawVLine(u8g2, dcx, (uint8_t)(BY + 1), (uint8_t)(BAR_H - 2));
            u8g2_SetDrawColor(u8g2, 1);

            /* Clip warning: "!" just right of the bar when ceiling hits 1.0 */
            if (vis->amp_ceil_norm >= 0.99f) {
                u8g2_DrawStr(u8g2, (uint8_t)(BAR_X + BAR_W + 2), 41, "!");
            }
        } else {
            u8g2_DrawStr(u8g2, 0, 41, "AMP  [PATCH]");
        }
    }

    /* ── PATTERN section ── */
    {
        u8g2_DrawStr(u8g2, 0, 55, "PAT");

        const uint8_t SQ_W  = 10;
        const uint8_t SQ_H  = 16;
        const uint8_t SQ_Y  = 45;
        const uint8_t SQ_X0 = BAR_X;

        for (uint8_t s = 0; s < 8; s++) {
            uint8_t sx = (uint8_t)(SQ_X0 + s * (SQ_W + 1));
            bool on = (vis->pattern_mask >> s) & 1u;

            u8g2_DrawFrame(u8g2, sx, SQ_Y, SQ_W, SQ_H);

            if (on) {
                float g = vis_clamp01(vis->gate_len);
                uint8_t fill_h = (uint8_t)(g * (float)(SQ_H - 2));
                if (fill_h < 1) fill_h = 1;
                uint8_t fill_y = (uint8_t)(SQ_Y + 1 + (SQ_H - 2) - fill_h);
                u8g2_DrawBox(u8g2, (uint8_t)(sx + 1), fill_y,
                             (uint8_t)(SQ_W - 2), fill_h);
            }
        }
    }
}
