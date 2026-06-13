/*
 * graph_popup — reusable pop-up graph box implementation.
 * See graph_popup.h for the API contract and design notes.
 */

#include "graph_popup.h"

#include <string.h>
#include <stdio.h>

/* ── File-local helpers ─────────────────────────────────────────────────── */

static inline float clampf01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Inset (in px) of the plot region inside the box border. Leaves room for the
 * frame and an optional title strip at the top. */
#define GPOPUP_PAD_X    4
#define GPOPUP_PAD_TOP  4
#define GPOPUP_PAD_BOT  4
#define GPOPUP_TITLE_H  8   /* extra top inset when a title is shown */

/* Encoder step size for value adjustment (normalised units per detent). */
#define GPOPUP_ADJUST_STEP 0.02f

/* Compute the inner plot rectangle (in absolute screen pixels). */
static void plot_rect(const gpopup_t *p,
                      uint8_t *px, uint8_t *py, uint8_t *pw, uint8_t *ph)
{
    /* ADSR style has no internal title strip (host draws the top bar) and uses
     * tighter insets to maximise the full-screen plot; it reserves 3px at the
     * bottom for the margin tick marks. */
    bool adsr = (p->style == GPOPUP_STYLE_ADSR);
    uint8_t top_pad = adsr ? 2 : (GPOPUP_PAD_TOP + (p->title ? GPOPUP_TITLE_H : 0));
    uint8_t bot_pad = adsr ? (GPOPUP_PAD_BOT + 3) : GPOPUP_PAD_BOT;
    uint8_t side_pad = adsr ? 2 : GPOPUP_PAD_X;
    *px = (uint8_t)(p->x + side_pad);
    *py = (uint8_t)(p->y + top_pad);
    *pw = (uint8_t)(p->w - 2 * side_pad);
    *ph = (uint8_t)(p->h - top_pad - bot_pad);
}

/* Map a normalised point to absolute screen pixels within the plot rect.
 * Y is inverted (screen y grows downward). */
static void point_to_px(const gpopup_t *p, const gpopup_point_t *pt,
                        uint8_t plot_x, uint8_t plot_y,
                        uint8_t plot_w, uint8_t plot_h,
                        int *out_x, int *out_y)
{
    float xn = clampf01(pt->x);
    float yn = clampf01(pt->y);
    *out_x = (int)(plot_x + xn * (plot_w - 1));
    *out_y = (int)(plot_y + (1.0f - yn) * (plot_h - 1));
}

/* Forward declarations for the ADSR helpers (defined below). */
static void adsr_apply_constraints(gpopup_t *p);
static bool adsr_y_editable(const gpopup_t *p, uint8_t idx);

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

void graph_popup_init(gpopup_t *p, uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->x = x;
    p->y = y;
    p->w = w;
    p->h = h;
    p->mode = GPOPUP_MODE_VIEW;
    p->active = false;
}

void graph_popup_set_points(gpopup_t *p, const gpopup_point_t *pts, uint8_t n)
{
    if (!p) return;
    if (n > GPOPUP_MAX_POINTS) n = GPOPUP_MAX_POINTS;
    if (pts) {
        for (uint8_t i = 0; i < n; ++i) {
            p->points[i].x = clampf01(pts[i].x);
            p->points[i].y = clampf01(pts[i].y);
        }
    }
    p->num_points = n;
    if (p->cursor >= n) p->cursor = (n > 0) ? (uint8_t)(n - 1) : 0;
    /* Keep the cursor off the fixed origin in ADSR style. */
    if (p->style == GPOPUP_STYLE_ADSR && n >= 2 && p->cursor == 0) p->cursor = 1;
    adsr_apply_constraints(p);
}

void graph_popup_open(gpopup_t *p, gpopup_mode_t mode, const char *title)
{
    if (!p) return;
    p->mode = mode;
    p->title = title;
    p->cursor = 0;
    p->editing_value = false;
    p->adjust_axis_y = true;
    p->active = true;
}

void graph_popup_set_style(gpopup_t *p, gpopup_style_t style)
{
    if (!p) return;
    p->style = style;
}

void graph_popup_set_ticks(gpopup_t *p, const float *xs, uint8_t n)
{
    if (!p) return;
    if (n > GPOPUP_MAX_POINTS) n = GPOPUP_MAX_POINTS;
    if (xs) {
        for (uint8_t i = 0; i < n; ++i) p->ticks[i] = clampf01(xs[i]);
    } else {
        n = 0;
    }
    p->num_ticks = n;
}

