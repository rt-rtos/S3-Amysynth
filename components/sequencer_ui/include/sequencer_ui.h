#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "u8g2.h"   // for the draw function
#include "priv_u8g2_seq.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef priv_u8g2_seq_state_t sequencer_ui_state_t;

void sequencer_ui_init(u8g2_t *u8g2);
void sequencer_ui_handle_encoder(long delta);
void sequencer_ui_handle_button(void);
void sequencer_ui_toggle_playing(void);
void sequencer_ui_set_bpm(uint16_t bpm);

// For priv_u8g2_seq_draw_frame to stay 100% unchanged
extern sequencer_ui_state_t seq_state;

#ifdef __cplusplus
}
#endif
