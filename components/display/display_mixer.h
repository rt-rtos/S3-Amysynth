#pragma once

#include "u8g2.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Mixer screen ────────────────────────────────────────────────────────
 * Header in the yellow band (title, divider at y 15), then up to
 * MIXER_VIS_ROWS rows of 8 px below the seam: the row name
 * (a drum-layer track row prints its role label instead), a level bar whose
 * fill is the fader position, and an M cell: "M" filled for the mixer's own
 * mute, a plain "m" when only the source's own mute is set. Two 3-char quick
 * cells follow the M cell; a settings row shows its value text where the
 * bar would be. The cursor row's name is filled; the focused element of the
 * cursor row is drawn inverted (the bar, a quick cell, or the whole row for
 * the dive). Scroll triangles at the right edge mark rows above and below
 * the window. The caller (ui_screen_mixer.c) formats every string; this
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
    char    q1[4], q2[4]; /* quick cells, "" none */
    char    text[8];      /* settings rows: value text in place of the bar */
} mixer_view_row_t;

typedef struct {
    char    title[12];
    uint8_t row_count;        /* rows filled, <= MIXER_VIS_ROWS */
    uint8_t cursor;           /* index into rows[], MIXER_NO_CURSOR none */
    uint8_t focus;            /* MIXER_FOCUS_* */
    uint8_t more_above, more_below;
    mixer_view_row_t rows[MIXER_VIS_ROWS];
} mixer_view_t;

void display_mixer_draw_frame(u8g2_t *u8g2, const mixer_view_t *view);

#ifdef __cplusplus
}
#endif