/* ── ADSR role-aware constraints ─────────────────────────────────────────────
 * Points are [origin, A(attack peak), D(decay end / sustain level), R(release
 * end)]. Enforce a musically-valid shape so the user cannot create nonsensical
 * curves:
 *   - X is monotonic with a tiny minimum gap so points can't cross or collapse.
 *   - origin pinned to (0,0); A.y pinned to 1.0; R.y pinned to 0.0.
 *   - only the D point's Y (the sustain level) is freely movable.
 * Index roles assume the standard 4-point ADSR seed used by the host. */
#define GPOPUP_MIN_X_GAP 0.02f   /* minimum normalised spacing between points */

static void adsr_apply_constraints(gpopup_t *p)
{
    if (p->style != GPOPUP_STYLE_ADSR || p->num_points < 4) return;

    gpopup_point_t *pt = p->points;

    /* Role-fixed Y levels. */
    pt[0].x = 0.0f; pt[0].y = 0.0f;   /* origin       */
    pt[1].y = 1.0f;                   /* attack peak  */
    pt[3].y = 0.0f;                   /* release end  */
    /* pt[2].y is the sustain level — left as edited, just bounded 0..1. */
    pt[2].y = clampf01(pt[2].y);

    /* Monotonic X with a minimum gap, walking forward from the fixed origin.
     * Each editable point is bounded below by its predecessor + gap and above
     * by 1.0; a second backward pass keeps an earlier point from being pushed
     * past a later one when the later point was dragged left. */
    for (uint8_t i = 1; i < p->num_points; ++i) {
        float lo = pt[i - 1].x + GPOPUP_MIN_X_GAP;
        if (pt[i].x < lo) pt[i].x = lo;
        if (pt[i].x > 1.0f) pt[i].x = 1.0f;
    }
    for (int i = (int)p->num_points - 2; i >= 1; --i) {
        float hi = pt[i + 1].x - GPOPUP_MIN_X_GAP;
        if (pt[i].x > hi) pt[i].x = hi;
        if (pt[i].x < 0.0f) pt[i].x = 0.0f;
    }
}

/* Whether the selected point's Y is editable in ADSR style. Only the sustain
 * point (index 2) has a free level; A/R levels are role-fixed. */
static bool adsr_y_editable(const gpopup_t *p, uint8_t idx)
{
    if (p->style != GPOPUP_STYLE_ADSR) return true;
    return (idx == 2);
}

void graph_popup_close(gpopup_t *p)
{
    if (!p) return;
    p->active = false;
    p->editing_value = false;
}

bool graph_popup_is_active(const gpopup_t *p)
{
    return p && p->active;
}

uint8_t graph_popup_get_points(const gpopup_t *p, gpopup_point_t *out, uint8_t max_out)
{
    if (!p) return 0;
    uint8_t n = p->num_points;
    if (out) {
        if (n > max_out) n = max_out;
        memcpy(out, p->points, n * sizeof(gpopup_point_t));
    }
    return n;
}

/* ── Render ─────────────────────────────────────────────────────────────── */

/* Interpolate the curve's screen-Y at a given screen column, walking the
 * point segments. Returns the baseline if the column is outside the curve. */
static int curve_y_at_col(const gpopup_t *p, int col,
                          uint8_t plot_x, uint8_t plot_y,
                          uint8_t plot_w, uint8_t plot_h)
{
    int baseline = (int)(plot_y + plot_h - 1);
    int px0, py0, px1, py1;
    point_to_px(p, &p->points[0], plot_x, plot_y, plot_w, plot_h, &px0, &py0);
    if (col <= px0) return py0;
    for (uint8_t i = 1; i < p->num_points; ++i) {
        point_to_px(p, &p->points[i], plot_x, plot_y, plot_w, plot_h, &px1, &py1);
        if (col <= px1) {
            int span = px1 - px0;
            if (span <= 0) return py1;
            /* Linear interpolation between the two bracketing points. */
            return py0 + ((py1 - py0) * (col - px0)) / span;
        }
        px0 = px1; py0 = py1;
    }
    (void)baseline;
    return py1; /* past the last point: hold its level */
}

