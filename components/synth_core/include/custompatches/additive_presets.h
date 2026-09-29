#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Fixed additive/partials presets (SEQ_PATCH_ADDITIVE_ORGAN/BELL) ─────────
 * Fixed-parameter voices on additive_voice_configure_track(): a drawbar organ
 * (first 8 harmonics, 1/n roll-off, sustained by the parent envelope) and an
 * inharmonic bell (free-bar ratios with staggered per-partial ring-down).
 * Unused partials are silenced with level=0 so the osc layout stays uniform. */
void additive_preset_configure_track(uint8_t synth_id, uint16_t patch,
                                     uint16_t num_voices);

#ifdef __cplusplus
}
#endif
