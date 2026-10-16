#include "display_drop.h"
#include <stdio.h>
#include <string.h>

/* Half-vectors of the eight segment angles, pixels from the cell centre. */
static const int8_t s_half[8][2] = {
    { 4, 0 }, { 4, 2 }, { 3, 3 }, { 2, 4 }, { 0, 4 }, { -2, 4 }, { -3, 3 }, { -4, 2 },
};

static inline uint8_t cell_x(uint8_t col) { return (uint8_t)(col * DROP_VIEW_CELL_W); }
static inline uint8_t cell_y(uint8_t row) { return (uint8_t)(DROP_VIEW_Y0 + row * DROP_VIEW_CELL_H); }

/* A line drawn every other pixel. */
static void draw_dotted_line(u8g2_t *u8g2, int x0, int y0, int x1, int y1)
{
    int dx = (x1 > x0) ? x1 - x0 : x0 - x1;
    int dy = (y1 > y0) ? y1 - y0 : y0 - y1;
    int n  = (dx > dy) ? dx : dy;
    for (int i = 0; i <= n; i += 2) {
        int x = x0 + ((x1 - x0) * i + (n ? n / 2 : 0)) / (n ? n : 1);
        int y = y0 + ((y1 - y0) * i + (n ? n / 2 : 0)) / (n ? n : 1);
        u8g2_DrawPixel(u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y);
    }
}

static void draw_segment(u8g2_t *u8g2, const drop_view_seg_t *s)
{
    int cx = cell_x(s->col) + DROP_VIEW_CELL_W / 2;
    int cy = cell_y(s->row) + DROP_VIEW_CELL_H / 2;
    uint8_t a = (s->angle < 8) ? s->angle : 0;
    int hx = s_half[a][0], hy = s_half[a][1];
    int x0 = cx - hx, y0 = cy - hy, x1 = cx + hx, y1 = cy + hy;
    if (s->type == 2) {
        draw_dotted_line(u8g2, x0, y0, x1, y1);
    } else {
        u8g2_DrawLine(u8g2, (u8g2_uint_t)x0, (u8g2_uint_t)y0, (u8g2_uint_t)x1, (u8g2_uint_t)y1);
    }
    if (s->type == 1) {
        /* The notch: a short tick along the normal at the centre. */
        int nx = -hy, ny = hx;
        int tx = (nx > 0) - (nx < 0), ty = (ny > 0) - (ny < 0);
        u8g2_DrawPixel(u8g2, (u8g2_uint_t)(cx + tx), (u8g2_uint_t)(cy + ty));
        u8g2_DrawPixel(u8g2, (u8g2_uint_t)(cx - tx), (u8g2_uint_t)(cy - ty));
    }
}

/* Header field, 4x6 text in an 8-pixel row; framed when it is the cursor,
 * inverted while editing. Returns the next field's x. */
static uint8_t draw_field(u8g2_t *u8g2, uint8_t x, uint8_t row_top, const char *text,
                          bool selected, bool editing)
{
    uint8_t w = (uint8_t)u8g2_GetStrWidth(u8g2, text);
    uint8_t fx = (x > 0) ? (uint8_t)(x - 1) : 0;
    if (selected && editing) {
        u8g2_DrawBox(u8g2, fx, row_top, (u8g2_uint_t)(w + 2), 8);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, x, (u8g2_uint_t)(row_top + 6), text);
        u8g2_SetDrawColor(u8g2, 1);
    } else {
        if (selected) u8g2_DrawFrame(u8g2, fx, row_top, (u8g2_uint_t)(w + 2), 8);
        u8g2_DrawStr(u8g2, x, (u8g2_uint_t)(row_top + 6), text);
    }
    return (uint8_t)(x + w + 6);
}

