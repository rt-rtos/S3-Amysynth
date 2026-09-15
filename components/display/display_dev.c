#include "display_dev.h"
#include <stdio.h>

#define DEV_TITLE_Y    8
#define DEV_ROW_H      12
#define DEV_FIRST_ROW  25
#define DEV_VIS_ROWS   3    /* roomy rows below the 16px yellow seam (25/37/49) */

/* Same row idiom as display_stepedit: invert the row when selected+editing,
 * arrow marker when merely selected. */
static void dev_draw_row(u8g2_t *u8g2, uint8_t y, const dev_row_view_t *row,
                         bool selected, bool editing)
{
    char buf[30];
    if (row->value[0] != '\0') {
        snprintf(buf, sizeof(buf), "%-12s: %s", row->label, row->value);
    } else {
        snprintf(buf, sizeof(buf), "%s", row->label);
    }
    if (selected && editing) {
        uint8_t w = (uint8_t)(u8g2_GetStrWidth(u8g2, buf) + 4);
        u8g2_DrawBox(u8g2, 0, (uint8_t)(y - 8), w, DEV_ROW_H);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, 2, y, buf);
        u8g2_SetDrawColor(u8g2, 1);
    } else if (selected) {
        u8g2_DrawStr(u8g2, 8, y, buf);
        u8g2_DrawTriangle(u8g2, 0, (int16_t)(y - 6),
                                0, (int16_t)(y - 1),
                                3, (int16_t)(y - 3));
    } else {
        u8g2_DrawStr(u8g2, 8, y, buf);
    }
}

void display_dev_draw_frame(u8g2_t *u8g2, const dev_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);

    char title[24];
    snprintf(title, sizeof(title), "DEV %s",
             (view && view->title) ? view->title : "");
    u8g2_DrawStr(u8g2, 2, DEV_TITLE_Y, title);
    u8g2_DrawHLine(u8g2, 0, 15, 128);

    if (view == NULL) { return; }

    uint8_t count = view->count;
    if (count > DEV_VIEW_MAX_ROWS) count = DEV_VIEW_MAX_ROWS;
    uint8_t first = 0;
    if (view->cursor >= DEV_VIS_ROWS) {
        first = (uint8_t)(view->cursor - (DEV_VIS_ROWS - 1));
    }
    uint8_t last = (uint8_t)(first + DEV_VIS_ROWS);
    if (last > count) last = count;

    for (uint8_t i = first; i < last; i++) {
        uint8_t y = (uint8_t)(DEV_FIRST_ROW + (i - first) * DEV_ROW_H);
        dev_draw_row(u8g2, y, &view->rows[i],
                     view->cursor == i, view->editing);
    }

    /* Scroll arrows in the yellow header, right-aligned. */
    if (first > 0) {
        u8g2_DrawTriangle(u8g2, 112, 2, 108, 8, 116, 8);
    }
    if (last < count) {
        u8g2_DrawTriangle(u8g2, 118, 2, 126, 2, 122, 8);
    }
}
