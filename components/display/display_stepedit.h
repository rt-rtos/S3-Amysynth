#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Step Trig popup for the step under the sequencer grid cursor: eight fields
 * (nine with Frame, present only on a wavetable track) in a five-row window
 * that scrolls with the cursor, a triangle marking the hidden direction.
 * Controls: CONTROLS.md. */

typedef enum {
    SE_FIELD_PITCH   = 0,
    SE_FIELD_PROB    = 1,
    SE_FIELD_RATCHET = 2,
    SE_FIELD_EVERY   = 3,
    SE_FIELD_PREV    = 4,
    SE_FIELD_VEL     = 5,   /* velocity_adj, signed percentage points */
    SE_FIELD_NUDGE   = 6,   /* signed ticks */
    SE_FIELD_TAPER   = 7,   /* ratchet taper, signed percent per sub-hit */
    SE_FIELD_FRAME   = 8,   /* wavetable frame lock; only while has_frame */
    SE_FIELD_COUNT,
} stepedit_field_t;

/* Rows the blue region fits below the yellow header. */
#define SE_VISIBLE_ROWS 5

typedef struct {
    uint8_t layer_idx;    /* 0-based; rendered as 1-based */
    uint8_t track_idx;    /* 0-based; rendered as 1-based */
    uint8_t step_idx;     /* 0-based; rendered as 1-based */
    int8_t  pitch_ofs;    /* -SEQ_STEP_PITCH_OFS_MAX..+, semitones, 0 neutral */
    uint8_t prob;         /* 0..100 */
    uint8_t ratchet;      /* 1..SEQ_MAX_RATCHET */
    uint8_t every;        /* 1..SEQ_STEP_EVERY_MAX; 1 = every loop */
    uint8_t prev;         /* 0/1 */
    uint8_t field_cursor; /* stepedit_field_t */
    uint8_t editing;      /* 1 = adjust mode (cursored row inverted) */
    int8_t  vel_adj;      /* -SEQ_STEP_VEL_ADJ_MAX..+, percentage points, 0 neutral */
    int8_t  nudge;        /* -SEQ_STEP_NUDGE_MAX..+, ticks, 0 on-grid */
    int8_t  taper;        /* -SEQ_STEP_TAPER_MAX..+, percent per sub-hit, 0 flat */
    uint8_t first_row;    /* stepedit_field_t drawn on the top visible row */
    uint8_t has_frame;    /* 1 = the track plays a wavetable: Frame is listed */
    uint8_t frame;        /* frame lock: 0 = none ("--"), 1..64 = frame 0..63 */
} stepedit_view_t;

void display_stepedit_draw_frame(u8g2_t *u8g2, const stepedit_view_t *view);

#ifdef __cplusplus
}
#endif
