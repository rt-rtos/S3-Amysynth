#include "display_lfo.h"
#include "display_text.h"   /* display_hz_label */
#include "voice_config.h"   /* WOBBLE dB authoring unit (shared with the editor) */
#include "note_div.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Two-panel layout (128x64): left checklist of the modulation targets (one LFO
 * carrier drives every checked one), right column of shared WAVE/RATE/DEPTH/EN
 * parameters. Both use the 5x7 font at an 8 px pitch, every row's box between
 * the yellow header band (rows 0..15) and the hint strip (y=57): row boxes span
 * baseline-7..baseline+1, first baseline 23 (box from y=16), last 55 (box to
 * y=56). The left panel holds five of the seven checklist
 * rows and scrolls with the cursor, a triangle marking the hidden direction
 * (the step-trig popup's cue). */
#define LFO_DIV_X      60      /* vertical divider between the two panels */
#define LFO_ROW0_Y     23      /* first checklist row baseline            */
#define LFO_ROW_DY      8      /* checklist row pitch                      */
#define LFO_RCOL_X     64      /* right-column text x                      */
#define LFO_SCROLL_X  117      /* scroll caret x (clears the widest row and
                                  the adjust caret at x=123)               */
#define LFO_TGT_CUE_X  55      /* checklist scroll cue x: right of the widest
                                  target label, inside the left panel      */

/* Precomputed y-offsets (from icon center) for 16-pixel-wide wave icons,
 * +/-4 so the icon fits its row's 9 px box. Positive = below center on screen
 * (screen y increases downward). */
#define LFO_ICON_HALF   4
static const int8_t s_wave_icon[LFO_WAVE_COUNT][16] = {
    /* SINE */
    { 0,-2,-3,-4,-4,-4,-3,-2, 0, 2, 3, 4, 4, 4, 3, 2},
    /* TRIANGLE */
    { 4, 3, 2, 1, 0,-1,-2,-3,-4,-3,-2,-1, 0, 1, 2, 3},
    /* SAW_UP  */
    { 4, 4, 3, 3, 2, 2, 1, 0, 0,-1,-2,-2,-3,-3,-4,-4},
    /* SAW_DOWN */
    {-4,-4,-3,-3,-2,-2,-1, 0, 0, 1, 2, 2, 3, 3, 4, 4},
    /* SQUARE */
    {-4,-4,-4,-4,-4,-4,-4,-4, 4, 4, 4, 4, 4, 4, 4, 4},
    /* RANDOM */
    { 2, 2, 2, 2,-3,-3,-3,-3, 1, 1, 1, 1,-2,-2,-2,-2},
};

static const char *target_name(int t)
{
    switch (t) {
        case LFO_TARGET_FILTER: return "Filter";
        case LFO_TARGET_AMP:    return "Amp";
        case LFO_TARGET_PITCH:  return "Pitch";
        case LFO_TARGET_PAN:    return "Pan";
        case LFO_TARGET_SCAN:   return "Scan";
        case LFO_TARGET_DIST_DRIVE: return "Drive";
        case LFO_TARGET_DIST_MIX:   return "Mix";
        default:                return "?";
    }
}

static void draw_wave_icon(u8g2_t *u8g2, uint8_t x0, uint8_t y_center,
                           lfo_wave_t wave, bool invert)
{
    if ((uint8_t)wave >= LFO_WAVE_COUNT) return;
    u8g2_SetDrawColor(u8g2, invert ? 0 : 1);
    for (uint8_t i = 0; i < 16; i++) {
        int16_t y = (int16_t)y_center + s_wave_icon[wave][i];
        if (y >= 0 && y < 64)
            u8g2_DrawPixel(u8g2, x0 + i, (uint8_t)y);
    }
    /* For saw waves, draw the reset edge. */
    if (wave == LFO_WAVE_SAW_UP || wave == LFO_WAVE_SAW_DOWN) {
        u8g2_DrawVLine(u8g2, x0 + 15, y_center - LFO_ICON_HALF, 2 * LFO_ICON_HALF + 1);
    }
    u8g2_SetDrawColor(u8g2, 1);
}

/* Right-column parameter row ("Label  value"), inverted when selected, with a
 * caret at the far right while adjusting. `scroll` is '^'/'v' on the window's
 * edge rows when more parameters lie that way, or 0; drawn inside the row's
 * colour context so it stays legible on the inverted bar. */
