#pragma once

#include "chord_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uint8_t intervals[12];
    uint8_t size;
} musical_scale_t;

typedef struct {
    uint8_t root_note;
    uint8_t scale_index;
    bool    enabled;
} quantizer_state_t;

uint8_t quantizer_scale_count(void);
const musical_scale_t *quantizer_get_scale(uint8_t scale_index);

uint8_t quantizer_clamp_midi(int32_t midi_note);
uint8_t quantizer_snap_midi_note(uint8_t midi_note, uint8_t root_note,
                                 const musical_scale_t *scale);

/* Snap midi_note to the nearest chord tone of (root 0-11, chord_type).
 * root is a chromatic pitch class; the function searches across all octaves. */
uint8_t quantizer_snap_to_chord(uint8_t midi_note, uint8_t root,
                                chord_type_t chord_type);

/* The -1-terminated semitone interval row (6 slots, max 5 notes) for a chord
 * type, or NULL if out of range - the single shared interval table. */
const int8_t *quantizer_chord_intervals(chord_type_t chord_type);

/* Most rows quantizer_voice_chord voices together. */
#define QUANTIZER_VOICE_MAX 5

/* Voice chord (root pitch class, chord_type) across n rows. refs[i] is row i's
 * reference note (its authored source note); out[i] receives a chord tone near
 * it, chosen jointly so the rows cover the chord's roles (root, third, fifth,
 * seventh, extension) instead of each snapping to its own nearest tone.
 * Obligations: refs and out hold n entries, n <= QUANTIZER_VOICE_MAX, refs in
 * 0..127. Guarantees: out[i] in 0..127; the result is a pure function of the
 * arguments (exhaustive minimum-cost search, ties to the first assignment with
 * row 0 varying slowest). Deterministic, stateless, no allocation, callable
 * from any task. A chord type without intervals (or n above the maximum)
 * falls back to quantizer_snap_to_chord per row; n == 0 writes nothing. */
void quantizer_voice_chord(const uint8_t *refs, uint8_t n, uint8_t root,
                           chord_type_t chord_type, uint8_t *out);

#ifdef __cplusplus
}
#endif