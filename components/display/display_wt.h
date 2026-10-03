#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Custom wavetable screen ─────────────────────────────────────────────
 * Title band: "WT CUSTOM" and the clean-note readout. Left: a grid of the
 * four per-keyframe rows (SHP, BRT, SYN, PK) under columns A and B, then
 * the RNG row; the cursor cell is framed while browsing and filled while
 * adjusting. Right: the 64-point waveform of frame 0 (A) or frame 63 (B),
 * or both overlaid. The caller (ui_screen_wt.c) formats every string; this
 * file only lays out and draws. */

/* Cursor stops, in walk order. */
enum {
    WT_CUR_A_SHP = 0,
    WT_CUR_A_BRT,
    WT_CUR_A_SYN,
    WT_CUR_A_PK,
    WT_CUR_B_SHP,
    WT_CUR_B_BRT,
    WT_CUR_B_SYN,
    WT_CUR_B_PK,
    WT_CUR_RNG,
    WT_CUR_COUNT,
};
#define WT_KEY_ROWS     4       /* SHP, BRT, SYN, PK */
#define WT_CELL_LEN     6
#define WT_VIEW_POINTS  64

/* Which waveform the right panel shows. */
enum {
    WT_SHOW_A = 0,
    WT_SHOW_B,
    WT_SHOW_BOTH,
};

typedef struct {
    uint8_t  cursor;                        /* WT_CUR_* */
    bool     editing;                       /* cursor cell is being adjusted */
    uint8_t  show;                          /* WT_SHOW_* */
    uint32_t generation;                    /* table rebuilds; redraws the preview */
    char     clean[12];                     /* "clean F#5" */
    char     cells[WT_CUR_COUNT][WT_CELL_LEN];
    int8_t   frame0[WT_VIEW_POINTS];        /* -127..127 */
    int8_t   frame63[WT_VIEW_POINTS];
} wt_view_t;

void display_wt_draw_frame(u8g2_t *u8g2, const wt_view_t *view);

#ifdef __cplusplus
}
#endif
