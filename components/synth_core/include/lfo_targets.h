#pragma once

/* ── Shared-LFO target map per patch family ──────────────────────────────
 * Which voice-relative oscs (bit n = osc n) each LFO target addresses when a
 * patch is wired to a shared LFO synth (voice_lfo_wire() in voice_config.h):
 * voice_mask takes FILTER, AMP, PAN and DIST, pitch_mask PITCH, scan_mask
 * SCAN. Osc layout per family:
 * - Juno (0..127): osc 0 the SILENT head carrying filter, EG0 and amp; osc 1
 *   the patch's own LFO; oscs 2..5 pulse, saw, sub pulse, noise.
 * - DX7 bank and FM range: osc 0 the ALGO control osc.
 * - Bass presets: oscs 0-1 audible, chained.
 * - Additive range (presets and the custom voice): osc 0 the BYO_PARTIALS
 *   parent. AMY sets each PARTIAL's velocity from the parent's amp every
 *   block and carries the parent's frequency modulation to the partials
 *   (interp_partials.c), so osc 0 is the voice's VCA and pitch. A FILTER
 *   target acts on the parent only; its effect on the partials is
 *   unverified. No SCAN.
 * Raw waves and wavetables are not mapped: their layout depends on unison and
 * belongs to the consumer (seq_track_voice_layout()). The piano (256) and
 * anything else are not mapped either; such a consumer stays on its software
 * stepper. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t voice_mask, pitch_mask, scan_mask;
} lfo_target_map_t;

/* True with *out filled for a mapped family, false for any other patch (*out
 * untouched). out may be NULL to ask only. Pure; any task. */
bool lfo_targets_of_patch(uint16_t patch, lfo_target_map_t *out);

/* True when the patch string wires mod slot 0 of the oscs in its target map
 * to its own LFO osc (osc 1, `L1`): Juno (all mapped oscs) and the DX7 bank
 * (the ALGO osc; the operators keep their own L1, which LFO2 never touches).
 * Wiring LFO2 there replaces that patch LFO, and only a patch re-apply
 * brings it back (voice_lfo_wire(), voice_config.h). Pure; any task. */
bool lfo_targets_patch_owns_slot0(uint16_t patch);
