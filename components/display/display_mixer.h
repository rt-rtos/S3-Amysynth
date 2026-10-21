#pragma once

#include "u8g2.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Mixer screen ────────────────────────────────────────────────────────
 * Header in the yellow band (title, the transport clock at the right of
 * the title, divider at y 15), then up to
 * MIXER_VIS_ROWS rows of 8 px below the seam: the row name
 * (a drum-layer track row prints its role label instead), a level bar whose
 * fill is the fader position, with the row's link group letter XORed over
 * its right end, and an M cell: "M" filled for the mixer's own
 * mute, a plain "m" when only the source's own mute is set. Two 4-char quick
 * cells follow the M cell (values only; the page puts the selected cell's
 * label in the title); a settings row shows its value text where the
 * bar would be. Each row has a 1 px bottom border on its free line (y + 7)
 * from x 0 to its last cell (Q2, Q1, M cell, else the bar's end), none on a
 * fully inverted row. The cursor row's name is filled; `focus` picks the
 * cursor row's marked element: a quick cell (Q1 / Q2, the page's PARAM
 * mode), drawn as a 1 px outline over x .. x + 19, y .. y + 7 or filled
 * while `editing`; the bar (or settings value), inverted while SHIFT is
 * held; or the whole row, inverted, for MIXER_FOCUS_DIVE. Scroll triangles at the right edge mark rows above
 * and below the window. The caller (ui_screen_mixer.c) formats every string; this
 * file only lays out and draws. Everything stays above the hint strip. */

#define MIXER_VIS_ROWS   5
#define MIXER_NO_CURSOR  0xFFu
#define MIXER_NO_FADE    0xFFu
#define MIXER_NO_DRUM    0xFFu

enum {
    MIXER_FOCUS_NONE = 0,
    MIXER_FOCUS_LEVEL,
    MIXER_FOCUS_Q1,
    MIXER_FOCUS_Q2,
    MIXER_FOCUS_DIVE,
};

typedef struct {
    char    name[5];
    uint8_t drum_track;   /* MIXER_NO_DRUM unless a drum-layer track row */
    uint8_t pos;          /* 0..100 */
    uint8_t fade_to;      /* MIXER_NO_FADE none */
    uint8_t mute;         /* mixer soft mute */
    uint8_t hard_mute;    /* the source's own mute, display only */
    uint8_t has_level;    /* 0 for a settings row */
    uint8_t has_mute;
    char    q1[5], q2[5]; /* quick cells, up to 4 chars, "" none */
    char    text[8];      /* settings rows: value text in place of the bar */
    uint8_t link;         /* MIXER_LINK_NONE or the group, shown as A..C in the bar */
} mixer_view_row_t;

typedef struct {
    char    title[12];
    char    clock[9];         /* transport clock text, "" while stopped */
    uint8_t row_count;        /* rows filled, <= MIXER_VIS_ROWS */
    uint8_t cursor;           /* index into rows[], MIXER_NO_CURSOR none */
    uint8_t focus;            /* MIXER_FOCUS_* */
    uint8_t editing;          /* Q1 / Q2 focus: 1 fills the cell, 0 outlines it */
    uint8_t more_above, more_below;
    mixer_view_row_t rows[MIXER_VIS_ROWS];
} mixer_view_t;

void display_mixer_draw_frame(u8g2_t *u8g2, const mixer_view_t *view);

#ifdef __cplusplus
}
#endif
