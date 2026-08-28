#include "display_dist.h"
#include <math.h>
#include <stdio.h>

/* Two-panel layout (128x64), geometry deliberately matched to display_lfo.c so
 * the editors read as siblings: left panel a live transfer curve, right column
 * the five parameter rows. All three distortion types are static waveshapers,
 * so the curve IS the effect - the soft knee of CLIP, the fold count of FOLD,
 * the quantization staircase of CRUSH.
 *
 * The curve shows the SHAPER ALONE, at full wet, against a dotted unity
 * diagonal. Mix is deliberately excluded even though folding it in would be
 * mathematically honest (blending dry and wet is memoryless, so the composite
 * is still a valid transfer function): at low Mix every type collapses to a
 * near-diagonal, which is exactly where the shapes differ most and the picture
 * is needed most. Keeping Mix out isolates Drive's effect, holds the picture
 * still while Mix is swept by ear, and loses nothing - Mix is a scalar that
 * reads fine as a number. CRUSH's sample-hold rate has memory and therefore no
 * transfer-curve form at all; it is not drawn either. */
#define DIST_DIV_X      60      /* vertical divider between the two panels */
#define DIST_RCOL_X     64      /* right-column text x                     */

/* Curve box: a square-ish plot inside the left panel, centred on its own
 * origin so the identity diagonal runs corner to corner. */
#define DIST_CV_X0       4      /* left edge of the plot                   */
#define DIST_CV_HW      25      /* half width in pixels (x = -1 .. +1)     */
#define DIST_CV_CY      35      /* vertical centre = output 0              */
#define DIST_CV_HH      19      /* half height in pixels (y = -1 .. +1)    */


/* Float mirror of AMY's dist_process() shaping (components/amy/src/filters.c),
 * minus the wet/dry blend - see the file header on why Mix stays out. Kept in
 * float for the display only: the point is the shape, not bit equivalence with
 * the fixed-point render path. */
static float dist_transfer(const seq_dist_t *d, float x)
{
    if (d->type == 0) return x;            /* OFF: every stage bypassed */

    /* Enabled stages compose in the engine's fixed clip -> fold -> crush
     * order, each re-applying drive as its own pre-gain - exactly what the
     * stacked passes in dist_block do at mix = 1. */
    float drive = (float)d->drive;
    float y = x;
    if (d->type & 1u) {                    /* CLIP: cubic soft knee */
        float v = y * drive;
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        y = v * (1.0f - v * v * 0.33333334f);
    }
    if (d->type & 2u) {                    /* FOLD: triangle wavefolder */
        float v = y * drive;
        float w = v + 1.0f;
        w -= 4.0f * floorf(w * 0.25f);
        y = 1.0f - fabsf(w - 2.0f);
    }
    if (d->type & 4u) {                    /* CRUSH: saturate then quantize */
        float v = y * drive;
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        /* bits == 0 would make the shift below undefined. Every store path
         * clamps to >= 1, but the renderer must not depend on that. */
        if (d->bits >= 1u && d->bits <= 23u) {
            float qscale = (float)(1u << (d->bits - 1u));
            v = floorf(v * qscale + 0.5f) / qscale;
        }
        y = v;
    }
    return y;
}

static void draw_curve(u8g2_t *u8g2, const seq_dist_t *d)
{
    const uint8_t cx = DIST_CV_X0 + DIST_CV_HW;
    const uint8_t y_top = DIST_CV_CY - DIST_CV_HH;
    const uint8_t y_bot = DIST_CV_CY + DIST_CV_HH;

    /* Unity diagonal (y = x), dotted so the curve stays the dominant mark. It
     * is the reference a transfer plot actually wants: the shaper's departure
     * from it IS the effect, and it passes through the origin, so it carries
     * the zero point too - which is why it replaces a plain horizontal axis
     * rather than joining one. */
    for (int i = 0; i <= 2 * DIST_CV_HW; i += 3) {
        int px = DIST_CV_X0 + i;
        int py = DIST_CV_CY + DIST_CV_HH - (i * DIST_CV_HH) / DIST_CV_HW;
        u8g2_DrawPixel(u8g2, px, py);
    }

    int prev_x = 0, prev_y = 0;
    bool have_prev = false;
    for (int i = 0; i <= 2 * DIST_CV_HW; i++) {
        float xn = (float)(i - DIST_CV_HW) / (float)DIST_CV_HW;
        float yn = dist_transfer(d, xn);

        /* Clamp into the box rather than letting a folded/boosted value run
         * off the panel and collide with the divider. */
        int py = DIST_CV_CY - (int)lrintf(yn * (float)DIST_CV_HH);
        if (py < (int)y_top) py = y_top;
        if (py > (int)y_bot) py = y_bot;
        int px = DIST_CV_X0 + i;

        if (have_prev) u8g2_DrawLine(u8g2, prev_x, prev_y, px, py);
        else           u8g2_DrawPixel(u8g2, px, py);
        prev_x = px; prev_y = py; have_prev = true;
    }
}

