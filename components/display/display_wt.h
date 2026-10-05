#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Custom wavetable screen ─────────────────────────────────────────────
 * No title. Top band, two 4x6 rows: left of a vertical border the keyframe
 * tabs A M B (the focused one filled) above the harmonics cell (the
 * harmonic count and its clean note, no label); right of it the
 * focused keyframe's five parameters, SHP and WID on row 1 (the third slot
 * left free for the BLE badge), BRT, SYN and PK on row 2. The cursor cell is
 * framed while browsing and filled while adjusting. Below, the full-width
 * 128-point waveform: the two unfocused keyframes as dots on even columns,
 * the focused one as a line. With the fourth tab, S (scan), focused the
 * parameter cells give way to a FRAME readout and a position ruler marking
 * A, M and B, and the waveform is that one frame with no other trace; with
 * `spectrum` set the same area shows that frame's harmonics as bars, harmonic
 * 1 on the left, the Range's harmonic count filling the width, 60 dB from
 * top to bottom with dotted lines 20 and 40 dB down. The
 * caller (ui_screen_wt.c) formats every value string; this file only lays
 * out and draws. */

/* Cursor stops, in walk order. */
enum {
    WT_CUR_SHP = 0,
    WT_CUR_WID,
    WT_CUR_BRT,
    WT_CUR_SYN,
    WT_CUR_PK,
    WT_CUR_HRM,
    WT_CUR_COUNT,
};
#define WT_CELL_LEN     8
#define WT_VIEW_KEYS    3
#define WT_VIEW_KEY_SCAN WT_VIEW_KEYS    /* `key` value of the scan tab */
#define WT_VIEW_FRAMES  64
#define WT_VIEW_HARMONICS 63
#define WT_VIEW_POINTS  128

typedef struct {
    uint8_t  cursor;                        /* WT_CUR_* */
    bool     editing;                       /* cursor cell is being adjusted */
    uint8_t  key;                           /* focused keyframe 0..2, or WT_VIEW_KEY_SCAN */
    uint8_t  frame;                         /* scan tab: the frame shown, 0..63 */
    char     frame_txt[4];                  /* scan tab: that frame as text */
    bool     spectrum;                      /* scan tab: bars, not the waveform */
    uint8_t  harm_count;                    /* bars shown, 1..WT_VIEW_HARMONICS */
    uint8_t  harm[WT_VIEW_HARMONICS];       /* half-dB below full scale per harmonic */
    uint32_t generation;                    /* table rebuilds; redraws the preview */
    char     cells[WT_CUR_COUNT][WT_CELL_LEN];      /* focused keyframe + harmonics */
    int8_t   wave[WT_VIEW_KEYS][WT_VIEW_POINTS];    /* -127..127, A M B; scan tab: [0] is the frame */
} wt_view_t;

void display_wt_draw_frame(u8g2_t *u8g2, const wt_view_t *view);

#ifdef __cplusplus
}
#endif
