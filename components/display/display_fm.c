#include "display_fm.h"
#include <stdio.h>
#include <string.h>

/* Graph geometry: 6 columns x 13 px, 9x7 boxes; carriers sit with their
 * bottom edge at FM_BOTTOM_Y, rows stack upward with a pitch that shrinks for
 * deep chains (a 6-deep chain lands the boxes touching). */
#define FM_BOX_W      9
#define FM_BOX_H      7
#define FM_COL_X0     2
#define FM_COL_PITCH  13
#define FM_BOTTOM_Y   55        /* last pixel row of the carrier boxes */
#define FM_TOP_Y      11        /* first row the topmost box may occupy */
#define FM_ROW_PITCH  11

#define FM_PANEL_X    82
#define FM_PANEL_Y0   22
#define FM_PANEL_ROW_H 8

/* Page 1 grid: six 7 px rows fill the blue band above the hint strip (y 57).
 * The two full-width lines have two columns; a column's '>' marker sits at
 * its x, the text 5 px right of it. The four T/L lines are a compact column
 * with fixed cell boxes (framed while navigating, filled while adjusting) and
 * the plot to their right, spanning those four rows. */
#define FM2_COL0_X    2
#define FM2_COL1_X    66
#define FM2_COL_W     64
#define FM2_Y0        21
#define FM2_ROW_H     7
#define FM2_WIDE_ROWS 2         /* OP/FRQ and coarse/fine */
#define FM2_T_BOX_X   0
#define FM2_T_BOX_W   35
#define FM2_L_BOX_X   36
#define FM2_L_BOX_W   14
#define FM2_PLOT_X    (128 - FM_PLOT_W)
#define FM2_PLOT_TOP  (FM2_Y0 + FM2_WIDE_ROWS * FM2_ROW_H - 6)
#define FM2_PLOT_BOT  56        /* last row above the hint strip */
#define FM2_LEVEL_MAX 99

#define FM_NO_OP      0xFF

typedef struct {
    uint8_t parent[FM_GRAPH_OPS];    /* placement parent (first target) or FM_NO_OP */
    uint8_t col[FM_GRAPH_OPS];
    uint8_t row[FM_GRAPH_OPS];       /* 0 = carrier row */
    uint8_t rows;                    /* rows used */
} fm_layout_t;

static uint8_t layout_width(const fm_layout_t *l, uint8_t node, uint8_t depth)
{
    if (depth >= FM_GRAPH_OPS) return 1;
    uint8_t w = 0;
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        if (l->parent[i] == node) w = (uint8_t)(w + layout_width(l, i, (uint8_t)(depth + 1)));
    }
    return w ? w : 1;
}

static void layout_place(fm_layout_t *l, uint8_t node, uint8_t col, uint8_t row)
{
    if (row >= FM_GRAPH_OPS) return;
    l->col[node] = col;
    l->row[node] = row;
    if (row + 1 > l->rows) l->rows = (uint8_t)(row + 1);
    uint8_t c = col;
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        if (l->parent[i] != node) continue;
        layout_place(l, i, c, (uint8_t)(row + 1));
        c = (uint8_t)(c + layout_width(l, i, 0));
    }
}

static void layout_build(const fm_graph_view_t *g, fm_layout_t *l)
{
    memset(l, 0, sizeof(*l));
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        l->parent[i] = FM_NO_OP;
        for (uint8_t t = 0; t < FM_GRAPH_OPS; t++) {
            if (g->out_mask[i] & (1u << t)) { l->parent[i] = t; break; }
        }
    }
    /* Placement is a tree walk from each carrier; an op that neither routes
     * to the output nor reaches a carrier (never true for a compiled graph)
     * is parked on the carrier row so it still shows. */
    bool placed[FM_GRAPH_OPS] = { false };
    uint8_t c = 0;
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        if (l->parent[i] != FM_NO_OP) continue;
        layout_place(l, i, c, 0);
        c = (uint8_t)(c + layout_width(l, i, 0));
    }
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        uint8_t n = i, guard = 0;
        while (l->parent[n] != FM_NO_OP && guard++ < FM_GRAPH_OPS) n = l->parent[n];
        placed[i] = (l->parent[n] == FM_NO_OP);
    }
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        if (!placed[i]) { l->col[i] = c++; l->row[i] = 0; }
    }
    if (l->rows == 0) l->rows = 1;
}

