#include "display_drone.h"
#include <stdio.h>

/* Layout mirrors the menu overlay: yellow header bar (rows 0..15) with the
 * title, then up to 3 rows in the blue region below the 16px seam. */
#define DRONE_TITLE_Y    8
#define DRONE_ROW_H      12
#define DRONE_FIRST_ROW  25
#define DRONE_LIST_ROWS  3

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
    if (view->cursor >= DRONE_LIST_ROWS) {
        first = (uint8_t)(view->cursor - (DRONE_LIST_ROWS - 1));
    }
    uint8_t last = (uint8_t)(first + DRONE_LIST_ROWS);
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

        if (r->has_glyph) {
            /* One 4x6 cell per step at a 5 px pitch, after the label. */
            uint8_t gx = (uint8_t)(u8g2_GetStrWidth(u8g2, r->label) + 6);
            for (uint8_t st = 0; st < 8u; st++) {
                uint8_t cx = (uint8_t)(gx + st * 5u);
                if (r->glyph & (1u << st)) u8g2_DrawBox(u8g2, cx, (uint8_t)(y - 7), 4, 6);
                else                       u8g2_DrawFrame(u8g2, cx, (uint8_t)(y - 7), 4, 6);
            }
        }

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
