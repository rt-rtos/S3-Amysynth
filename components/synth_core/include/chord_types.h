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
     * fall out of the existing table. CHORD_OFF must stay last (see
     * CHORD_REAL_COUNT), so new real types insert above and renumber it -
     * a snapshot that stored OFF under the old numbering decodes to a real
     * chord, which the load-time clamp accepts (wrong chord, no crash). */
    CHORD_OFF,
    CHORD_TYPE_COUNT
} chord_type_t;

/* Number of real (multi-note) chord types, i.e. every member before CHORD_OFF -
 * which, since OFF is last, is CHORD_OFF's own value. Pickers where "root only"
 * has no meaning bound themselves with this instead of CHORD_TYPE_COUNT, so the
 * OFF option stays where it was asked for (the drones) rather than appearing in
 * every chord list. Keep CHORD_OFF last for this to hold. */
#define CHORD_REAL_COUNT  ((uint8_t)CHORD_OFF)

/* Display names follow chord-chart convention: the dominant types and the
 * major 6th are written bare (C7, C9, C6), so a rooted label concatenates
 * naturally ("C#7"), while Maj/Min qualify everything else. */
static inline const char *chord_type_name(chord_type_t t)
{
    static const char *const s[] = {
        "Maj", "Min", "Maj7", "Min7", "7", "Sus2", "Sus4", "Dim", "Aug",
        "Min9", "Maj9", "6", "Min6", "9", "--"
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
