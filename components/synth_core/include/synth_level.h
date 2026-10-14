#pragma once

/* ── Per-synth output level (amy_event.synth_level) ──────────────────────
 * The one writer of AMY's synth_level for app synths 0..SYNTH_SLOT_COUNT-1.
 * Three factors per synth, the product sent: one LFO AMP overshoot cap per
 * mod slot (VOICE_LFO_DEPTH_AMP, voice_config.h; written by voice_config
 * only) and the mixer gain (mixer.h; written by the mixer only). AMY applies the level
 * at ingest, linear, to every audible osc of the synth's voices, and resets
 * it to 1.0 on every patch load.
 *
 * Contract:
 * - Execution context: UI task (Core 0) only. Sends go through amy_helpers,
 *   never under amy_queue_lock; nothing here allocates or blocks beyond the
 *   ingress FIFO's own back-pressure.
 * - synth < SYNTH_SLOT_COUNT; other values are ignored (the LFO bank synths
 *   and out-of-range ids never carry a level).
 * - The caller guarantees the synth has an instrument (a configured patch)
 *   when it calls a setter or reassert; AMY prints a stderr line otherwise.
 * - Every setter and reassert sends one event, also when the factor is
 *   unchanged (the caller may be re-establishing the value after a patch
 *   load reset it). */

#include <stdint.h>

/* All factors 1.0; nothing sent. Call once from synth_ui_init(), before
 * sequencer_core_init() wires the first LFOs. */
void  synth_level_init(void);

/* LFO AMP overshoot cap of mod slot `slot` (0 or 1), 0..1 (1.0 while that
 * slot wires no AMP target). Setting one slot leaves the other's cap as it
 * was, so the two LFO wirings never order-depend on each other.
 * voice_config only. */
void  synth_level_set_lfo_cap(uint8_t synth, uint8_t slot, float cap);

/* Mixer gain, 0..1. The mixer only. */
void  synth_level_set_mix(uint8_t synth, float gain);

/* Both caps x gain, as last sent (1.0 for an ignored synth). */
float synth_level_get(uint8_t synth);

/* Resend the stored product. */
void  synth_level_reassert(uint8_t synth);
