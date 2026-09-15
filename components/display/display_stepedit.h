#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Step Trig editor renderer ─────────────────────────────────────────────
 * Full-screen popup (same convention as the ADSR / filter / LFO editors) for
 * one step's probability / ratchet / conditional trig / micro-timing, addressed
 * by the sequencer grid's existing cursor - no separate cursor of its own.
 *
 *   STEP L1 T2 S05
 *   ───────────────
 *  >Pitch   : +3
 *   Prob    : 75%
 *   Ratchet : 2
 *   Every   : 2
 *   Prev    : OFF   v   <- more fields below
 *
 * Eight fields behind a five-row window: only the panel height limits it, so
 * the window scrolls with the cursor and a triangle at the right edge marks the
 * hidden direction. The first five are the opening screen, unchanged.
 *
 * Select/adjust workflow, as on the DEV screen: encoder turns navigate the
 * field cursor (triangle marker), short-press enters adjust mode (row
 * inverted), turns change the value, short-press confirms. Prev is a boolean
 * and click-toggles directly, with no adjust phase. Every and Prev are
 * independent conditions (both must hold for the step to fire); Every 1 =
 * every loop. */

typedef enum {
    SE_FIELD_PITCH   = 0,
    SE_FIELD_PROB    = 1,
    SE_FIELD_RATCHET = 2,
    SE_FIELD_EVERY   = 3,
    SE_FIELD_PREV    = 4,
    SE_FIELD_VEL     = 5,   /* velocity_adj, signed percentage points */
    SE_FIELD_NUDGE   = 6,   /* signed ticks */
    SE_FIELD_TAPER   = 7,   /* ratchet taper, signed percent per sub-hit */
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
} stepedit_view_t;

void display_stepedit_draw_frame(u8g2_t *u8g2, const stepedit_view_t *view);

#ifdef __cplusplus
}
#endif