void display_drop_draw_frame(u8g2_t *u8g2, const drop_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    if (view == NULL) return;

    char buf[12];
    u8g2_SetFont(u8g2, u8g2_font_4x6_tr);

    /* ── Header row 1 (yellow): ON | focus | patch | root ── */
    uint8_t x = 2;
    x = draw_field(u8g2, x, 0, view->enabled ? (view->muted ? "MUTE" : "ON") : "OFF",
                   view->cursor == DROP_CUR_ENABLE, view->editing);
    x = draw_field(u8g2, x, 0, view->focus ? "SUB" : "DROP",
                   view->cursor == DROP_CUR_FOCUS, view->editing);
    snprintf(buf, sizeof(buf), "P%u", (unsigned)view->patch);
    if (view->patch_select) {
        uint8_t iw = (uint8_t)u8g2_GetStrWidth(u8g2, buf);
        u8g2_DrawFrame(u8g2, (u8g2_uint_t)(x - 1), 0, (u8g2_uint_t)(iw + 2), 8);
    }
    u8g2_DrawStr(u8g2, x, 6, buf);
    x = (uint8_t)(x + u8g2_GetStrWidth(u8g2, buf) + 6);
    draw_field(u8g2, x, 0, view->root_name, view->cursor == DROP_CUR_ROOT, view->editing);

    /* ── Header row 2 (yellow): bars | seed | rate ── */
    x = 2;
    snprintf(buf, sizeof(buf), "%uBAR", (unsigned)view->loop_bars);
    x = draw_field(u8g2, x, 8, buf, view->cursor == DROP_CUR_BARS, view->editing);
    snprintf(buf, sizeof(buf), "%04X%s", (unsigned)view->seed16, view->seed_frozen ? "*" : "");
    x = draw_field(u8g2, x, 8, buf, view->cursor == DROP_CUR_SEED, view->editing);
    {
        const char *rs = view->rate_str ? view->rate_str : "?";
        draw_field(u8g2, x, 8, rs, view->cursor == DROP_CUR_RATE, view->editing);
    }

    /* ── Field ── */
    /* Spawner: a chevron on the top edge of its column. */
    {
        int sx = cell_x(view->spawn_col) + DROP_VIEW_CELL_W / 2;
        u8g2_DrawLine(u8g2, (u8g2_uint_t)(sx - 3), DROP_VIEW_Y0, (u8g2_uint_t)sx, DROP_VIEW_Y0 + 3);
        u8g2_DrawLine(u8g2, (u8g2_uint_t)(sx + 3), DROP_VIEW_Y0, (u8g2_uint_t)sx, DROP_VIEW_Y0 + 3);
    }
    /* Jackpot cells: a hatched box each. */
    for (uint8_t cell = 0; cell < DROP_VIEW_CELLS; cell++) {
        if (!((view->jackpot[cell >> 3] >> (cell & 7u)) & 1u)) continue;
        int jx = cell_x((uint8_t)(cell % DROP_VIEW_COLS));
        int jy = cell_y((uint8_t)(cell / DROP_VIEW_COLS));
        u8g2_DrawFrame(u8g2, (u8g2_uint_t)jx, (u8g2_uint_t)jy, DROP_VIEW_CELL_W, DROP_VIEW_CELL_H);
        u8g2_DrawLine(u8g2, (u8g2_uint_t)(jx + 2), (u8g2_uint_t)(jy + DROP_VIEW_CELL_H - 1),
                      (u8g2_uint_t)(jx + DROP_VIEW_CELL_W - 1), (u8g2_uint_t)(jy + 1));
        u8g2_DrawLine(u8g2, (u8g2_uint_t)jx, (u8g2_uint_t)(jy + 3),
                      (u8g2_uint_t)(jx + 3), (u8g2_uint_t)jy);
    }
    for (uint8_t i = 0; i < view->seg_count && i < DROP_VIEW_SEGS; i++) {
        draw_segment(u8g2, &view->segs[i]);
    }
    for (uint8_t i = 0; i < view->ball_count && i < DROP_VIEW_BALLS; i++) {
        int bx = view->balls[i].x, by = DROP_VIEW_Y0 + view->balls[i].y;
        if (bx > 126) bx = 126;
        if (by > 62) by = 62;   /* the field's last pixel row is 63 */
        u8g2_DrawBox(u8g2, (u8g2_uint_t)bx, (u8g2_uint_t)by, 2, 2);
    }
    /* Cell cursor: dotted frame while navigating, solid while editing. */
    if (view->cursor >= DROP_CUR_CELL0 && view->cursor < DROP_CUR_COUNT) {
        uint8_t cell = (uint8_t)(view->cursor - DROP_CUR_CELL0);
        uint8_t col = (uint8_t)(cell % DROP_VIEW_COLS);
        uint8_t row = (uint8_t)(cell / DROP_VIEW_COLS);
        int x0 = cell_x(col), y0 = cell_y(row);
        int x1 = x0 + DROP_VIEW_CELL_W - 1, y1 = y0 + DROP_VIEW_CELL_H - 1;
        if (view->editing) {
            u8g2_DrawFrame(u8g2, (u8g2_uint_t)x0, (u8g2_uint_t)y0,
                           DROP_VIEW_CELL_W, DROP_VIEW_CELL_H);
        } else {
            draw_dotted_line(u8g2, x0, y0, x1, y0);
            draw_dotted_line(u8g2, x0, y1, x1, y1);
            draw_dotted_line(u8g2, x0, y0, x0, y1);
            draw_dotted_line(u8g2, x1, y0, x1, y1);
        }
    }

    /* Patch-name banner during the hold+turn gesture, as on the arp view. */
    if (view->patch_select && view->patch_name) {
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        uint8_t nw = (uint8_t)u8g2_GetStrWidth(u8g2, view->patch_name);
        if (nw > 124) nw = 124;
        uint8_t bx = (uint8_t)((128 - (nw + 4)) / 2);
        uint8_t by = 32;
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawBox(u8g2, bx, by, (uint8_t)(nw + 4), 13);
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawRFrame(u8g2, bx, by, (uint8_t)(nw + 4), 13, 2);
        u8g2_DrawStr(u8g2, (uint8_t)(bx + 2), (uint8_t)(by + 9), view->patch_name);
    }
    if (view->banner) {
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        uint8_t nw = (uint8_t)u8g2_GetStrWidth(u8g2, view->banner);
        uint8_t bx = (uint8_t)((128 - (nw + 4)) / 2);
        uint8_t by = 32;
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawBox(u8g2, bx, by, (uint8_t)(nw + 4), 13);
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawRFrame(u8g2, bx, by, (uint8_t)(nw + 4), 13, 2);
        u8g2_DrawStr(u8g2, (uint8_t)(bx + 2), (uint8_t)(by + 9), view->banner);
    }
}