void graph_popup_draw(u8g2_t *u8g2, const gpopup_t *p)
{
    if (!u8g2 || !p || !p->active) return;

    const bool adsr = (p->style == GPOPUP_STYLE_ADSR);

    /* Background clear of the box area. ADSR style is full-screen and framed by
     * the host's top bar, so it skips the rounded border for max plot area. */
    u8g2_SetDrawColor(u8g2, 0);
    u8g2_DrawBox(u8g2, p->x, p->y, p->w, p->h);
    u8g2_SetDrawColor(u8g2, 1);
    if (!adsr) {
        u8g2_DrawRFrame(u8g2, p->x, p->y, p->w, p->h, 2);
    }

    /* Optional title (PLAIN style only; ADSR uses the host top bar). */
    if (p->title && !adsr) {
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        u8g2_DrawStr(u8g2, (uint8_t)(p->x + 3), (uint8_t)(p->y + 7), p->title);
    }

    uint8_t plot_x, plot_y, plot_w, plot_h;
    plot_rect(p, &plot_x, &plot_y, &plot_w, &plot_h);

    int baseline = (int)(plot_y + plot_h - 1);

    /* Axes: bottom + left of the plot region. */
    u8g2_DrawHLine(u8g2, plot_x, (uint8_t)baseline, plot_w);
    u8g2_DrawVLine(u8g2, plot_x, plot_y, plot_h);

    if (p->num_points == 0) return;

    /* ADSR: fill the area under the curve before the line/markers so points and
     * the crisp curve edge sit on top. One VLine per column from the curve down
     * to the baseline. */
    if (adsr) {
        int last_x;
        {
            int lx, ly;
            point_to_px(p, &p->points[p->num_points - 1], plot_x, plot_y,
                        plot_w, plot_h, &lx, &ly);
            last_x = lx;
        }
        for (int cx = (int)plot_x; cx <= last_x; ++cx) {
            int cy = curve_y_at_col(p, cx, plot_x, plot_y, plot_w, plot_h);
            if (cy < (int)plot_y) cy = (int)plot_y;
            if (cy > baseline) cy = baseline;
            if (baseline - cy > 0) {
                u8g2_DrawVLine(u8g2, (uint8_t)cx, (uint8_t)cy,
                               (uint8_t)(baseline - cy));
            }
        }
    }

    /* Curve: connect consecutive points (drawn on top of any fill). */
    int prev_x = 0, prev_y = 0;
    for (uint8_t i = 0; i < p->num_points; ++i) {
        int cx, cy;
        point_to_px(p, &p->points[i], plot_x, plot_y, plot_w, plot_h, &cx, &cy);
        if (i > 0) {
            u8g2_DrawLine(u8g2, prev_x, prev_y, cx, cy);
        }
        prev_x = cx;
        prev_y = cy;
    }

    /* ADSR: minimal time tick marks in the bottom margin (just under the axis).
     * Spacing reflects the host's (possibly non-linear) time->X mapping. */
    if (adsr && p->num_ticks > 0) {
        int ty = baseline + 1;
        for (uint8_t i = 0; i < p->num_ticks; ++i) {
            int tx = (int)(plot_x + clampf01(p->ticks[i]) * (plot_w - 1));
            u8g2_DrawVLine(u8g2, (uint8_t)tx, (uint8_t)ty, 2);
        }
    }

    /* Edit-mode markers + cursor highlight. */
    if (p->mode == GPOPUP_MODE_EDIT) {
        /* ADSR letter labels for the editable points (A, D/S, R). */
        static const char adsr_letters[4] = { 0, 'A', 'S', 'R' };

        for (uint8_t i = 0; i < p->num_points; ++i) {
            int cx, cy;
            point_to_px(p, &p->points[i], plot_x, plot_y, plot_w, plot_h, &cx, &cy);

            /* Label above-left of the point (ADSR style, skip the origin). */
            if (adsr && i > 0 && i < 4 && adsr_letters[i]) {
                char lb[2] = { adsr_letters[i], 0 };
                u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
                int lx = cx - 2;
                int ly = cy - 4;            /* sit above the disc */
                if (ly < (int)plot_y + 6) ly = cy + 11; /* flip below if cramped */
                if (lx < (int)plot_x) lx = (int)plot_x;
                if (lx > (int)(plot_x + plot_w - 5)) lx = (int)(plot_x + plot_w - 5);
                if (i == p->cursor) {
                    /* Emphasise the selected label with an inverted pad. */
                    u8g2_DrawBox(u8g2, (uint8_t)(lx - 1), (uint8_t)(ly - 6), 7, 8);
                    u8g2_SetDrawColor(u8g2, 0);
                    u8g2_DrawStr(u8g2, (uint8_t)lx, (uint8_t)ly, lb);
                    u8g2_SetDrawColor(u8g2, 1);
                } else {
                    u8g2_DrawStr(u8g2, (uint8_t)lx, (uint8_t)ly, lb);
                }
            }

            if (i == p->cursor) {
                /* Selected point: filled disc; when adjusting, ring around it. */
                u8g2_DrawDisc(u8g2, cx, cy, 2, U8G2_DRAW_ALL);
                if (p->editing_value) {
                    u8g2_DrawCircle(u8g2, cx, cy, 4, U8G2_DRAW_ALL);
                }
            } else if (!adsr || i > 0) {
                /* Skip a marker on the fixed origin in ADSR style. */
                u8g2_DrawFrame(u8g2, cx - 1, cy - 1, 3, 3);
            }
        }

        /* Value readout for the selected point while adjusting (PLAIN style).
         * In ADSR style the host top bar shows the real ms/percent readout, so
         * the in-plot text is suppressed to keep the plot clean. */
        if (!adsr && p->editing_value && p->cursor < p->num_points) {
            char buf[16];
            const gpopup_point_t *pt = &p->points[p->cursor];
            snprintf(buf, sizeof(buf), "%c %d,%d",
                     p->adjust_axis_y ? 'V' : 'H',
                     (int)(clampf01(pt->x) * 100.0f + 0.5f),
                     (int)(clampf01(pt->y) * 100.0f + 0.5f));
            u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
            /* Bottom-right inside the box. */
            uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, buf);
            u8g2_DrawStr(u8g2,
                         (uint8_t)(p->x + p->w - tw - 3),
                         (uint8_t)(p->y + p->h - 3),
                         buf);
        }
    }
}

