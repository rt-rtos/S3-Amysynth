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
    char label[DRONE_LABEL_LEN];
    char value[DRONE_VALUE_LEN];
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

/* ── Drone visualiser overlay ───────────────────────────────────────────────
 * Plain data snapshot passed from synth_ui (which owns drone_core access).
 * All values are normalised or primitive so display stays free of synth_core. */
typedef struct {
    float   sweep_lo_norm;   /* sweep_lo mapped to 0..1 across 100..8000 Hz */
    float   sweep_hi_norm;   /* sweep_hi mapped to 0..1 across 100..8000 Hz */
    float   amp_const;       /* 0..1 PEAK knob value (on-beat level)           */
    float   amp_mod;         /* 0..1 DUCK knob value (duck depth)              */
    float   amp_floor_norm;  /* off-beat amplitude from drone_get_amp_levels_norm() */
    float   amp_ceil_norm;   /* on-beat amplitude from drone_get_amp_levels_norm()  */
    float   resonance;       /* raw Q value 0.1..8.0 for needle + label          */
    uint8_t rate_idx;        /* drone_rate_t enum (0=1/4, 1=1/8, 2=1/16, 3=1/32) */
    uint8_t sweep_bars;      /* sweep period in bars 1..16                        */
    uint8_t pattern_mask;    /* 8-bit step on/off mask (LSB = step 0) */
    float   gate_len;        /* 0..1 chop length (PULSE duty) */
    bool    wave_mode;       /* true = WAVE (show amp/gate), false = PATCH */
} drone_vis_t;

/* Draw the full-screen drone visualiser overlay (clears + sends the buffer). */
void display_drone_vis_draw(u8g2_t *u8g2, const drone_vis_t *vis);

#ifdef __cplusplus
}
#endif
