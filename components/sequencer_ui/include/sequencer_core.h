#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void sequencer_core_init(void);
void sequencer_core_set_playing(bool playing);
void sequencer_core_set_step(uint8_t track, uint8_t step, bool state);
void sequencer_core_set_bpm(uint16_t bpm);
uint8_t sequencer_core_get_current_step(void);

#ifdef __cplusplus
}
#endif
