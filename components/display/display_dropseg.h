#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ball drop segment popup for the segment on the drop screen's cursor cell:
 * two fields, pitch offset and velocity, drawn as the Step Trig popup draws
 * its rows. Controls: CONTROLS.md. */

typedef enum {
    DS_FIELD_PITCH = 0,   /* pitch_ofs, signed semitones */
    DS_FIELD_VEL   = 1,   /* vel_adj, signed percentage points */
    DS_FIELD_COUNT,
} dropseg_field_t;

/* One-byte fields only: no padding, so the whole struct hashes safely. */
typedef struct {
    uint8_t col;          /* 0-based; rendered as 1-based */
    uint8_t row;          /* 0-based, 0 = top; rendered as 1-based */
    uint8_t field;        /* dropseg_field_t */
    uint8_t editing;      /* 1 = adjust mode (cursored row inverted) */
    int8_t  pitch_ofs;    /* -SEQ_STEP_PITCH_OFS_MAX..+, semitones, 0 neutral */
    int8_t  vel_adj;      /* -SEQ_STEP_VEL_ADJ_MAX..+, percentage points, 0 neutral */
} dropseg_view_t;

void display_dropseg_draw_frame(u8g2_t *u8g2, const dropseg_view_t *view);

#ifdef __cplusplus
}
#endif
