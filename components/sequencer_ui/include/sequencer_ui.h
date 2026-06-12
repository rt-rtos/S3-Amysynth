#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "u8g2.h"
#include "priv_u8g2_seq.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef priv_u8g2_seq_state_t sequencer_ui_state_t;

/* Initialise the display, create the drum layer (index 0), and start
 * the FreeRTOS UI task. Must be called after amy_start(). */
void sequencer_ui_init(u8g2_t *u8g2);

/* Add a new sequencer layer (drum or melodic). Returns the layer index
 * or 0xFF if the layer table is full. Safe to call after init. */
uint8_t sequencer_ui_add_layer(seq_layer_type_t type, uint8_t num_steps);

/* Advance the active layer displayed/edited on screen.
 * Resets the cursor to track 0, step 0, edit_mode = true. */
void sequencer_ui_cycle_active_layer(void);

/* Input dispatch ── called from encoder / button tasks */
void sequencer_ui_handle_encoder(long delta);
void sequencer_ui_handle_button(void);
void sequencer_ui_toggle_playing(void);
void sequencer_ui_set_bpm(uint16_t bpm);
void sequencer_ui_adjust_track_note(int delta);
void sequencer_ui_cycle_melodic_patch(int delta);
void sequencer_ui_set_drum_select_mode(bool held);
void sequencer_ui_set_patch_select_mode(bool held);

/* Global state (bpm is read directly by encoder_task in main.c) */
extern sequencer_ui_state_t seq_state;

#ifdef __cplusplus
}
#endif