static uint8_t box_x(const fm_layout_t *l, uint8_t op)
{
    return (uint8_t)(FM_COL_X0 + l->col[op] * FM_COL_PITCH);
}

static uint8_t row_pitch(const fm_layout_t *l)
{
    if (l->rows <= 1) return FM_ROW_PITCH;
    uint8_t span = (uint8_t)(FM_BOTTOM_Y - FM_BOX_H + 1 - FM_TOP_Y);   /* 38 */
    uint8_t p = (uint8_t)(span / (l->rows - 1));
    if (p > FM_ROW_PITCH) p = FM_ROW_PITCH;
    if (p < FM_BOX_H) p = FM_BOX_H;
    return p;
}

static uint8_t box_y(const fm_layout_t *l, uint8_t op)
{
    return (uint8_t)(FM_BOTTOM_Y - FM_BOX_H + 1 - l->row[op] * row_pitch(l));
}

/* Connector from op `from` (above) down into op `to`: straight when aligned,
 * otherwise an L through the middle of the gap. */
static void draw_connector(u8g2_t *u8g2, const fm_layout_t *l, uint8_t from, uint8_t to)
{
    uint8_t cx0 = (uint8_t)(box_x(l, from) + FM_BOX_W / 2);
    uint8_t cx1 = (uint8_t)(box_x(l, to)   + FM_BOX_W / 2);
    uint8_t y0  = (uint8_t)(box_y(l, from) + FM_BOX_H);    /* first row below `from` */
    uint8_t y1  = box_y(l, to);                            /* top row of `to` */
    if (y1 <= y0) {
        /* Touching or overlapping rows (deep chain / fan-out across rows):
         * only a horizontal stub can show the link. */
        if (cx0 != cx1) u8g2_DrawHLine(u8g2, cx0 < cx1 ? cx0 : cx1, y1, (uint8_t)(cx0 > cx1 ? cx0 - cx1 : cx1 - cx0));
        return;
    }
    if (cx0 == cx1) {
        u8g2_DrawVLine(u8g2, cx0, y0, (uint8_t)(y1 - y0));
        return;
    }
    uint8_t ym = (uint8_t)((y0 + y1) / 2);
    u8g2_DrawVLine(u8g2, cx0, y0, (uint8_t)(ym - y0 + 1));
    u8g2_DrawHLine(u8g2, cx0 < cx1 ? cx0 : cx1, ym, (uint8_t)((cx0 > cx1 ? cx0 - cx1 : cx1 - cx0) + 1));
    u8g2_DrawVLine(u8g2, cx1, ym, (uint8_t)(y1 - ym));
}

static void draw_feedback_loop(u8g2_t *u8g2, uint8_t x, uint8_t y)
{
    /* Out of the right edge, up over the top, back in near the top-right. */
    u8g2_DrawHLine(u8g2, (uint8_t)(x + FM_BOX_W), (uint8_t)(y + 3), 3);
    u8g2_DrawVLine(u8g2, (uint8_t)(x + FM_BOX_W + 2), (uint8_t)(y - 2), 6);
    u8g2_DrawHLine(u8g2, (uint8_t)(x + FM_BOX_W - 2), (uint8_t)(y - 2), 5);
    u8g2_DrawVLine(u8g2, (uint8_t)(x + FM_BOX_W - 2), (uint8_t)(y - 2), 2);
}

