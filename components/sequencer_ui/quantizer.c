#include "quantizer.h"

static const musical_scale_t s_scales[] = {
    { .name = "Chromatic",        .intervals = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, .size = 12 },
    { .name = "Major (Ionian)",   .intervals = {0, 2, 4, 5, 7, 9, 11},                 .size = 7  },
    { .name = "Natural Minor",    .intervals = {0, 2, 3, 5, 7, 8, 10},                 .size = 7  },
    { .name = "Dorian",           .intervals = {0, 2, 3, 5, 7, 9, 10},                 .size = 7  },
    { .name = "Phrygian",         .intervals = {0, 1, 3, 5, 7, 8, 10},                 .size = 7  },
    { .name = "Lydian",           .intervals = {0, 2, 4, 6, 7, 9, 11},                 .size = 7  },
    { .name = "Mixolydian",       .intervals = {0, 2, 4, 5, 7, 9, 10},                 .size = 7  },
    { .name = "Minor Pentatonic", .intervals = {0, 3, 5, 7, 10},                       .size = 5  },
    { .name = "Major Pentatonic", .intervals = {0, 2, 4, 7, 9},                        .size = 5  }
};

/* Floored integer division (rounds toward negative infinity), unlike C's
 * built-in division which truncates toward zero. Needed so negative note/octave
 * offsets map to the correct octave (e.g. -1/12 should be -1, not 0). */
static int32_t quantizer_floor_div(int32_t numerator, int32_t denominator)
{
    int32_t quotient = numerator / denominator;
    int32_t remainder = numerator % denominator;
    /* If there's a remainder and the signs differ, C truncated up; correct it. */
    if (remainder != 0 && ((remainder < 0) != (denominator < 0))) {
        quotient -= 1;
    }
    return quotient;
}

uint8_t quantizer_scale_count(void)
{
    return (uint8_t)(sizeof(s_scales) / sizeof(s_scales[0]));
}

const musical_scale_t *quantizer_get_scale(uint8_t scale_index)
{
    if (scale_index >= quantizer_scale_count()) {
        return &s_scales[0];
    }
    return &s_scales[scale_index];
}

uint8_t quantizer_clamp_midi(int32_t midi_note)
{
    if (midi_note < 0) {
        return 0;
    }
    if (midi_note > 127) {
        return 127;
    }
    return (uint8_t)midi_note;
}

/* Convert a scale degree (which step of the scale, can be negative or beyond
 * one octave) into a MIDI note. The degree is split into an octave count and an
 * in-scale index; the index picks a semitone offset from the scale's interval
 * table, and the octave adds/subtracts 12 semitones each. */
uint8_t quantizer_degree_to_midi(int32_t scale_degree, uint8_t root_note,
                                 const musical_scale_t *scale)
{
    if (scale == NULL || scale->size == 0) {
        return 60;
    }

    /* floor-div/mod so negative degrees wrap into a valid [0, size) index. */
    int32_t octave = quantizer_floor_div(scale_degree, scale->size);
    int32_t scale_index = scale_degree - (octave * scale->size);
    if (scale_index < 0) {
        scale_index += scale->size;
    }

    int32_t midi_note = (int32_t)root_note + (octave * 12) + scale->intervals[scale_index];
    return quantizer_clamp_midi(midi_note);
}

/* Snap an arbitrary MIDI note to the nearest note in the given scale/key.
 * Strategy: generate every scale note across the octave the input falls in plus
 * the neighbours (to handle notes near an octave boundary), and keep the
 * closest one. Ties break toward the lower note for deterministic results. */
uint8_t quantizer_snap_midi_note(uint8_t midi_note, uint8_t root_note,
                                 const musical_scale_t *scale)
{
    if (scale == NULL || scale->size == 0) {
        return midi_note;
    }

    int32_t best_note = (int32_t)midi_note;
    int32_t best_distance = 9999;
    /* Locate which octave (relative to root) the input note sits in. */
    int32_t relative_note = (int32_t)midi_note - (int32_t)root_note;
    int32_t base_octave = quantizer_floor_div(relative_note, 12);

    /* Search the surrounding octaves so candidates just across an octave line
     * are still considered as the nearest match. */
    for (int32_t octave = base_octave - 1; octave <= base_octave + 1; octave++) {
        for (uint8_t i = 0; i < scale->size; i++) {
            int32_t candidate = (int32_t)root_note + (octave * 12) + scale->intervals[i];
            if (candidate < 0 || candidate > 127) {
                continue; /* out of MIDI range */
            }

            int32_t distance = candidate - (int32_t)midi_note;
            if (distance < 0) {
                distance = -distance; /* absolute distance in semitones */
            }

            if (distance < best_distance ||
                (distance == best_distance && candidate < best_note)) {
                best_distance = distance;
                best_note = candidate;
            }
        }
    }

    return (uint8_t)best_note;
}