/* Right-column parameter row ("Label  value"), inverted when selected, with a
 * caret at the far right while adjusting. Same shape as the LFO page's row
 * renderer, minus the scroll caret: five rows always fit. */
static void draw_right_row(u8g2_t *u8g2, uint8_t baseline, const char *text,
                           bool selected, bool editing)
{
    if (selected) {
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawBox(u8g2, DIST_DIV_X + 1, baseline - 7, 127 - DIST_DIV_X, 9);
        u8g2_SetDrawColor(u8g2, 0);
    }
    u8g2_DrawStr(u8g2, DIST_RCOL_X, baseline, text);
    if (selected && editing) {
        u8g2_DrawBox(u8g2, 123, baseline - 6, 3, 6);
    }
    if (selected) u8g2_SetDrawColor(u8g2, 1);
}

void dist_view_draw(u8g2_t *u8g2, const dist_view_t *v)
{
    const seq_dist_t *d = &v->dist;

    /* ── Header bar ── */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    char hdr[20];
    if (v->target_label && v->target_label[0]) {
        snprintf(hdr, sizeof(hdr), "DIST %s", v->target_label);
    } else {
        snprintf(hdr, sizeof(hdr), "DIST L%u T%u%s",
                 v->layer_idx + 1u, v->track_idx + 1u, v->src_badge);
    }
    u8g2_DrawStr(u8g2, 1, 10, hdr);
    u8g2_DrawHLine(u8g2, 0, 13, 128);
    u8g2_DrawVLine(u8g2, DIST_DIV_X, 15, 42);

    /* ── Left panel: live transfer curve ── */
    draw_curve(u8g2, d);

    /* ── Right panel: the five parameter rows ──
     * Same slot pitch as the LFO page; DIST_FLD_COUNT == the slot count, so
     * there is nothing to scroll and no edge carets. */
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
    static const uint8_t slot_y[DIST_FLD_COUNT] = { 22, 31, 39, 47, 55 };
    /* Full labels fit: the widest row, "Type: CRUSH", is 11 glyphs of the 5 px
     * font = 55 px from DIST_RCOL_X, ending at x=118, clear of the adjust caret
     * at x=123. Keep new value strings inside that budget. */
    char buf[16];

    for (uint8_t f = 0; f < DIST_FLD_COUNT; f++) {
        bool sel = (v->cursor == f);
        switch (f) {
            case DIST_FLD_TYPE:
                snprintf(buf, sizeof(buf), "Type: %s", seq_dist_stage_label(d->type));
                break;
            case DIST_FLD_DRIVE:
                snprintf(buf, sizeof(buf), "Drive: %u", (unsigned)d->drive);
                break;
            case DIST_FLD_BITS:
                /* 24 is the no-op ceiling (AMY samples are s8.23), so name it
                 * rather than showing a number that quietly does nothing. */
                if (d->bits >= DIST_BITS_MAX) snprintf(buf, sizeof(buf), "Bits: --");
                else snprintf(buf, sizeof(buf), "Bits: %u", (unsigned)d->bits);
                break;
            case DIST_FLD_RATE:
                if (d->rate <= 1u) snprintf(buf, sizeof(buf), "Rate: --");
                else snprintf(buf, sizeof(buf), "Rate: %u", (unsigned)d->rate);
                break;
            case DIST_FLD_MIX:
                snprintf(buf, sizeof(buf), "Mix: %u%%", (unsigned)d->mix);
                break;
            default:
                buf[0] = '\0';
                break;
        }
        draw_right_row(u8g2, slot_y[f], buf, sel, v->editing);

        /* BITS and RATE only exist for CRUSH; strike them through while the
         * crush stage is off rather than hiding them - the values persist and
         * re-arm the moment it comes back, matching the WOBBLE rows. */
        if (!(d->type & 4u) && (f == DIST_FLD_BITS || f == DIST_FLD_RATE)) {
            u8g2_SetDrawColor(u8g2, sel ? 0 : 1);
            u8g2_DrawHLine(u8g2, DIST_RCOL_X, (uint8_t)(slot_y[f] - 3),
                           u8g2_GetStrWidth(u8g2, buf));
            u8g2_SetDrawColor(u8g2, 1);
        }
    }
}