/* Box outline with every other pixel lit (a refused link target). */
static void draw_frame_dotted(u8g2_t *u8g2, uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    for (uint8_t i = 0; i < w; i += 2u) {
        u8g2_DrawPixel(u8g2, (uint8_t)(x + i), y);
        u8g2_DrawPixel(u8g2, (uint8_t)(x + i), (uint8_t)(y + h - 1u));
    }
    for (uint8_t j = 0; j < h; j += 2u) {
        u8g2_DrawPixel(u8g2, x, (uint8_t)(y + j));
        u8g2_DrawPixel(u8g2, (uint8_t)(x + w - 1u), (uint8_t)(y + j));
    }
}

static uint8_t plot_y(uint8_t level)
{
    const uint16_t h = FM2_PLOT_BOT - FM2_PLOT_TOP;
    if (level > FM2_LEVEL_MAX) level = FM2_LEVEL_MAX;
    return (uint8_t)(FM2_PLOT_BOT - (level * h + FM2_LEVEL_MAX / 2) / FM2_LEVEL_MAX);
}

/* One trace line (Bresenham). Chained lines share their joint, so every line
 * but the first skips its start pixel. Dotted lights every other pixel, the
 * phase carried across calls so the dots stay even; thick adds the pixel
 * above. */
static void plot_line(u8g2_t *u8g2, int x0, int y0, int x1, int y1,
                      bool skip_first, bool dotted, bool thick, uint8_t *phase)
{
    int dx = (x1 > x0) ? x1 - x0 : x0 - x1;
    int dy = (y1 > y0) ? y0 - y1 : y1 - y0;          /* -|dy| */
    int sx = (x0 < x1) ? 1 : -1, sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (skip_first) {
            skip_first = false;
        } else if (!dotted || ((*phase)++ & 1u) == 0u) {
            u8g2_DrawPixel(u8g2, (uint8_t)x0, (uint8_t)y0);
            if (thick) u8g2_DrawPixel(u8g2, (uint8_t)x0, (uint8_t)(y0 - 1));
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* The read-only envelope plot. Segments g = 0..4 run between breakpoint
 * columns g and g+1: T1, T2, T3, the sustain stub, T4. */
static void draw_eg_plot(u8g2_t *u8g2, const fm_view_t *view, bool dotted)
{
    static const uint8_t seg_of_t[4] = { 0, 1, 2, 4 };
    static const uint8_t bp_of_l[4]  = { 1, 2, 3, 5 };   /* L4 also marks bp 0 */
    static const uint8_t prev_l[4]   = { 3, 0, 1, 2 };   /* level a Tn starts from */
    int hl_seg = -1, hl_pt = -1;
    if (view->cursor >= FM2_CUR_T1 && view->cursor <= FM2_CUR_L4) {
        uint8_t n = (uint8_t)((view->cursor - FM2_CUR_T1) / 2u);
        if (((view->cursor - FM2_CUR_T1) & 1u) == 0u) hl_seg = n;
        else                                          hl_pt  = n;
    }

    const uint8_t *bp = view->plot_bp_x;
    uint8_t phase = 0;
    for (uint8_t g = 0; g + 1u < FM_PLOT_BPS; g++) {
        bool thick = (hl_seg >= 0 && seg_of_t[hl_seg] == g);
        for (uint8_t x = bp[g]; x < bp[g + 1u]; x++) {
            plot_line(u8g2, FM2_PLOT_X + x, plot_y(view->plot_level[x]),
                      FM2_PLOT_X + x + 1, plot_y(view->plot_level[x + 1u]),
                      x != 0u, dotted, thick, &phase);
        }
        if (thick && bp[g] == bp[g + 1u]) {
            /* A zero-time segment is a jump: mark it as a double vertical. */
            uint8_t ya = plot_y(view->eg_level[prev_l[hl_seg]]);
            uint8_t yb = plot_y(view->eg_level[hl_seg]);
            uint8_t top = (ya < yb) ? ya : yb, len = (uint8_t)((ya < yb ? yb - ya : ya - yb) + 1u);
            u8g2_DrawVLine(u8g2, (uint8_t)(FM2_PLOT_X + bp[g]), top, len);
            u8g2_DrawVLine(u8g2, (uint8_t)(FM2_PLOT_X + bp[g] + 1u), top, len);
        }
    }
    if (hl_pt >= 0) {
        uint8_t y = plot_y(view->eg_level[hl_pt]);
        u8g2_DrawBox(u8g2, (uint8_t)(FM2_PLOT_X + bp[bp_of_l[hl_pt]] - 1u), (uint8_t)(y - 1), 3, 3);
        if (hl_pt == 3) u8g2_DrawBox(u8g2, (uint8_t)(FM2_PLOT_X + bp[0] - 1u), (uint8_t)(y - 1), 3, 3);
    }
}

/* Page 1 below the title: "MUTE" top-right, the two wide lines with page 0's
 * cursor convention ('>' while navigating, inverted while adjusting), the
 * compact T/L column, and the plot. */
static void draw_page_freq_eg(u8g2_t *u8g2, const fm_view_t *view)
{
    bool muted = (view->muted & (1u << view->selected_op)) != 0u;
    if (muted) u8g2_DrawStr(u8g2, 104, 8, "MUTE");

    u8g2_SetFont(u8g2, u8g2_font_4x6_tr);
    for (uint8_t c = 0; c < FM2_CUR_COUNT; c++) {
        uint8_t y = (uint8_t)(FM2_Y0 + (c / 2u) * FM2_ROW_H);
        bool on = (view->cursor == c);
        if (c < FM2_CUR_T1) {
            uint8_t x = (c & 1u) ? FM2_COL1_X : FM2_COL0_X;
            if (on && view->editing) {
                u8g2_DrawBox(u8g2, (uint8_t)(x + 4), (uint8_t)(y - 6), FM2_COL_W - 6, FM2_ROW_H);
                u8g2_SetDrawColor(u8g2, 0);
                u8g2_DrawStr(u8g2, (uint8_t)(x + 5), y, view->cells[c]);
                u8g2_SetDrawColor(u8g2, 1);
            } else {
                if (on) u8g2_DrawStr(u8g2, x, y, ">");
                u8g2_DrawStr(u8g2, (uint8_t)(x + 5), y, view->cells[c]);
            }
            continue;
        }
        uint8_t bx = (c & 1u) ? FM2_L_BOX_X : FM2_T_BOX_X;
        uint8_t bw = (c & 1u) ? FM2_L_BOX_W : FM2_T_BOX_W;
        if (on && view->editing) {
            u8g2_DrawBox(u8g2, bx, (uint8_t)(y - 6), bw, FM2_ROW_H);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, (uint8_t)(bx + 2), y, view->cells[c]);
            u8g2_SetDrawColor(u8g2, 1);
        } else {
            if (on) u8g2_DrawFrame(u8g2, bx, (uint8_t)(y - 6), bw, FM2_ROW_H);
            u8g2_DrawStr(u8g2, (uint8_t)(bx + 2), y, view->cells[c]);
        }
    }
    draw_eg_plot(u8g2, view, muted);
}

void display_fm_draw_frame(u8g2_t *u8g2, const fm_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    if (view == NULL) {
        u8g2_DrawStr(u8g2, 2, 8, "FM");
        return;
    }
    u8g2_DrawStr(u8g2, 2, 8, view->title);
    if (view->page == 1) {
        draw_page_freq_eg(u8g2, view);
        return;
    }
    char opl[6];
    snprintf(opl, sizeof(opl), "OP%u", (unsigned)(FM_GRAPH_OPS - view->selected_op));
    u8g2_DrawStr(u8g2, 106, 8, opl);

    fm_layout_t l;
    layout_build(&view->graph, &l);

    /* Connectors first so boxes paint over their ends. */
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        uint8_t m = view->graph.out_mask[i];
        for (uint8_t t = 0; t < FM_GRAPH_OPS; t++) {
            if (m & (1u << t)) draw_connector(u8g2, &l, i, t);
        }
    }
    /* Output bus under the carriers. */
    uint8_t bus_x0 = 255, bus_x1 = 0;
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        if (!(view->graph.out_mask[i] & FM_OUT_BIT)) continue;
        uint8_t cx = (uint8_t)(box_x(&l, i) + FM_BOX_W / 2);
        if (cx < bus_x0) bus_x0 = cx;
        if (cx > bus_x1) bus_x1 = cx;
        u8g2_DrawVLine(u8g2, cx, (uint8_t)(box_y(&l, i) + FM_BOX_H), 1);
    }
    if (bus_x0 <= bus_x1) {
        u8g2_DrawHLine(u8g2, bus_x0, (uint8_t)(FM_BOTTOM_Y + 1), (uint8_t)(bus_x1 - bus_x0 + 1));
    }

    u8g2_SetFont(u8g2, u8g2_font_4x6_tr);
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        uint8_t x = box_x(&l, i), y = box_y(&l, i);
        char d[2] = { (char)('0' + (FM_GRAPH_OPS - i)), '\0' };
        bool sel = (view->selected_op == i);
        if (sel) {
            u8g2_DrawBox(u8g2, x, y, FM_BOX_W, FM_BOX_H);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, (uint8_t)(x + 3), (uint8_t)(y + 6), d);
            u8g2_SetDrawColor(u8g2, 1);
        } else {
            if (view->linking && (view->link_bad & (1u << i))) {
                draw_frame_dotted(u8g2, x, y, FM_BOX_W, FM_BOX_H);
            } else {
                u8g2_DrawFrame(u8g2, x, y, FM_BOX_W, FM_BOX_H);
            }
            u8g2_DrawStr(u8g2, (uint8_t)(x + 3), (uint8_t)(y + 6), d);
        }
        if (view->muted & (1u << i)) {
            /* Corner to corner, inverted over the filled selected box. */
            if (sel) u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawLine(u8g2, x, (uint8_t)(y + FM_BOX_H - 1), (uint8_t)(x + FM_BOX_W - 1), y);
            u8g2_SetDrawColor(u8g2, 1);
        }
        bool framed = view->linking ? (i == view->link_src || i == view->link_cursor)
                                    : (view->cursor == FM_CUR_OP_BASE + i);
        if (framed) {
            /* Cursor on the box itself (vs. parked in the panel), or the link
             * source / link cursor: outer frame. */
            u8g2_DrawFrame(u8g2, (uint8_t)(x - 2), (uint8_t)(y - 2), FM_BOX_W + 4, FM_BOX_H + 4);
        }
        if (view->graph.fb_op == i) draw_feedback_loop(u8g2, x, y);
    }

    /* Parameter panel for the selected operator. */
    for (uint8_t r = 0; r < FM_PANEL_ROWS; r++) {
        uint8_t y = (uint8_t)(FM_PANEL_Y0 + r * FM_PANEL_ROW_H);
        bool on = !view->linking && (view->cursor == FM_CUR_RATIO + r);
        bool struck = (FM_CUR_RATIO + r == FM_CUR_FB) && !view->fb_applies;
        uint8_t w = (uint8_t)u8g2_GetStrWidth(u8g2, view->rows[r]);
        if (on && view->editing) {
            u8g2_DrawBox(u8g2, FM_PANEL_X + 4, (uint8_t)(y - 6), 128 - (FM_PANEL_X + 4), FM_PANEL_ROW_H);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, FM_PANEL_X + 5, y, view->rows[r]);
            if (struck) u8g2_DrawHLine(u8g2, FM_PANEL_X + 5, (uint8_t)(y - 3), w);
            u8g2_SetDrawColor(u8g2, 1);
        } else {
            if (on) u8g2_DrawStr(u8g2, FM_PANEL_X, y, ">");
            u8g2_DrawStr(u8g2, FM_PANEL_X + 5, y, view->rows[r]);
            if (struck) u8g2_DrawHLine(u8g2, FM_PANEL_X + 5, (uint8_t)(y - 3), w);
        }
    }
}
