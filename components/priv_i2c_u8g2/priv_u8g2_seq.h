#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEQ_TRACKS 4
#define SEQ_STEPS 16

typedef struct {
    bool grid[SEQ_TRACKS][SEQ_STEPS];
    uint16_t bpm;
    uint8_t current_pattern;
    uint8_t current_step;      // 0-15
    bool playing;
    uint8_t selected_track;    // 0-3
    uint8_t selected_step;     // 0-15
    bool edit_mode;            // true if editing steps, false if navigating
} priv_u8g2_seq_state_t;

/**
 * @brief Draw the sequencer frame based on the provided state.
 * 
 * @param u8g2 Pointer to the u8g2 instance.
 * @param state Pointer to the sequencer state.
 */
void priv_u8g2_seq_draw_frame(u8g2_t *u8g2, const priv_u8g2_seq_state_t *state);

#ifdef __cplusplus
}
#endif
