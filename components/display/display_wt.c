#include "display_wt.h"

/* Top band: two rows of 7 px boxes (baselines y 7 and 15), u8g2_font_4x6_tr.
 * Keyframe tabs and RNG left of the border at x 30, parameter cells of
 * 31 px at x 33, 65 and 97 right of it. The row 1 slot at x 97 stays empty:
 * display_badge_draw() probes the top row for unlit pixels. Every cell has
 * its label at x + 2 and its value right-aligned to end at x + width - 2;
 * the cursor cell is framed while browsing and filled while adjusting. The
 * scan tab replaces the parameter cells with one FRAME cell, drawn filled
 * because the encoder is bound to it, and a ruler on row 2: one position per
 * frame from WT_RULER_X0 to WT_RULER_X1, ticks on frames 0, 32 and 63. */
#define WT_ROW1_Y     7
#define WT_ROW2_Y     15
#define WT_BOX_H      7
#define WT_TAB_W      7
#define WT_TAB_STEP   7
#define WT_BORDER_X   30
#define WT_CELL_W     31
#define WT_RNG_W      29
#define WT_RULER_X0    34
#define WT_RULER_X1    126
#define WT_RULER_Y     12

/* Waveform below the band: one pixel column per preview point. */
#define WT_WAVE_TOP   17
#define WT_WAVE_BOT   56
#define WT_WAVE_MID   ((WT_WAVE_TOP + WT_WAVE_BOT) / 2)
#define WT_WAVE_AMP   ((WT_WAVE_BOT - WT_WAVE_TOP) / 2)

typedef struct {
    uint8_t     x, y, w;
    const char *label;
} wt_cell_pos_t;

/* Indexed by WT_CUR_*. */
static const wt_cell_pos_t s_cells[WT_CUR_COUNT] = {
    [WT_CUR_SHP] = { 33, WT_ROW1_Y, WT_CELL_W, "SHP" },
    [WT_CUR_WID] = { 65, WT_ROW1_Y, WT_CELL_W, "WID" },
    [WT_CUR_BRT] = { 33, WT_ROW2_Y, WT_CELL_W, "BRT" },
    [WT_CUR_SYN] = { 65, WT_ROW2_Y, WT_CELL_W, "SYN" },
    [WT_CUR_PK]  = { 97, WT_ROW2_Y, WT_CELL_W, "PK" },
    [WT_CUR_RNG] = { 0,  WT_ROW2_Y, WT_RNG_W,  "RNG" },
};

static const char *const s_tabs[WT_VIEW_KEYS + 1] = { "A", "M", "B", "S" };
static const wt_cell_pos_t s_scan_cell = { 33, WT_ROW1_Y, 63, "FRAME" };
static const uint8_t s_ruler_ticks[WT_VIEW_KEYS] = { 0, 32, WT_VIEW_FRAMES - 1 };

static void draw_cell(u8g2_t *u8g2, const wt_cell_pos_t *c, const char *val, bool on, bool editing)
{
    uint8_t vw = (uint8_t)u8g2_GetStrWidth(u8g2, val);
    uint8_t vx = (uint8_t)(c->x + c->w - 1 - vw);
    uint8_t top = (uint8_t)(c->y - 6);
    if (on && editing) {
        u8g2_DrawBox(u8g2, c->x, top, c->w, WT_BOX_H);
        u8g2_SetDrawColor(u8g2, 0);
    } else if (on) {
        u8g2_DrawFrame(u8g2, c->x, top, c->w, WT_BOX_H);
    }
    u8g2_DrawStr(u8g2, (uint8_t)(c->x + 2), c->y, c->label);
    u8g2_DrawStr(u8g2, vx, c->y, val);
    u8g2_SetDrawColor(u8g2, 1);
}

static void draw_tabs(u8g2_t *u8g2, uint8_t key)
{
    for (uint8_t k = 0; k <= WT_VIEW_KEYS; k++) {
        uint8_t bx = (uint8_t)(k * WT_TAB_STEP);
        uint8_t tx = (uint8_t)(bx + (WT_TAB_W - u8g2_GetStrWidth(u8g2, s_tabs[k]) + 1) / 2);
        if (k == key) {
            u8g2_DrawBox(u8g2, bx, (uint8_t)(WT_ROW1_Y - 6), WT_TAB_W, WT_BOX_H);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, tx, WT_ROW1_Y, s_tabs[k]);
            u8g2_SetDrawColor(u8g2, 1);
        } else {
            u8g2_DrawStr(u8g2, tx, WT_ROW1_Y, s_tabs[k]);
        }
    }
}

