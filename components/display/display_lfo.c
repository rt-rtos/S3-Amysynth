#include "display_lfo.h"
#include "voice_config.h"   /* WOBBLE dB authoring unit (shared with the editor) */
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Two-panel layout (128x64): left checklist of the modulation targets (one LFO
 * carrier drives every checked one), right column of shared WAVE/RATE/DEPTH/EN
 * parameters. Both use the 5x7 font at a tight pitch so every row ends above
 * the hint strip at y=57. The panel holds five checklist rows, so the targets
 * are split across two tabs - the five carrier targets, then the distortion
 * pair - and the shoulder button flips between them. */
#define LFO_DIV_X      60      /* vertical divider between the two panels */
#define LFO_ROW0_Y     21      /* first checklist row baseline            */
#define LFO_ROW_DY      8      /* checklist row pitch                      */
#define LFO_RCOL_X     64      /* right-column text x                      */
#define LFO_SCROLL_X  117      /* scroll caret x (clears the widest row and
                                  the adjust caret at x=123)               */
#define LFO_TAB_X      45      /* tab marker x: right of the widest target
                                  label, still inside the left panel       */
#define LFO_TAB_Y      53      /* tab marker baseline = last checklist row  */

/* Precomputed y-offsets (from icon center) for 16-pixel-wide wave icons.
 * Positive = below center on screen (screen y increases downward). */
