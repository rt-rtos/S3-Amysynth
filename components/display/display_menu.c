#include "display_menu.h"
#include <string.h>

/* Layout: yellow header bar (rows 0..15 on the dual-colour panel) with the
 * title, then up to 3 rows below. The first row starts far enough down that
 * neither its text nor its highlight crosses the 16px yellow/blue seam. */
#define MENU_TITLE_Y    8
#define MENU_ROW_H      12
#define MENU_FIRST_ROW  25
#define MENU_VIS_ROWS   3

void display_menu_draw_frame_titled(u8g2_t *u8g2, const char *title,
                                    const menu_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);

    /* Title bar. */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(u8g2, 2, MENU_TITLE_Y, title ? title : "MENU");
    u8g2_DrawHLine(u8g2, 0, 15, 128);

    if (view == NULL || view->count == 0) {
        return;
    }

    /* Compute the scroll window so the cursor is always visible. */
    uint8_t first = 0;
    if (view->cursor >= MENU_VIS_ROWS) {
        first = (uint8_t)(view->cursor - (MENU_VIS_ROWS - 1));
    }
    uint8_t last = (uint8_t)(first + MENU_VIS_ROWS);
    if (last > view->count) last = view->count;

    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    for (uint8_t i = first; i < last; i++) {
        const menu_item_view_t *it = &view->items[i];
        uint8_t row = (uint8_t)(i - first);
        uint8_t y   = (uint8_t)(MENU_FIRST_ROW + row * MENU_ROW_H);
        bool selected = (i == view->cursor);

        if (selected) {
            /* Highlight the whole row (filled bar, inverted text). */
            u8g2_DrawBox(u8g2, 0, (uint8_t)(y - 9), 128, MENU_ROW_H);
            u8g2_SetDrawColor(u8g2, 0);
        }

        u8g2_DrawStr(u8g2, 2, y, it->label);

        if (it->value[0] != '\0') {
            uint8_t vw = (uint8_t)u8g2_GetStrWidth(u8g2, it->value);
            uint8_t vx = (vw < 124) ? (uint8_t)(126 - vw) : 2;
            /* Frame the value to signal "live". The row highlight leaves the
             * draw color at 0, so the frame reads as a cut-out box. */
            if (selected && view->editing) {
                u8g2_DrawFrame(u8g2, (uint8_t)(vx - 2), (uint8_t)(y - 9),
                               (uint8_t)(vw + 4), MENU_ROW_H);
            }
            u8g2_DrawStr(u8g2, vx, y, it->value);
        }

        if (selected) {
            u8g2_SetDrawColor(u8g2, 1);  /* restore */
        }
    }

    /* Scroll arrows inside the yellow header so they never straddle the seam or
     * collide with the bottom hint strip, which would silently erase them. */
    if (first > 0) {
        u8g2_DrawTriangle(u8g2, 112, 2, 108, 8, 116, 8);
    }
    if (last < view->count) {
        u8g2_DrawTriangle(u8g2, 118, 2, 126, 2, 122, 8);
    }
}