static uint8_t ruler_x(uint8_t frame)
{
    return (uint8_t)(WT_RULER_X0 + (frame * (WT_RULER_X1 - WT_RULER_X0)) / (WT_VIEW_FRAMES - 1));
}

static void draw_ruler(u8g2_t *u8g2, uint8_t frame)
{
    u8g2_DrawHLine(u8g2, WT_RULER_X0, WT_RULER_Y, WT_RULER_X1 - WT_RULER_X0 + 1);
    for (uint8_t i = 0; i < WT_VIEW_KEYS; i++) {
        u8g2_DrawVLine(u8g2, ruler_x(s_ruler_ticks[i]), WT_RULER_Y - 1, 3);
    }
    u8g2_DrawBox(u8g2, (uint8_t)(ruler_x(frame) - 1), WT_RULER_Y - 3, 3, 7);
}

static void draw_centre_line(u8g2_t *u8g2)
{
    for (uint8_t x = 0; x < WT_VIEW_POINTS; x = (uint8_t)(x + 4u)) {
        u8g2_DrawPixel(u8g2, x, WT_WAVE_MID);
    }
}

/* 60 dB over the waveform area's rows: 3 half-dB units per pixel. */
static void draw_harmonics(u8g2_t *u8g2, const uint8_t *harm, uint8_t count)
{
    const uint8_t rows = WT_WAVE_BOT - WT_WAVE_TOP + 1;
    if (count == 0 || count > WT_VIEW_HARMONICS) count = WT_VIEW_HARMONICS;
    uint8_t slot = (uint8_t)(WT_VIEW_POINTS / count);
    uint8_t x0 = (uint8_t)((WT_VIEW_POINTS - slot * count) / 2);
    for (uint8_t db = 20; db < 60; db = (uint8_t)(db + 20)) {
        for (uint8_t x = 0; x < WT_VIEW_POINTS; x = (uint8_t)(x + 4u)) {
            u8g2_DrawPixel(u8g2, x, (uint8_t)(WT_WAVE_TOP + (db * 2u) / 3u));
        }
    }
    for (uint8_t n = 0; n < count; n++) {
        uint8_t drop = (uint8_t)(harm[n] / 3u);
        if (drop >= rows) continue;
        u8g2_DrawBox(u8g2, (uint8_t)(x0 + n * slot), (uint8_t)(WT_WAVE_TOP + drop),
                     (uint8_t)(slot - 1u), (uint8_t)(rows - drop));
    }
}

static uint8_t wave_y(int8_t v)
{
    return (uint8_t)(WT_WAVE_MID - ((int)v * WT_WAVE_AMP) / 127);
}

static void draw_wave_line(u8g2_t *u8g2, const int8_t *pts)
{
    for (uint8_t i = 0; i + 1u < WT_VIEW_POINTS; i++) {
        u8g2_DrawLine(u8g2, i, wave_y(pts[i]), (uint8_t)(i + 1u), wave_y(pts[i + 1u]));
    }
}

static void draw_wave_dots(u8g2_t *u8g2, const int8_t *pts)
{
    for (uint8_t i = 0; i < WT_VIEW_POINTS; i = (uint8_t)(i + 2u)) {
        u8g2_DrawPixel(u8g2, i, wave_y(pts[i]));
    }
}

void display_wt_draw_frame(u8g2_t *u8g2, const wt_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    if (view == NULL) return;
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_4x6_tr);

    uint8_t key = (view->key <= WT_VIEW_KEY_SCAN) ? view->key : 0;
    draw_tabs(u8g2, key);
    u8g2_DrawVLine(u8g2, WT_BORDER_X, 0, WT_ROW2_Y + 1);
    if (key == WT_VIEW_KEY_SCAN) {
        draw_cell(u8g2, &s_cells[WT_CUR_RNG], view->cells[WT_CUR_RNG], false, false);
        draw_cell(u8g2, &s_scan_cell, view->frame_txt, true, true);
        draw_ruler(u8g2, view->frame < WT_VIEW_FRAMES ? view->frame : WT_VIEW_FRAMES - 1);
        if (view->spectrum) {
            draw_harmonics(u8g2, view->harm, view->harm_count);
        } else {
            draw_centre_line(u8g2);
            draw_wave_line(u8g2, view->wave[0]);
        }
        return;
    }
    draw_centre_line(u8g2);
    for (uint8_t c = 0; c < WT_CUR_COUNT; c++) {
        draw_cell(u8g2, &s_cells[c], view->cells[c], view->cursor == c, view->editing);
    }
    for (uint8_t k = 0; k < WT_VIEW_KEYS; k++) {
        if (k != key) draw_wave_dots(u8g2, view->wave[k]);
    }
    draw_wave_line(u8g2, view->wave[key]);
}
