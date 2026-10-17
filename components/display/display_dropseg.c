#include "display_dropseg.h"
#include <stdio.h>

/* The Step Trig popup's geometry (display_stepedit.c): title in the yellow
 * header, rows from y=24 at a 9 px pitch, clear of the hint strip. Two rows
 * fit whole, so there is no window to scroll. */
#define DS_TITLE_Y   8
#define DS_ROW_H     9
#define DS_FIRST_ROW 24

/* Triangle marker when selected (turn navigates), box+invert in adjust mode
 * (turn changes the value). */
static void ds_draw_row(u8g2_t *u8g2, uint8_t y, const char *label,
                        const char *value, bool selected, bool editing)
{
    char buf[26];
    snprintf(buf, sizeof(buf), "%-8s: %s", label, value);
    if (selected && editing) {
        uint8_t w = (uint8_t)(u8g2_GetStrWidth(u8g2, buf) + 4);
        u8g2_DrawBox(u8g2, 4, (uint8_t)(y - 8), w, DS_ROW_H);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, 6, y, buf);
        u8g2_SetDrawColor(u8g2, 1);
    } else if (selected) {
        u8g2_DrawStr(u8g2, 6, y, buf);
        u8g2_DrawTriangle(u8g2, 0, (int16_t)(y - 6),
                                0, (int16_t)(y - 1),
                                3, (int16_t)(y - 3));
    } else {
        u8g2_DrawStr(u8g2, 6, y, buf);
    }
}

/* An explicit + on a set value and a bare 0 when neutral, as the step popup
 * prints its signed fields. */
static void ds_signed_text(int v, char *out, size_t sz)
{
    if (v != 0) snprintf(out, sz, "%+d", v);
    else        snprintf(out, sz, "0");
}

void display_dropseg_draw_frame(u8g2_t *u8g2, const dropseg_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);

    if (view == NULL) { return; }

    char title[24];
    snprintf(title, sizeof(title), "SEG C%02u R%u",
             (unsigned)(view->col + 1), (unsigned)(view->row + 1));
    u8g2_DrawStr(u8g2, 2, DS_TITLE_Y, title);
    u8g2_DrawHLine(u8g2, 0, 15, 128);

    char val[8];
    ds_signed_text(view->pitch_ofs, val, sizeof(val));
    ds_draw_row(u8g2, DS_FIRST_ROW, "Pitch", val,
                view->field == DS_FIELD_PITCH, view->editing);
    ds_signed_text(view->vel_adj, val, sizeof(val));
    ds_draw_row(u8g2, DS_FIRST_ROW + DS_ROW_H, "Vel", val,
                view->field == DS_FIELD_VEL, view->editing);
}
