#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "priv_u8g2_seq.h"   /* seq_layer_type_t, seq_layer_t, SEQ_* defines */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Core lifecycle ── */
void sequencer_core_init(void);
void sequencer_core_set_playing(bool playing);
void sequencer_core_set_bpm(uint16_t bpm);
void sequencer_core_set_quantizer_enabled(bool enabled);
void sequencer_core_set_quantizer_root_note(uint8_t root_note);
void sequencer_core_set_quantizer_scale(uint8_t scale_index);
bool sequencer_core_get_quantizer_enabled(void);
uint8_t sequencer_core_get_quantizer_root_note(void);
uint8_t sequencer_core_get_quantizer_scale(void);
void sequencer_core_set_melodic_patch(uint16_t patch_number);
uint16_t sequencer_core_get_melodic_patch(void);

/* Returns the current playhead step for the given layer (0..num_steps-1).
 * When paused the last computed step is returned (display freezes). */
uint8_t sequencer_core_get_current_step(uint8_t layer_idx);

/* ── Layer management ── */

/* Add a new layer. Returns the new layer index (0..MAX_LAYERS-1), or
 * 0xFF if the layer table is full. Configures the AMY synth immediately. */
uint8_t          sequencer_core_add_layer(seq_layer_type_t type, uint8_t num_steps);
uint8_t          sequencer_core_get_num_layers(void);
seq_layer_type_t sequencer_core_get_layer_type(uint8_t layer_idx);

/* ── Per-layer step / note control ── */
void    sequencer_core_set_step(uint8_t layer_idx, uint8_t track,
                                uint8_t step, bool state);
void    sequencer_core_set_track_midi_note(uint8_t layer_idx, uint8_t track,
                                           uint8_t midi_note);
uint8_t sequencer_core_get_track_midi_note(uint8_t layer_idx, uint8_t track);
uint8_t sequencer_core_get_track_source_note(uint8_t layer_idx, uint8_t track);

#ifdef __cplusplus
}
#endif