static const int8_t s_wave_icon[LFO_WAVE_COUNT][16] = {
    /* SINE */
    { 0,-3,-5,-6,-7,-6,-5,-3, 0, 3, 5, 6, 7, 6, 5, 3},
    /* TRIANGLE */
    { 7, 5, 4, 2, 0,-2,-4,-6,-7,-6,-4,-2, 0, 2, 4, 6},
    /* SAW_UP  */
    { 7, 6, 5, 4, 3, 2, 1, 0,-1,-2,-3,-4,-5,-6,-7,-7},
    /* SAW_DOWN */
    {-7,-6,-5,-4,-3,-2,-1, 0, 1, 2, 3, 4, 5, 6, 7, 7},
    /* SQUARE */
    {-7,-7,-7,-7,-7,-7,-7,-7, 7, 7, 7, 7, 7, 7, 7, 7},
    /* RANDOM */
    { 4, 4, 4, 4,-5,-5,-5,-5, 2, 2, 2, 2,-4,-4,-4,-4},
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

static const char *rate_label(lfo_rate_t r)
{
    switch (r) {
        case LFO_RATE_1_8:   return "1/8";
        case LFO_RATE_1_4:   return "1/4";
        case LFO_RATE_1_2:   return "1/2";
        case LFO_RATE_1BAR:  return "1BR";
        case LFO_RATE_2BAR:  return "2BR";
        case LFO_RATE_4BAR:  return "4BR";
        case LFO_RATE_1_16:  return "1/16";
        case LFO_RATE_1_32:  return "1/32";
        case LFO_RATE_1_4T:  return "1/4T";
        case LFO_RATE_1_8T:  return "1/8T";
        case LFO_RATE_1_16T: return "1/16T";
        case LFO_RATE_1_32T: return "1/32T";
        default:             return "???";
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
        u8g2_DrawVLine(u8g2, x0 + 15, y_center - 7, 14);
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
    if (v->target_label && v->target_label[0]) {
        snprintf(hdr, sizeof(hdr), "LFO %s", v->target_label);
    } else {
        snprintf(hdr, sizeof(hdr), "LFO L%u T%u%s",
                 v->layer_idx + 1u, v->track_idx + 1u, v->src_badge);
    }
    u8g2_DrawStr(u8g2, 1, 10, hdr);
    /* En chip lives in the header: the right panel is full with the five
     * parameter rows. Cursor on LFO_FLD_EN highlights it. */
    {
        bool en_sel = (v->cursor == LFO_FLD_EN);
        const char *en_txt = l->enabled ? "ON" : "--";
        uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, en_txt);
        if (en_sel) {
            u8g2_DrawBox(u8g2, (uint8_t)(127 - tw - 4), 1, (uint8_t)(tw + 4), 11);
            u8g2_SetDrawColor(u8g2, 0);
        } else {
            u8g2_DrawFrame(u8g2, (uint8_t)(127 - tw - 4), 1, (uint8_t)(tw + 4), 11);
        }
        u8g2_DrawStr(u8g2, (uint8_t)(127 - tw - 2), 10, en_txt);
        if (en_sel) u8g2_SetDrawColor(u8g2, 1);
    }
    u8g2_DrawHLine(u8g2, 0, 13, 128);
    u8g2_DrawVLine(u8g2, LFO_DIV_X, 15, 42);

    /* Panels use the compact font so all rows clear the hint strip. */
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);

    /* ── Left panel: target checklist, one tab at a time ── */
    bool     dist_tab = (v->tgt_tab != 0);
    uint8_t  tgt_lo   = dist_tab ? LFO_TARGET_DIST_DRIVE : 0;
    uint8_t  tgt_hi   = dist_tab ? LFO_TARGET_COUNT : LFO_TARGET_DIST_DRIVE;
    for (uint8_t t = tgt_lo; t < tgt_hi; t++) {
        uint8_t base = LFO_ROW0_Y + (uint8_t)(t - tgt_lo) * LFO_ROW_DY;
        bool selected = (v->cursor == t);
        bool on = (l->targets & LFO_TGT_BIT(t)) != 0;

        if (selected) {
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawBox(u8g2, 0, base - 7, LFO_DIV_X, 9);
            u8g2_SetDrawColor(u8g2, 0);
        }
        /* checkbox: filled when the target is active, framed when not */
        if (on) u8g2_DrawBox(u8g2, 2, base - 6, 6, 6);
        else    u8g2_DrawFrame(u8g2, 2, base - 6, 6, 6);
        u8g2_DrawStr(u8g2, 11, base, target_name(t));
        /* Distortion targets without a serving stepper (drone): inert, struck
         * through like the WOBBLE rows on software tracks. Current colour
         * already matches the row context (0 on the inverted selection bar). */
        if (dist_tab && v->dist_inert)
            u8g2_DrawHLine(u8g2, 11, (uint8_t)(base - 3),
                           u8g2_GetStrWidth(u8g2, target_name(t)));
        if (selected) u8g2_SetDrawColor(u8g2, 1);
    }

    /* Tab marker: the only cue that half the targets live behind the shoulder
     * button. It shares the bottom checklist row, which on tab 1 is empty and
     * on tab 0 may carry the selection bar - so follow that row's colour. */
    {
        bool last_row_sel = (!dist_tab && v->cursor == LFO_TARGET_SCAN);
        u8g2_SetDrawColor(u8g2, last_row_sel ? 0 : 1);
        u8g2_DrawStr(u8g2, LFO_TAB_X, LFO_TAB_Y, dist_tab ? "2/2" : "1/2");
        u8g2_SetDrawColor(u8g2, 1);
    }

    /* ── Right panel: shared LFO parameters, LFO_PANEL_SLOTS rows at 8 px
     * pitch. More parameters than slots, so the window scrolls by one when the
     * cursor reaches the last row. ── */
    static const uint8_t slot_y[LFO_PANEL_SLOTS] = { 22, 31, 39, 47, 55 };
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
            u8g2_DrawStr(u8g2, LFO_RCOL_X, base, "Wav");
            if (sel && v->editing) u8g2_DrawBox(u8g2, 123, base - 6, 3, 6);  /* color 0 while selected */
            draw_wave_icon(u8g2, 92, base - 6, l->wave, sel);                /* restores color 1 */
            if (sel) u8g2_SetDrawColor(u8g2, 1);
            continue;
        }

        switch (fld) {
            case LFO_FLD_RATE:
                snprintf(buf, sizeof(buf), "Rte %s", rate_label(l->rate));
                break;
            case LFO_FLD_DEPTH:
                snprintf(buf, sizeof(buf), "Dep %u%%", (unsigned)l->depth);
                break;
            case LFO_FLD_FLT_OCT:
                /* Octaves around the authored cutoff - the unit the log2 filter
                 * rail works in (voice_config.h). Resolves the legacy
                 * depth-derived sentinel, so this is the effective swing. */
                snprintf(buf, sizeof(buf), "Flt %.2foc",
                         (double)voice_lfo_filter_octaves(l));
                break;
            case LFO_FLD_WOB_RATE:
                snprintf(buf, sizeof(buf), "WRt %s", rate_label((lfo_rate_t)l->wob_rate));
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
         * (patch strings without a native carrier): strike them through
         * rather than hide them - the values persist and re-arm the moment
         * the patch turns native. Match the row's colour context so the
         * strike stays legible on the inverted selection bar. */
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