static void draw_right_row(u8g2_t *u8g2, uint8_t baseline, const char *text,
                           bool selected, bool editing, char scroll)
{
    if (selected) {
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawBox(u8g2, LFO_DIV_X + 1, baseline - 7, 127 - LFO_DIV_X, 9);
        u8g2_SetDrawColor(u8g2, 0);
    }
    u8g2_DrawStr(u8g2, LFO_RCOL_X, baseline, text);
    if (scroll) {
        char s[2] = { scroll, '\0' };
        u8g2_DrawStr(u8g2, LFO_SCROLL_X, baseline, s);
    }
    if (selected && editing) {
        /* filled caret at the row's right edge = "turn to adjust" */
        u8g2_DrawBox(u8g2, 123, baseline - 6, 3, 6);
    }
    if (selected) u8g2_SetDrawColor(u8g2, 1);
}

void lfo_view_draw(u8g2_t *u8g2, const lfo_view_t *v)
{
    const seq_lfo_t *l = &v->lfo;

    /* ── Header bar ── */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    char hdr[20];
    const char *title = (v->index == 1u) ? "LFO2" : "LFO";
    if (v->target_label && v->target_label[0]) {
        snprintf(hdr, sizeof(hdr), "%s %s", title, v->target_label);
    } else {
        snprintf(hdr, sizeof(hdr), "%s L%u T%u%s", title,
                 v->layer_idx + 1u, v->track_idx + 1u, v->src_badge);
    }
    u8g2_DrawStr(u8g2, 1, 10, hdr);
    uint8_t hdr_w = (uint8_t)u8g2_GetStrWidth(u8g2, hdr);
    uint8_t chip_x;
    /* En chip lives in the header: the right panel is full with the five
     * parameter rows. Cursor on LFO_FLD_EN highlights it. */
    {
        bool en_sel = (v->cursor == LFO_FLD_EN);
        const char *en_txt = l->enabled ? "ON" : "--";
        uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, en_txt);
        chip_x = (uint8_t)(127 - tw - 4);
        if (en_sel) {
            u8g2_DrawBox(u8g2, (uint8_t)(127 - tw - 4), 1, (uint8_t)(tw + 4), 11);
            u8g2_SetDrawColor(u8g2, 0);
        } else {
            u8g2_DrawFrame(u8g2, (uint8_t)(127 - tw - 4), 1, (uint8_t)(tw + 4), 11);
        }
        u8g2_DrawStr(u8g2, (uint8_t)(127 - tw - 2), 10, en_txt);
        if (en_sel) u8g2_SetDrawColor(u8g2, 1);
    }
    /* One display-only chip left of EN, a single header-font glyph: on the
     * LFO1 page "2" while LFO2 is open (inverted while it runs: on, with
     * targets, on a mapped patch); on the LFO2 page an outlined, struck "P"
     * while LFO2 replaces the patch's own LFO. */
    {
        const char *glyph = NULL;
        bool inv = false, strike = false;
        if (v->index == 0u && v->lfo2_open) {
            glyph = "2";
            inv   = v->lfo2_on && v->other_targets != 0u && v->lfo2_avail;
        } else if (v->index == 1u && v->patch_lfo_replaced) {
            glyph  = "P";
            strike = true;
        }
        if (glyph) {
            uint8_t gw = (uint8_t)u8g2_GetStrWidth(u8g2, glyph);
            uint8_t x  = (uint8_t)(chip_x - 2 - (gw + 4));
            if (inv) {
                u8g2_DrawBox(u8g2, x, 1, (uint8_t)(gw + 4), 11);
                u8g2_SetDrawColor(u8g2, 0);
            } else {
                u8g2_DrawFrame(u8g2, x, 1, (uint8_t)(gw + 4), 11);
            }
            u8g2_DrawStr(u8g2, (uint8_t)(x + 2), 10, glyph);
            if (strike) u8g2_DrawHLine(u8g2, (uint8_t)(x + 1), 6, (uint8_t)(gw + 2));
            if (inv) u8g2_SetDrawColor(u8g2, 1);
            chip_x = x;
        }
    }
    /* FILTER swing range ("550-2.2k"), right-aligned against the leftmost
     * header chip in the small font; dropped when a long title leaves no room
     * for it. */
    if (v->flt_lo_hz > 0.0f && (l->targets & LFO_TGT_BIT(LFO_TARGET_FILTER))) {
        char lo[6], hi[6], rng[14];
        display_hz_label(lo, v->flt_lo_hz);
        display_hz_label(hi, v->flt_hi_hz);
        snprintf(rng, sizeof(rng), "%s-%s", lo, hi);
        u8g2_SetFont(u8g2, u8g2_font_4x6_tr);
        int rx = (int)chip_x - 3 - (int)u8g2_GetStrWidth(u8g2, rng);
        if (rx > (int)hdr_w + 3) u8g2_DrawStr(u8g2, (uint8_t)rx, 8, rng);
    }
    u8g2_DrawHLine(u8g2, 0, 13, 128);
    u8g2_DrawVLine(u8g2, LFO_DIV_X, 16, 41);   /* y 16..56: clear of header and hint */

    /* Panels use the compact font so all rows clear the hint strip. */
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);

    /* ── Left panel: target checklist, LFO_TGT_SLOTS rows from tgt_first ── */
    uint8_t tgt_lo = v->tgt_first;
    if (tgt_lo > LFO_TARGET_COUNT - LFO_TGT_SLOTS)
        tgt_lo = (uint8_t)(LFO_TARGET_COUNT - LFO_TGT_SLOTS);
    uint8_t tgt_hi = (uint8_t)(tgt_lo + LFO_TGT_SLOTS);
    for (uint8_t t = tgt_lo; t < tgt_hi; t++) {
        uint8_t base = LFO_ROW0_Y + (uint8_t)(t - tgt_lo) * LFO_ROW_DY;
        bool selected = (v->cursor == t);
        bool on = (l->targets & LFO_TGT_BIT(t)) != 0;

        if (selected) {
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawBox(u8g2, 0, base - 7, LFO_DIV_X, 9);
            u8g2_SetDrawColor(u8g2, 0);
        }
        /* checkbox: filled when the target is active, framed when not; a
         * target the other LFO drives shows that LFO's number instead
         * (checking it moves the target here). */
        if (on) {
            u8g2_DrawBox(u8g2, 2, base - 6, 6, 6);
        } else if (v->other_targets & LFO_TGT_BIT(t)) {
            u8g2_DrawStr(u8g2, 2, base, (v->index == 1u) ? "1" : "2");
        } else {
            u8g2_DrawFrame(u8g2, 2, base - 6, 6, 6);
        }
        u8g2_DrawStr(u8g2, 11, base, target_name(t));
        /* Distortion targets without a serving stepper (drone): inert, struck
         * through like the WOBBLE rows on software tracks. Current colour
         * already matches the row context (0 on the inverted selection bar). */
        if (t >= LFO_TARGET_DIST_DRIVE && v->dist_inert)
            u8g2_DrawHLine(u8g2, 11, (uint8_t)(base - 3),
                           u8g2_GetStrWidth(u8g2, target_name(t)));
        /* Scroll cues on the window's edge rows, in the row's colour: an
         * up triangle when rows lie above, a down one when rows lie below. */
        if (t == tgt_lo && tgt_lo > 0)
            u8g2_DrawTriangle(u8g2, LFO_TGT_CUE_X,     (int16_t)(base - 2),
                                    LFO_TGT_CUE_X + 3, (int16_t)(base - 2),
                                    LFO_TGT_CUE_X + 1, (int16_t)(base - 7));
        if (t == tgt_hi - 1u && tgt_hi < LFO_TARGET_COUNT)
            u8g2_DrawTriangle(u8g2, LFO_TGT_CUE_X,     (int16_t)(base - 7),
                                    LFO_TGT_CUE_X + 3, (int16_t)(base - 7),
                                    LFO_TGT_CUE_X + 1, (int16_t)(base - 2));
        if (selected) u8g2_SetDrawColor(u8g2, 1);
    }

    /* ── Right panel: shared LFO parameters, LFO_PANEL_SLOTS rows on the left
     * panel's baselines. More parameters than slots, so the window scrolls by
     * one when the cursor reaches the last row. ── */
    static const uint8_t slot_y[LFO_PANEL_SLOTS] = {
        LFO_ROW0_Y, LFO_ROW0_Y + LFO_ROW_DY, LFO_ROW0_Y + 2 * LFO_ROW_DY,
        LFO_ROW0_Y + 3 * LFO_ROW_DY, LFO_ROW0_Y + 4 * LFO_ROW_DY,
    };
    char buf[14];

    uint8_t first = 0;   /* first parameter row shown in the window */
    if (v->cursor >= LFO_FLD_WAVE && v->cursor < LFO_FLD_WAVE + LFO_PANEL_ROWS) {
        uint8_t row = (uint8_t)(v->cursor - LFO_FLD_WAVE);
        if (row >= LFO_PANEL_SLOTS)
            first = (uint8_t)(row - LFO_PANEL_SLOTS + 1u);
    }

    for (uint8_t s = 0; s < LFO_PANEL_SLOTS; s++) {
        uint8_t row  = (uint8_t)(first + s);
        uint8_t base = slot_y[s];
        uint8_t fld  = (uint8_t)(LFO_FLD_WAVE + row);
        bool    sel  = (v->cursor == fld);

        /* Edge carets are the only cue that the panel continues off-screen. */
        char scroll = 0;
        if (s == 0 && first > 0)                                    scroll = '^';
        if (s == LFO_PANEL_SLOTS - 1 && row + 1u < LFO_PANEL_ROWS)  scroll = 'v';

        if (fld == LFO_FLD_WAVE) {
            /* WAVE row: label + live waveform icon instead of a value string. */
            if (sel) {
                u8g2_SetDrawColor(u8g2, 1);
                u8g2_DrawBox(u8g2, LFO_DIV_X + 1, base - 7, 127 - LFO_DIV_X, 9);
                u8g2_SetDrawColor(u8g2, 0);
            }
            u8g2_DrawStr(u8g2, LFO_RCOL_X, base, "Wave");
            if (sel && v->editing) u8g2_DrawBox(u8g2, 123, base - 6, 3, 6);  /* color 0 while selected */
            draw_wave_icon(u8g2, 92, base - 3, l->wave, sel);                /* restores color 1 */
            if (sel) u8g2_SetDrawColor(u8g2, 1);
            continue;
        }

        switch (fld) {
            case LFO_FLD_RATE:
                snprintf(buf, sizeof(buf), "Rate %s", note_div_label(l->rate));
                break;
            case LFO_FLD_DEPTH:
                snprintf(buf, sizeof(buf), "Depth %u%%", (unsigned)l->depth);
                break;
            case LFO_FLD_FLT_OCT:
                /* Octaves around the authored cutoff - the unit the log2 filter
                 * rail works in (voice_config.h). Resolves the legacy
                 * depth-derived sentinel, so this is the effective swing. */
                snprintf(buf, sizeof(buf), "Flt %.2foc",
                         (double)voice_lfo_filter_octaves(l));
                break;
            case LFO_FLD_WOB_RATE:
                snprintf(buf, sizeof(buf), "WRt %s", note_div_label((note_div_t)l->wob_rate));
                break;
            case LFO_FLD_WOB_DEPTH: {
                /* Depth reaches speak dB of dip below the authored depth
                 * (voice_config.h); rate-only has no depth breathing to
                 * measure, so it speaks rate-swing octaves instead. */
                uint8_t wob_db = voice_wob_depth_to_db(l->wob_depth);
                if (wob_db == 0) snprintf(buf, sizeof(buf), "Wob OFF");
                else if (l->wob_reach == WOB_REACH_RATE)
                    snprintf(buf, sizeof(buf), "Wob %.2foc",
                             (double)((float)l->wob_depth / 100.0f
                                      * VOICE_WOB_DEPTH_RATE));
                else snprintf(buf, sizeof(buf), "Wob %udB", (unsigned)wob_db);
                break;
            }
            case LFO_FLD_WOB_MODE: {
                /* Which of the carrier's two rails the wobble reaches. */
                static const char *reach_lbl[WOB_REACH_COUNT] =
                    { "Dep+Rt", "Dep", "Rate" };
                uint8_t wr = (l->wob_reach < WOB_REACH_COUNT) ? l->wob_reach : 0;
                snprintf(buf, sizeof(buf), "WTo %s", reach_lbl[wr]);
                break;
            }
            default:
                buf[0] = '\0';
                break;
        }
        draw_right_row(u8g2, base, buf, sel, v->editing, scroll);

        /* WOBBLE rows are inert when the target runs the software stepper
         * (no modulator osc: no shared LFO synth, or a patch without a
         * native carrier): strike them through rather than hide them - the
         * values persist and re-arm the moment the target gets one. Match
         * the row's colour context so the strike stays legible on the
         * inverted selection bar. */
        if (!v->wob_native &&
            (fld == LFO_FLD_WOB_RATE || fld == LFO_FLD_WOB_DEPTH ||
             fld == LFO_FLD_WOB_MODE)) {
            u8g2_SetDrawColor(u8g2, sel ? 0 : 1);
            u8g2_DrawHLine(u8g2, LFO_RCOL_X, (uint8_t)(base - 3),
                           u8g2_GetStrWidth(u8g2, buf));
            u8g2_SetDrawColor(u8g2, 1);
        }
    }
}
