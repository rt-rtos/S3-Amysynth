#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Additive / partials voice engine (AMY BYO_PARTIALS) ─────────────────────
 * Additive voice: osc 0 is AMY's BYO_PARTIALS control osc and carries the
 * shared amp envelope (the per-track ADSR editor targets it); oscs 1..N are
 * PARTIAL sines carrying the spectrum, so oscs_per_voice is N+1. AMY finds the
 * children by adjacency and the parent's preset (= N), with no algo_source
 * routing.
 * Per partial: ratio above the note, level (0 = silent, so presets use fewer
 * partials without changing the layout), optional decay_ms (a local decay so
 * upper partials ring down faster; the bell preset uses it). */

/* Hard ceiling on the partial count: a voice costs ADD_MAX_PARTIALS + 1 oscs
 * from the global pool, so 13 oscs/voice worst case keeps a 4-row x 4-voice
 * additive layer at 208 oscs, under budget. Compile-time fixed, like
 * FM_NUM_OPS. */
#define ADD_MAX_PARTIALS 12

typedef struct {
    uint8_t num_partials;               /* 1..ADD_MAX_PARTIALS (N)              */
    float   ratio[ADD_MAX_PARTIALS];    /* freq ratio of each partial (1,2,3..) */
    float   level[ADD_MAX_PARTIALS];    /* 0..1 relative level of each partial  */
    float   decay_ms[ADD_MAX_PARTIALS]; /* per-partial decay; 0 = follow parent */
} additive_voice_t;

/* The single live-editable "custom" additive voice
 * (SEQ_PATCH_ADDITIVE_CUSTOM), owned by this module. Editors mutate it
 * directly (the s_fm_voice / s_fx convention) then call
 * sequencer_core_additive_voice_changed(). It starts as the drawbar-organ
 * default. */
extern additive_voice_t s_additive_voice;

/* Drawbar-organ default: first 8 harmonics at 1/n roll-off, all following the
 * parent envelope. Shared starting point for s_additive_voice and presets. */
void additive_voice_default(additive_voice_t *v);

/* Full (re)configure of one synth slot as an (N+1)-osc additive voice. Call
 * after allocating/reallocating the synth (patch switch, layer creation). */
void additive_voice_configure_track(uint8_t synth_id, uint16_t num_voices,
                                    const additive_voice_t *voice);

/* Live update of an already-configured additive voice (ratio/level only),
 * cheap enough for every encoder tick. Changing num_partials is NOT supported
 * here - it changes oscs_per_voice and the parent's preset, so it needs a full
 * additive_voice_configure_track(). */
void additive_voice_push_live(uint8_t synth_id, const additive_voice_t *voice);

#ifdef __cplusplus
}
#endif
