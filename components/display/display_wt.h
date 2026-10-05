#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Custom wavetable screen ─────────────────────────────────────────────
 * No title. Top band, two 4x6 rows: left of a vertical border the keyframe
 * tabs A M B (the focused one filled) above the RNG cell; right of it the
 * focused keyframe's five parameters, SHP and WID on row 1 (the third slot
 * left free for the BLE badge), BRT, SYN and PK on row 2. The cursor cell is
 * framed while browsing and filled while adjusting. Below, the full-width
 * 128-point waveform: the two unfocused keyframes as dots on even columns,
 * the focused one as a line. With the fourth tab, S (scan), focused the
 * parameter cells give way to a FRAME readout and a position ruler marking
 * A, M and B, and the waveform is that one frame with no other trace. The
 * caller (ui_screen_wt.c) formats every value string; this file only lays
 * out and draws. */

/* Cursor stops, in walk order. */
enum {
    WT_CUR_SHP = 0,
    WT_CUR_WID,
    WT_CUR_BRT,
    WT_CUR_SYN,
    WT_CUR_PK,
    WT_CUR_RNG,
    WT_CUR_COUNT,
};
#define WT_CELL_LEN     6
#define WT_VIEW_KEYS    3
#define WT_VIEW_KEY_SCAN WT_VIEW_KEYS    /* `key` value of the scan tab */
#define WT_VIEW_FRAMES  64
#define WT_VIEW_POINTS  128

typedef struct {
    uint8_t  cursor;                        /* WT_CUR_* */
    bool     editing;                       /* cursor cell is being adjusted */
    uint8_t  key;                           /* focused keyframe 0..2, or WT_VIEW_KEY_SCAN */
    uint8_t  frame;                         /* scan tab: the frame shown, 0..63 */
    char     frame_txt[4];                  /* scan tab: that frame as text */
    uint32_t generation;                    /* table rebuilds; redraws the preview */
    char     cells[WT_CUR_COUNT][WT_CELL_LEN];      /* focused keyframe + RNG */
    int8_t   wave[WT_VIEW_KEYS][WT_VIEW_POINTS];    /* -127..127, A M B; scan tab: [0] is the frame */
} wt_view_t;

void display_wt_draw_frame(u8g2_t *u8g2, const wt_view_t *view);

#ifdef __cplusplus
}
#endif
