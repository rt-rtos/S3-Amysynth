#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Multi-oscillator bass presets (SEQ_PATCH_BASS_1..3, sequencer_core.h) ───
 * Three two-osc bass presets selectable as melodic layer or arp patches; the
 * two audible oscs chain via chained_osc for note propagation.
 *
 * BASS_1  PULSE + detuned SAW_DOWN, LPF24 swept by EG1
 * BASS_2  acid pluck: SINE + SAW_DOWN, resonant LPF24 swept by EG1
 * BASS_3  bright bass: PULSE + SAW_DOWN one octave down, no filter
 *
 * Each preset carries its own envelope. Attack floor: every eg0_times[0] is
 * >= 2 ms. */
void bass_preset_configure_track(uint8_t synth_id, uint16_t patch,
                                 uint16_t num_voices);

/* The amp CONST `patch`'s build gives voice-relative `osc` (the preset's
 * balance between its two audible oscs); 1.0 for any other patch or osc. A
 * consumer that writes amp CONST over the preset's oscs (the drones' level
 * and gate) multiplies by it so the balance survives. Pure; any task. */
float bass_preset_osc_level(uint16_t patch, uint8_t osc);

#ifdef __cplusplus
}
#endif
