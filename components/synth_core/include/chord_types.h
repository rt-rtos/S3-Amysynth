#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHORD_MAJ  = 0,
    CHORD_MIN,
    CHORD_MAJ7,
    CHORD_MIN7,
    CHORD_DOM7,
    CHORD_SUS2,
    CHORD_SUS4,
    CHORD_DIM,
    CHORD_AUG,
    CHORD_MIN9,
    CHORD_MAJ9,
    CHORD_MAJ6,
    CHORD_MIN6,
    CHORD_DOM9,
    /* No chord: the root alone. It needs no special case downstream: its
     * interval row is just {0}, so note counts, voice sizing and voicing all
     * fall out of the existing table. Chord types are stored by value in
     * project files, so CHORD_OFF keeps its value (14) forever and every new
     * real type appends after it; display order lives in chord_type_order. */
    CHORD_OFF,
    CHORD_M7B5,
    CHORD_TYPE_COUNT
} chord_type_t;

/* Number of real (multi-note) chord types: every member except CHORD_OFF.
 * Not an enum bound - OFF sits mid-enum - so pickers step with
 * chord_type_next_real()/chord_type_next() rather than modulo over a count. */
#define CHORD_REAL_COUNT  ((uint8_t)(CHORD_TYPE_COUNT - 1))

/* Picker display order: the real types first, CHORD_OFF last. */
static const chord_type_t chord_type_order[CHORD_TYPE_COUNT] = {
    CHORD_MAJ, CHORD_MIN, CHORD_MAJ7, CHORD_MIN7, CHORD_DOM7, CHORD_SUS2,
    CHORD_SUS4, CHORD_DIM, CHORD_AUG, CHORD_MIN9, CHORD_MAJ9, CHORD_MAJ6,
    CHORD_MIN6, CHORD_DOM9, CHORD_M7B5, CHORD_OFF
};

/* Position of t in chord_type_order, or -1 if t is not a chord type. */
static inline int chord_type_order_index(chord_type_t t)
{
    for (int i = 0; i < CHORD_TYPE_COUNT; i++)
        if (chord_type_order[i] == t) return i;
    return -1;
}

/* The real type dir steps from t in display order, wrapping; never CHORD_OFF.
 * An OFF or out-of-range t starts from the first entry. */
static inline chord_type_t chord_type_next_real(chord_type_t t, int dir)
{
    int i = chord_type_order_index(t);
    if (i < 0 || i >= CHORD_REAL_COUNT) return chord_type_order[0];
    i = (i + dir % CHORD_REAL_COUNT + CHORD_REAL_COUNT) % CHORD_REAL_COUNT;
    return chord_type_order[i];
}

/* The type dir steps from t in display order including CHORD_OFF, wrapping.
 * An out-of-range t starts from the first entry. */
static inline chord_type_t chord_type_next(chord_type_t t, int dir)
{
    int i = chord_type_order_index(t);
    if (i < 0) return chord_type_order[0];
    i = (i + dir % CHORD_TYPE_COUNT + CHORD_TYPE_COUNT) % CHORD_TYPE_COUNT;
    return chord_type_order[i];
}

/* Display names follow chord-chart convention: the dominant types, the
 * major 6th and the half-diminished 7th are written bare (C7, C9, C6, Cm7b5),
 * so a rooted label concatenates naturally ("C#7"), while Maj/Min qualify
 * everything else. Indexed by enum value, not display order. */
static inline const char *chord_type_name(chord_type_t t)
{
    static const char *const s[] = {
        "Maj", "Min", "Maj7", "Min7", "7", "Sus2", "Sus4", "Dim", "Aug",
        "Min9", "Maj9", "6", "Min6", "9", "--", "m7b5"
    };
    return ((unsigned)t < CHORD_TYPE_COUNT) ? s[t] : "?";
}

/* Chromatic root note (0=C .. 11=B) to display name. */
static inline const char *chord_root_name(uint8_t root)
{
    static const char *const s[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return s[root % 12];
}

#ifdef __cplusplus
}
#endif
