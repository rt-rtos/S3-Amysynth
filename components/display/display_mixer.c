#include "display_mixer.h"
#include "display_seq.h"   /* display_drum_track_label */

/* Header in the panel's yellow band (y 0..15) as the menu draws it: title
 * in u8g2_font_6x10_tf at baseline 8, divider at y 15. Rows in
 * u8g2_font_5x7_tr below the seam: row i spans y = MIX_ROW_Y0 + MIX_ROW_H * i
 * .. + 7, name baseline y + 6, so the last row ends at y 56 and the hint
 * strip (y 57..63) stays untouched. */
#define MIX_TITLE_Y   8
#define MIX_ROW_Y0    17
#define MIX_ROW_H     8
#define MIX_NAME_W    21     /* cursor box x 0..20 */
#define MIX_BAR_X     22     /* frame x 22..79, y + 1 .. y + 5 */
#define MIX_BAR_W     58
#define MIX_BAR_H     5
#define MIX_FILL_X    23
#define MIX_FILL_MAX  56
#define MIX_M_X       84     /* M cell x 84..90 */
#define MIX_M_W       7
#define MIX_Q1_X      94     /* quick cells x 94..108 and 110..124 */
#define MIX_Q2_X      110
#define MIX_Q_W       15
#define MIX_SCROLL_X  124

static uint8_t bar_px(uint8_t pos)
{
    if (pos > 100u) pos = 100u;
    return (uint8_t)((pos * MIX_FILL_MAX + 50u) / 100u);
}

/* Text at x + 1 in a w-wide cell, filled behind when `on`; fg is the row's
 * foreground colour (0 on a fully inverted row). Leaves fg selected. */
static void draw_cell(u8g2_t *u8g2, uint8_t x, uint8_t y, uint8_t w,
                      const char *s, bool on, uint8_t fg)
{
    u8g2_SetDrawColor(u8g2, fg);
    if (on) {
        u8g2_DrawBox(u8g2, x, y, w, MIX_ROW_H);
        u8g2_SetDrawColor(u8g2, (uint8_t)!fg);
    }
    u8g2_DrawStr(u8g2, (uint8_t)(x + 1), (uint8_t)(y + 6), s);
    u8g2_SetDrawColor(u8g2, fg);
}

/* Level bar; `inverted` (LEVEL focus) fills the frame and cuts the fill out. */
static void draw_bar(u8g2_t *u8g2, uint8_t y, uint8_t pos, uint8_t fade_to,
                     bool inverted, uint8_t fg)
{
    uint8_t fill = bar_px(pos);
    u8g2_SetDrawColor(u8g2, fg);
    if (inverted) {
        u8g2_DrawBox(u8g2, MIX_BAR_X, (uint8_t)(y + 1), MIX_BAR_W, MIX_BAR_H);
        u8g2_SetDrawColor(u8g2, (uint8_t)!fg);
    } else {
        u8g2_DrawFrame(u8g2, MIX_BAR_X, (uint8_t)(y + 1), MIX_BAR_W, MIX_BAR_H);
    }
    if (fill > 0u) u8g2_DrawBox(u8g2, MIX_FILL_X, (uint8_t)(y + 2), fill, MIX_BAR_H - 2);
    if (fade_to != MIXER_NO_FADE) {
        u8g2_SetDrawColor(u8g2, 2);   /* XOR: visible over fill and gap alike */
        u8g2_DrawVLine(u8g2, (uint8_t)(MIX_FILL_X + bar_px(fade_to)), y, 7);
    }
    u8g2_SetDrawColor(u8g2, fg);
}

static void draw_row(u8g2_t *u8g2, const mixer_view_row_t *r, uint8_t y,
                     bool cur, uint8_t focus)
{
    uint8_t fg = 1;
    if (focus == MIXER_FOCUS_DIVE) {
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawBox(u8g2, 0, y, 128, MIX_ROW_H);
        fg = 0;
    }
    const char *name = (r->drum_track != MIXER_NO_DRUM)
                     ? display_drum_track_label(r->drum_track) : r->name;
    u8g2_SetDrawColor(u8g2, fg);
    if (cur && fg) {
        u8g2_DrawBox(u8g2, 0, y, MIX_NAME_W, MIX_ROW_H);
        u8g2_SetDrawColor(u8g2, 0);
    }
    u8g2_DrawStr(u8g2, 0, (uint8_t)(y + 6), name);
    u8g2_SetDrawColor(u8g2, fg);

    if (r->has_level) {
        draw_bar(u8g2, y, r->pos, r->fade_to, focus == MIXER_FOCUS_LEVEL, fg);
    } else {
        draw_cell(u8g2, (uint8_t)(MIX_BAR_X - 1), y, MIX_BAR_W + 1, r->text,
                  focus == MIXER_FOCUS_LEVEL, fg);
    }

    if (r->has_mute && r->mute) {
        draw_cell(u8g2, MIX_M_X, y, MIX_M_W, "M", fg != 0, fg);
    } else if (r->has_mute && r->hard_mute) {
        draw_cell(u8g2, MIX_M_X, y, MIX_M_W, "m", false, fg);
    }

    if (r->q1[0] != '\0' || focus == MIXER_FOCUS_Q1)
        draw_cell(u8g2, MIX_Q1_X, y, MIX_Q_W, r->q1, focus == MIXER_FOCUS_Q1, fg);
    if (r->q2[0] != '\0' || focus == MIXER_FOCUS_Q2)
        draw_cell(u8g2, MIX_Q2_X, y, MIX_Q_W, r->q2, focus == MIXER_FOCUS_Q2, fg);
    u8g2_SetDrawColor(u8g2, 1);
}

void display_mixer_draw_frame(u8g2_t *u8g2, const mixer_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    if (view == NULL) return;
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(u8g2, 2, MIX_TITLE_Y, view->title);
    u8g2_DrawHLine(u8g2, 0, 15, 128);
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);

    uint8_t n = (view->row_count <= MIXER_VIS_ROWS) ? view->row_count : MIXER_VIS_ROWS;
    for (uint8_t i = 0; i < n; i++) {
        bool cur = (view->cursor == i);
        draw_row(u8g2, &view->rows[i], (uint8_t)(MIX_ROW_Y0 + MIX_ROW_H * i), cur,
                 cur ? view->focus : (uint8_t)MIXER_FOCUS_NONE);
    }

    /* Scroll triangles, 4 px wide by 3 tall, in the right-edge column; the
     * lower one is cut out of a fully inverted last row. */
    if (view->more_above) {
        u8g2_DrawTriangle(u8g2, MIX_SCROLL_X, 3, MIX_SCROLL_X + 3, 3, MIX_SCROLL_X + 1, 1);
    }
    if (view->more_below) {
        bool dive_last = (n == MIXER_VIS_ROWS && view->cursor == MIXER_VIS_ROWS - 1u &&
                          view->focus == MIXER_FOCUS_DIVE);
        u8g2_SetDrawColor(u8g2, dive_last ? 0 : 1);
        u8g2_DrawTriangle(u8g2, MIX_SCROLL_X, 50, MIX_SCROLL_X + 3, 50, MIX_SCROLL_X + 1, 52);
        u8g2_SetDrawColor(u8g2, 1);
    }
}
