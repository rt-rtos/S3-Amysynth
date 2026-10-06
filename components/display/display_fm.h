#pragma once

#include "u8g2.h"
#include "custompatches/fm_graph.h"   /* fm_graph_view_t */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── FM operator screen, two pages ───────────────────────────────────────
 * Page 0: DX7-chart layout: carriers on the bottom row, each modulator
 * stacked above what it modulates, a self-loop on the feedback operator, a
 * diagonal stroke through a muted operator's box. Left 80 px hold the graph,
 * the right column the selected operator's parameter rows. FB is the one
 * voice-level row among per-operator ones, so it is struck through unless
 * the selected operator carries the feedback loop. Every bit of a fan-out
 * mask gets a connector, but a box is stacked above its first target only,
 * so a further target's connector may cross boxes.
 * While linking (page 0): outer frames on the source and link-cursor boxes,
 * a dotted outline on boxes in link_bad, and no page-0 cursor marks (box
 * frame, panel '>' or inverted row).
 * Page 1: the selected operator's frequency and envelope as cells in
 * FM2_CUR_* order, two per line (T and L share one): the OP/FRQ and
 * coarse/fine lines full width, the four T/L lines a compact left column with
 * a read-only plot of the envelope to their right. The plot marks the segment
 * of the Tn and the point of the Ln under the cursor, and is dotted while the
 * operator is muted.
 * The caller (ui_screen_fm.c) formats every string and computes the plot
 * trace; this file only lays out and draws. Operator index i is labelled
 * OP(6-i). */

/* Page 0 cursor positions: the six operator boxes, then the panel rows. */
enum {
    FM_CUR_OP_BASE = 0,           /* + operator index 0..5 */
    FM_CUR_RATIO   = 6,
    FM_CUR_LEVEL,
    FM_CUR_TO,
    FM_CUR_FB,
    FM_CUR_ALGO,
    FM_CUR_COUNT,
};
#define FM_PANEL_ROWS  (FM_CUR_COUNT - FM_CUR_RATIO)
#define FM_ROW_LEN     12

/* Page 1 cursor positions, which are also the cell order. */
enum {
    FM2_CUR_OP = 0,
    FM2_CUR_FRQ,
    FM2_CUR_COARSE,
    FM2_CUR_FINE,
    FM2_CUR_R1,
    FM2_CUR_L1,
    FM2_CUR_R2,
    FM2_CUR_L2,
    FM2_CUR_R3,
    FM2_CUR_L3,
    FM2_CUR_R4,
    FM2_CUR_L4,
    FM2_CUR_COUNT,
};
#define FM_CELL_LEN    12

/* Page 1 plot trace: one DX7 level (0..99) per pixel column. Breakpoint
 * columns: 0 start (L4), 1..3 the ends of T1..T3 (L1..L3), 4 the end of the
 * fixed sustain stub (still L3), 5 the end of T4 (L4, the last column). */
#define FM_PLOT_W       77
#define FM_PLOT_STUB_W  10
#define FM_PLOT_BPS     6

typedef struct {
    fm_graph_view_t graph;
    uint8_t page;                     /* 0 = graph + panel, 1 = frequency/EG */
    uint8_t selected_op;              /* operator whose rows the panel shows */
    uint8_t cursor;                   /* FM_CUR_* on page 0, FM2_CUR_* on page 1 */
    uint8_t muted;                    /* bit i: operator i muted */
    bool    editing;                  /* cursor row's value is being adjusted */
    bool    fb_applies;               /* selected op is the feedback op; else FB row struck */
    bool    linking;                  /* page 0 link mode; the link_* fields are 0 otherwise */
    uint8_t link_src;                 /* source operator */
    uint8_t link_cursor;              /* box under the link cursor */
    uint8_t link_bad;                 /* bit t: a click on box t would be refused */
    char    title[14];                /* "FM ALG 12" / "FM CUSTOM" / "OP1 FREQ/EG" / "LINK OP1" */
    char    rows[FM_PANEL_ROWS][FM_ROW_LEN];
    char    cells[FM2_CUR_COUNT][FM_CELL_LEN];
    uint8_t eg_level[4];              /* page 1: L1..L4, for the point marks */
    uint8_t plot_bp_x[FM_PLOT_BPS];   /* page 1: breakpoint columns, see above */
    uint8_t plot_level[FM_PLOT_W];    /* page 1: trace, DX7 level per column */
} fm_view_t;

void display_fm_draw_frame(u8g2_t *u8g2, const fm_view_t *view);

#ifdef __cplusplus
}
#endif
