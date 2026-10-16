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

/* Status field; framed when it is the cursor, double-framed while editing. */
static uint8_t draw_field(u8g2_t *u8g2, uint8_t x, const char *text,
                          bool selected, bool editing)
{
    uint8_t w = (uint8_t)u8g2_GetStrWidth(u8g2, text);
    if (selected) {
        u8g2_DrawFrame(u8g2, (u8g2_uint_t)(x > 1 ? x - 2 : 0), 0, (u8g2_uint_t)(w + 3), 9);
        if (editing) {
            u8g2_DrawHLine(u8g2, (u8g2_uint_t)(x > 1 ? x - 2 : 0), 9, (u8g2_uint_t)(w + 3));
        }
    }
    u8g2_DrawStr(u8g2, x, 7, text);
    return (uint8_t)(x + w + 5);
}

void display_drop_draw_frame(u8g2_t *u8g2, const drop_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    if (view == NULL) return;

    char buf[12];
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);

    /* ── Status row: ON | focus | patch | root | bars | seed | rate ── */
    uint8_t x = 2;
    x = draw_field(u8g2, x, view->enabled ? (view->muted ? "MUTE" : "ON") : "OFF",
                   view->cursor == DROP_CUR_ENABLE, view->editing);
    x = draw_field(u8g2, x, view->focus ? "S" : "D",
                   view->cursor == DROP_CUR_FOCUS, view->editing);
    snprintf(buf, sizeof(buf), "%u", (unsigned)view->patch);
    if (view->patch_select) {
        uint8_t iw = (uint8_t)u8g2_GetStrWidth(u8g2, buf);
        u8g2_DrawFrame(u8g2, (u8g2_uint_t)(x - 2), 0, (u8g2_uint_t)(iw + 3), 9);
    }
    u8g2_DrawStr(u8g2, x, 7, buf);
    x = (uint8_t)(x + u8g2_GetStrWidth(u8g2, buf) + 5);
    x = draw_field(u8g2, x, view->root_name, view->cursor == DROP_CUR_ROOT, view->editing);
    snprintf(buf, sizeof(buf), "%ub", (unsigned)view->loop_bars);
    x = draw_field(u8g2, x, buf, view->cursor == DROP_CUR_BARS, view->editing);
    snprintf(buf, sizeof(buf), "%04X%s", (unsigned)view->seed16, view->seed_frozen ? "*" : "");
    x = draw_field(u8g2, x, buf, view->cursor == DROP_CUR_SEED, view->editing);
    {
        const char *rs = view->rate_str ? view->rate_str : "?";
        uint8_t rw = (uint8_t)u8g2_GetStrWidth(u8g2, rs);
        uint8_t rx = (rw < 126u) ? (uint8_t)(126u - rw) : 0u;
        if (rx < x) rx = x;
        draw_field(u8g2, rx, rs, view->cursor == DROP_CUR_RATE, view->editing);
    }

    /* ── Field ── */
    /* Spawner: a chevron on the top edge of its column. */
    {
        int sx = cell_x(view->spawn_col) + DROP_VIEW_CELL_W / 2;
        u8g2_DrawLine(u8g2, (u8g2_uint_t)(sx - 3), DROP_VIEW_Y0, (u8g2_uint_t)sx, DROP_VIEW_Y0 + 3);
        u8g2_DrawLine(u8g2, (u8g2_uint_t)(sx + 3), DROP_VIEW_Y0, (u8g2_uint_t)sx, DROP_VIEW_Y0 + 3);
    }
    /* Jackpot: a hatched box over its cells. */
    if (view->jackpot.enabled) {
        int jx = cell_x(view->jackpot.col0);
        int jy = cell_y(view->jackpot.row);
        int jw = (view->jackpot.col1 - view->jackpot.col0 + 1) * DROP_VIEW_CELL_W;
        u8g2_DrawFrame(u8g2, (u8g2_uint_t)jx, (u8g2_uint_t)jy, (u8g2_uint_t)jw, DROP_VIEW_CELL_H);
        for (int d = 3; d < jw + DROP_VIEW_CELL_H; d += 3) {
            int x0 = jx + d, y0 = jy;
            int x1 = jx + d - (DROP_VIEW_CELL_H - 1), y1 = jy + DROP_VIEW_CELL_H - 1;
            if (x0 > jx + jw - 1) { y0 += x0 - (jx + jw - 1); x0 = jx + jw - 1; }
            if (x1 < jx) { y1 -= jx - x1; x1 = jx; }
            if (y0 <= y1) u8g2_DrawLine(u8g2, (u8g2_uint_t)x0, (u8g2_uint_t)y0,
                                        (u8g2_uint_t)x1, (u8g2_uint_t)y1);
        }
    }
    for (uint8_t i = 0; i < view->seg_count && i < DROP_VIEW_SEGS; i++) {
        draw_segment(u8g2, &view->segs[i]);
    }
    for (uint8_t i = 0; i < view->ball_count && i < DROP_VIEW_BALLS; i++) {
        int bx = view->balls[i].x, by = DROP_VIEW_Y0 + view->balls[i].y;
        if (bx > 126) bx = 126;
        if (by > 62) by = 62;
        u8g2_DrawBox(u8g2, (u8g2_uint_t)bx, (u8g2_uint_t)by, 2, 2);
    }
    /* Cell cursor: dotted frame while navigating, solid while editing. */
    if (view->cursor < DROP_VIEW_CELLS) {
        uint8_t col = (uint8_t)(view->cursor % DROP_VIEW_COLS);
        uint8_t row = (uint8_t)(view->cursor / DROP_VIEW_COLS);
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
        uint8_t by = 26;
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
        uint8_t by = 26;
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawBox(u8g2, bx, by, (uint8_t)(nw + 4), 13);
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawRFrame(u8g2, bx, by, (uint8_t)(nw + 4), 13, 2);
        u8g2_DrawStr(u8g2, (uint8_t)(bx + 2), (uint8_t)(by + 9), view->banner);
    }
}
