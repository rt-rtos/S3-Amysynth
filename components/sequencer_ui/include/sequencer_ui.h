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

/* ── Graph pop-up integration (isolated, easily removable) ───────────────────
 * Demo hooks for the reusable graph_popup widget. main.c calls these; the
 * pop-up state and all U8g2 plumbing live inside sequencer_ui.c. Removing these
 * declarations and their callers fully reverts the integration. */

/* True while the graph pop-up overlay is open. */
bool sequencer_ui_graph_is_active(void);

/* Open the curve editor seeded from the current melodic ADSR envelope. */
void sequencer_ui_graph_open_envelope(void);

/* Route input to the pop-up while it is active. Each returns true if the
 * pop-up consumed the event (caller should then skip normal sequencer input).
 * sequencer_ui_graph_handle_button(is_long): is_long=true => long-press/cancel. */
bool sequencer_ui_graph_handle_encoder(long delta);
bool sequencer_ui_graph_handle_button(bool is_long);

/* Toggle the encoder adjust axis (vertical level <-> horizontal time) while the
 * graph editor is open. Returns true if the event was consumed (graph open). */
bool sequencer_ui_graph_toggle_axis(void);

/* Toggle the graph time range SHORT(2s linear) <-> LONG(15s, log-squashed tail)
 * while the editor is open. Re-seeds the curve. Returns true if consumed. */
bool sequencer_ui_graph_toggle_range(void);

#ifdef __cplusplus
}
#endif
