#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Drone screen renderer (scrollable param list) ────────────────────────
 * Drone logic lives in synth_core (drone_core); synth_ui formats each
 * parameter into a label + value string and hands over a flat array. The
 * renderer draws a scrollable list, framing the selected value while editing. */

#define DRONE_LABEL_LEN      12
#define DRONE_VALUE_LEN      12

typedef struct {
    char    label[DRONE_LABEL_LEN];
    char    value[DRONE_VALUE_LEN];
    bool    has_glyph;   /* draw `glyph` between label and value          */
    uint8_t glyph;       /* 8-step mask, one cell per step (bit 0 = step 0),
                            filled = on                                    */
} drone_row_view_t;

typedef struct {
    const drone_row_view_t *rows;
    uint8_t count;
    uint8_t cursor;     /* highlighted row                       */
    bool    editing;    /* true => value of cursor row is editing */
} drone_view_t;

/* Draw the full drone screen (clears + sends the buffer) under a
 * caller-supplied title, so the stutter and normal drone screens share this
 * renderer. */
void display_drone_draw_frame_titled(u8g2_t *u8g2, const char *title,
                                     const drone_view_t *view);

#ifdef __cplusplus
}
#endif
