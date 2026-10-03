#include "display_stepedit.h"
#include <stdio.h>

/* Yellow header (rows 0..15) carries the STEP title; the visible field window
 * fills the blue region starting at y=24 so no row crosses the 16px seam. 9 px
 * row pitch fits SE_VISIBLE_ROWS baselines (24..60) with descent room inside
 * 64 - which is what caps the window at five of the eight (or nine) fields. */
#define SE_TITLE_Y   8
#define SE_ROW_H     9
#define SE_FIRST_ROW 24
/* Scroll cues share the cursor marker's 3 px size, parked clear of the widest
 * value string. */
#define SE_CUE_X     124

/* Select/adjust phases in one row: triangle
 * marker when merely selected (turn navigates), box+invert when in adjust
 * mode (turn changes the value). */
static void se_draw_row(u8g2_t *u8g2, uint8_t y, const char *label,
                        const char *value, bool selected, bool editing)
{
    char buf[26];
    snprintf(buf, sizeof(buf), "%-8s: %s", label, value);
    if (selected && editing) {
        uint8_t w = (uint8_t)(u8g2_GetStrWidth(u8g2, buf) + 4);
        u8g2_DrawBox(u8g2, 4, (uint8_t)(y - 8), w, SE_ROW_H);
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

/* One field's label and value text. Signed fields print an explicit + and a
 * bare 0 when neutral, so "off" reads differently from a set value. */
static void se_field_text(const stepedit_view_t *view, uint8_t field,
                          const char **label_out, char *value_out, size_t value_sz)
{
    switch (field) {
        case SE_FIELD_PITCH:
            *label_out = "Pitch";
            if (view->pitch_ofs != 0) snprintf(value_out, value_sz, "%+d", (int)view->pitch_ofs);
            else                      snprintf(value_out, value_sz, "0");
            break;
        case SE_FIELD_PROB:
            *label_out = "Prob";
            snprintf(value_out, value_sz, "%u%%", (unsigned)view->prob);
            break;
        case SE_FIELD_RATCHET:
            *label_out = "Ratchet";
            snprintf(value_out, value_sz, "%u", (unsigned)view->ratchet);
            break;
        case SE_FIELD_EVERY:
            *label_out = "Every";
            snprintf(value_out, value_sz, "%u", (unsigned)view->every);
            break;
        case SE_FIELD_PREV:
            *label_out = "Prev";
            snprintf(value_out, value_sz, "%s", view->prev ? "ON" : "OFF");
            break;
        case SE_FIELD_VEL:
            *label_out = "Vel";
            if (view->vel_adj != 0) snprintf(value_out, value_sz, "%+d", (int)view->vel_adj);
            else                    snprintf(value_out, value_sz, "0");
            break;
        case SE_FIELD_NUDGE:
            *label_out = "Nudge";
            if (view->nudge != 0) snprintf(value_out, value_sz, "%+d", (int)view->nudge);
            else                  snprintf(value_out, value_sz, "0");
            break;
        case SE_FIELD_TAPER:
            *label_out = "Taper";
            if (view->taper != 0) snprintf(value_out, value_sz, "%+d%%", (int)view->taper);
            else                  snprintf(value_out, value_sz, "0");
            break;
        default:
            *label_out = "FRM";
            if (view->frame != 0u) snprintf(value_out, value_sz, "%u", (unsigned)(view->frame - 1u));
            else                   snprintf(value_out, value_sz, "--");
            break;
    }
}

void display_stepedit_draw_frame(u8g2_t *u8g2, const stepedit_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);

    if (view == NULL) { return; }

    char title[24];
    snprintf(title, sizeof(title), "STEP L%u T%u S%02u",
             (unsigned)(view->layer_idx + 1), (unsigned)(view->track_idx + 1),
             (unsigned)(view->step_idx + 1));
    u8g2_DrawStr(u8g2, 2, SE_TITLE_Y, title);
    u8g2_DrawHLine(u8g2, 0, 15, 128);

    uint8_t count = view->has_frame ? (uint8_t)SE_FIELD_COUNT : (uint8_t)SE_FIELD_FRAME;
    uint8_t first = view->first_row;
    if (first > count - SE_VISIBLE_ROWS) first = (uint8_t)(count - SE_VISIBLE_ROWS);

    uint8_t y = SE_FIRST_ROW;
    char val[8];

    for (uint8_t i = 0; i < SE_VISIBLE_ROWS; i++) {
        uint8_t field = (uint8_t)(first + i);
        const char *label = "";
        se_field_text(view, field, &label, val, sizeof(val));
        /* Prev is click-toggled and has no adjust phase - never inverted. */
        bool editing = view->editing && field != SE_FIELD_PREV;
        se_draw_row(u8g2, y, label, val, view->field_cursor == field, editing);
        y = (uint8_t)(y + SE_ROW_H);
    }

    /* Scroll cues: which way the hidden fields lie, on the first/last row. */
    if (first > 0) {
        u8g2_DrawTriangle(u8g2, SE_CUE_X,     (int16_t)(SE_FIRST_ROW - 2),
                                SE_CUE_X + 3, (int16_t)(SE_FIRST_ROW - 2),
                                SE_CUE_X + 1, (int16_t)(SE_FIRST_ROW - 7));
    }
    if (first + SE_VISIBLE_ROWS < count) {
        int16_t ly = (int16_t)(SE_FIRST_ROW + (SE_VISIBLE_ROWS - 1) * SE_ROW_H);
        u8g2_DrawTriangle(u8g2, SE_CUE_X,     (int16_t)(ly - 7),
                                SE_CUE_X + 3, (int16_t)(ly - 7),
                                SE_CUE_X + 1, (int16_t)(ly - 2));
    }
}
