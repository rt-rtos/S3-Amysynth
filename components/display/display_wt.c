#include "display_wt.h"

/* Grid: 7 px rows in the blue band, the column header on the first, the
 * four keyframe rows, then RNG on the last row above the hint strip (y 57).
 * Cell boxes as on the FM screen's page 2 (framed while browsing, filled
 * while adjusting). */
#define WT_HDR_Y      21
#define WT_ROW_Y0     28
#define WT_ROW_H      7
#define WT_LABEL_X    1
#define WT_COL_A_X    15
#define WT_COL_B_X    39
#define WT_COL_W      22

/* Waveform panel right of a divider: one pixel column per preview point. */
#define WT_DIV_X      63
#define WT_WAVE_X     64
#define WT_WAVE_TOP   17
#define WT_WAVE_BOT   56
#define WT_WAVE_MID   ((WT_WAVE_TOP + WT_WAVE_BOT) / 2)
#define WT_WAVE_AMP   ((WT_WAVE_BOT - WT_WAVE_TOP) / 2)

static const char *const s_row_labels[WT_KEY_ROWS] = { "SHP", "BRT", "SYN", "PK" };

static void draw_cell(u8g2_t *u8g2, uint8_t bx, uint8_t y, const char *txt, bool on, bool editing)
{
    uint8_t w = (uint8_t)u8g2_GetStrWidth(u8g2, txt);
    uint8_t tx = (uint8_t)(bx + (WT_COL_W - w) / 2);
    if (on && editing) {
        u8g2_DrawBox(u8g2, bx, (uint8_t)(y - 6), WT_COL_W, WT_ROW_H);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, tx, y, txt);
        u8g2_SetDrawColor(u8g2, 1);
    } else {
        if (on) u8g2_DrawFrame(u8g2, bx, (uint8_t)(y - 6), WT_COL_W, WT_ROW_H);
        u8g2_DrawStr(u8g2, tx, y, txt);
    }
}

static uint8_t wave_y(int8_t v)
{
    return (uint8_t)(WT_WAVE_MID - ((int)v * WT_WAVE_AMP) / 127);
}

static void draw_wave(u8g2_t *u8g2, const int8_t *pts)
{
    for (uint8_t i = 0; i + 1u < WT_VIEW_POINTS; i++) {
        u8g2_DrawLine(u8g2, (uint8_t)(WT_WAVE_X + i), wave_y(pts[i]),
                      (uint8_t)(WT_WAVE_X + i + 1u), wave_y(pts[i + 1u]));
    }
}

void display_wt_draw_frame(u8g2_t *u8g2, const wt_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(u8g2, 2, 8, "WT CUSTOM");
    if (view == NULL) return;
    uint8_t cw = (uint8_t)u8g2_GetStrWidth(u8g2, view->clean);
    u8g2_DrawStr(u8g2, (uint8_t)(126 - cw), 8, view->clean);

    u8g2_SetFont(u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(u8g2, (uint8_t)(WT_COL_A_X + WT_COL_W / 2 - 2), WT_HDR_Y, "A");
    u8g2_DrawStr(u8g2, (uint8_t)(WT_COL_B_X + WT_COL_W / 2 - 2), WT_HDR_Y, "B");
    for (uint8_t r = 0; r < WT_KEY_ROWS; r++) {
        uint8_t y = (uint8_t)(WT_ROW_Y0 + r * WT_ROW_H);
        u8g2_DrawStr(u8g2, WT_LABEL_X, y, s_row_labels[r]);
        uint8_t ca = (uint8_t)(WT_CUR_A_SHP + r), cb = (uint8_t)(WT_CUR_B_SHP + r);
        draw_cell(u8g2, WT_COL_A_X, y, view->cells[ca], view->cursor == ca, view->editing);
        draw_cell(u8g2, WT_COL_B_X, y, view->cells[cb], view->cursor == cb, view->editing);
    }
    uint8_t ry = (uint8_t)(WT_ROW_Y0 + WT_KEY_ROWS * WT_ROW_H);
    u8g2_DrawStr(u8g2, WT_LABEL_X, ry, "RNG");
    draw_cell(u8g2, WT_COL_A_X, ry, view->cells[WT_CUR_RNG], view->cursor == WT_CUR_RNG,
              view->editing);

    u8g2_DrawVLine(u8g2, WT_DIV_X, WT_WAVE_TOP, WT_WAVE_BOT - WT_WAVE_TOP + 1);
    for (uint8_t x = WT_WAVE_X; x < 128u; x = (uint8_t)(x + 4u)) {
        u8g2_DrawPixel(u8g2, x, WT_WAVE_MID);
    }
    if (view->show != WT_SHOW_B) draw_wave(u8g2, view->frame0);
    if (view->show != WT_SHOW_A) draw_wave(u8g2, view->frame63);
}