/* ── Input ──────────────────────────────────────────────────────────────── */

gpopup_result_t graph_popup_handle_encoder(gpopup_t *p, long delta)
{
    if (!p || !p->active || delta == 0) return GPOPUP_RESULT_NONE;
    if (p->mode != GPOPUP_MODE_EDIT || p->num_points == 0) {
        return GPOPUP_RESULT_NONE;
    }

    /* In ADSR style the origin (index 0) is fixed and not selectable. */
    long min_idx = (p->style == GPOPUP_STYLE_ADSR) ? 1 : 0;

    if (!p->editing_value) {
        /* Selecting: move the cursor between points, clamped to ends. */
        long idx = (long)p->cursor + delta;
        if (idx < min_idx) idx = min_idx;
        if (idx > (long)p->num_points - 1) idx = (long)p->num_points - 1;
        p->cursor = (uint8_t)idx;
    } else {
        /* Adjusting: change the selected point along the active axis. */
        gpopup_point_t *pt = &p->points[p->cursor];
        float step = (float)delta * GPOPUP_ADJUST_STEP;
        if (p->adjust_axis_y) {
            /* Role-fixed levels (A peak, R end) ignore Y nudges in ADSR mode. */
            if (adsr_y_editable(p, p->cursor)) {
                pt->y = clampf01(pt->y + step);
            }
        } else {
            pt->x = clampf01(pt->x + step);
        }
        /* Re-impose ADSR validity (monotonic time, role-fixed levels). */
        adsr_apply_constraints(p);
    }
    return GPOPUP_RESULT_NONE;
}

gpopup_result_t graph_popup_handle_button(gpopup_t *p)
{
    if (!p || !p->active) return GPOPUP_RESULT_NONE;
    if (p->mode != GPOPUP_MODE_EDIT) {
        /* VIEW mode: short press confirms (dismiss the preview). */
        return GPOPUP_RESULT_CONFIRMED;
    }
    /* EDIT mode: toggle select <-> adjust on the current point. */
    p->editing_value = !p->editing_value;
    return GPOPUP_RESULT_NONE;
}

gpopup_result_t graph_popup_handle_button_long(gpopup_t *p)
{
    if (!p || !p->active) return GPOPUP_RESULT_NONE;
    /* Long press always cancels, from any mode/state. */
    return GPOPUP_RESULT_CANCELLED;
}

bool graph_popup_toggle_axis(gpopup_t *p)
{
    if (!p || !p->active || p->mode != GPOPUP_MODE_EDIT) return false;
    p->adjust_axis_y = !p->adjust_axis_y;
    return true;
}

bool graph_popup_axis_is_y(const gpopup_t *p)
{
    /* Default to Y when not editable so callers reading state are sane. */
    if (!p) return true;
    return p->adjust_axis_y;
}
