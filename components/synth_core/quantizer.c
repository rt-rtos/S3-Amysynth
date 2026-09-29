#include "quantizer.h"
#include <stddef.h>

static const musical_scale_t s_scales[] = {
    { .name = "Chromatic",        .intervals = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, .size = 12 },
    { .name = "Major (Ionian)",   .intervals = {0, 2, 4, 5, 7, 9, 11},                 .size = 7  },
    { .name = "Natural Minor",    .intervals = {0, 2, 3, 5, 7, 8, 10},                 .size = 7  },
    { .name = "Dorian",           .intervals = {0, 2, 3, 5, 7, 9, 10},                 .size = 7  },
    { .name = "Phrygian",         .intervals = {0, 1, 3, 5, 7, 8, 10},                 .size = 7  },
    { .name = "Lydian",           .intervals = {0, 2, 4, 6, 7, 9, 11},                 .size = 7  },
    { .name = "Mixolydian",       .intervals = {0, 2, 4, 5, 7, 9, 10},                 .size = 7  },
    { .name = "Minor Pentatonic", .intervals = {0, 3, 5, 7, 10},                       .size = 5  },
    { .name = "Major Pentatonic", .intervals = {0, 2, 4, 7, 9},                        .size = 5  },
    /* Append-only past this point: scale_index is persisted in project
     * snapshots, so reordering the table would silently re-key saved scales. */
    { .name = "Harmonic Minor",   .intervals = {0, 2, 3, 5, 7, 8, 11},                 .size = 7  },
    { .name = "Locrian",          .intervals = {0, 1, 3, 5, 6, 8, 10},                 .size = 7  },
    { .name = "Whole Tone",       .intervals = {0, 2, 4, 6, 8, 10},                    .size = 6  }
};

/* Floored division (toward negative infinity) rather than C's truncation, so
 * negative note offsets map to the correct octave (-1/12 = -1, not 0). */
static int32_t quantizer_floor_div(int32_t numerator, int32_t denominator)
{
    int32_t quotient = numerator / denominator;
    int32_t remainder = numerator % denominator;
    /* Remainder with differing signs means C truncated up; correct it. */
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

/* Snap a MIDI note to the nearest note in the given scale/key: generate every
 * scale note across the input's octave plus its neighbours (octave-boundary
 * cases) and keep the closest. Ties break low, for determinism. */
uint8_t quantizer_snap_midi_note(uint8_t midi_note, uint8_t root_note,
                                 const musical_scale_t *scale)
{
    if (scale == NULL || scale->size == 0) {
        return midi_note;
    }

    int32_t best_note = (int32_t)midi_note;
    int32_t best_distance = 9999;
    int32_t relative_note = (int32_t)midi_note - (int32_t)root_note;
    int32_t base_octave = quantizer_floor_div(relative_note, 12);

    /* Neighbouring octaves too, so candidates just across an octave line are
     * still considered. */
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

/* Semitone intervals (from root) for each chord type.
 * Terminated by -1. Max 5 notes per chord. */
static const int8_t s_chord_intervals[CHORD_TYPE_COUNT][6] = {
    /* MAJ  */ {  0,  4,  7, -1, -1, -1 },
    /* MIN  */ {  0,  3,  7, -1, -1, -1 },
    /* MAJ7 */ {  0,  4,  7, 11, -1, -1 },
    /* MIN7 */ {  0,  3,  7, 10, -1, -1 },
    /* DOM7 */ {  0,  4,  7, 10, -1, -1 },
    /* SUS2 */ {  0,  2,  7, -1, -1, -1 },
    /* SUS4 */ {  0,  5,  7, -1, -1, -1 },
    /* DIM  */ {  0,  3,  6, -1, -1, -1 },
    /* AUG  */ {  0,  4,  8, -1, -1, -1 },
    /* MIN9 */ {  0,  3,  7, 10, 14, -1 },  // min7 + 9th
    /* MAJ9 */ {  0,  4,  7, 11, 14, -1 },  // maj7 + 9th
    /* MAJ6 */ {  0,  4,  7,  9, -1, -1 },
    /* MIN6 */ {  0,  3,  7,  9, -1, -1 },
    /* DOM9 */ {  0,  4,  7, 10, 14, -1 },  // dom7 + 9th
    /* OFF  */ {  0, -1, -1, -1, -1, -1 },  // root only - "no chord"
};

/* The -1-terminated semitone interval row for a chord type; NULL if out of
 * range. Shared so other modules (e.g. the drone) voice from one table. */
const int8_t *quantizer_chord_intervals(chord_type_t chord_type)
{
    if ((unsigned)chord_type >= CHORD_TYPE_COUNT) return NULL;
    return s_chord_intervals[chord_type];
}

uint8_t quantizer_snap_to_chord(uint8_t midi_note, uint8_t root,
                                chord_type_t chord_type)
{
    if ((unsigned)chord_type >= CHORD_TYPE_COUNT) return midi_note;
    const int8_t *intervals = s_chord_intervals[chord_type];
    uint8_t root_pc = root % 12;

    int32_t best_note     = (int32_t)midi_note;
    int32_t best_distance = 9999;

    /* Search chord tones across all reachable MIDI octaves. */
    for (int32_t oct = -1; oct <= 10; oct++) {
        for (int i = 0; i < 6; i++) {
            if (intervals[i] < 0) break;
            int32_t candidate = oct * 12 + (int32_t)root_pc + intervals[i];
            if (candidate < 0 || candidate > 127) continue;
            int32_t dist = candidate - (int32_t)midi_note;
            if (dist < 0) dist = -dist;
            if (dist < best_distance ||
                (dist == best_distance && candidate < best_note)) {
                best_distance = dist;
                best_note     = candidate;
            }
        }
    }
    return (uint8_t)best_note;
}

/* Tone slots per chord-table row, terminator included. */
#define QUANTIZER_CHORD_ROW ((int)sizeof(s_chord_intervals[0]))

/* Missing and doubling penalties for one chord tone, by its interval from the
 * root. The first matching role wins; sus = the chord has neither 3 nor 4, so
 * its 2 or 5 stands in for the third. */
static void voice_tone_penalties(int8_t interval, bool sus,
                                 int32_t *missing, int32_t *doubling)
{
    if (interval == 0) {                                   /* root */
        *missing = 32; *doubling = 0;
    } else if (interval == 3 || interval == 4 ||
               (sus && (interval == 2 || interval == 5))) { /* third / sus */
        *missing = 40; *doubling = 6;
    } else if (interval == 7) {                            /* perfect fifth */
        *missing = 12; *doubling = 1;
    } else if (interval == 6 || interval == 8) {           /* altered fifth */
        *missing = 30; *doubling = 6;
    } else if (interval >= 9 && interval <= 11) {          /* sixth / seventh */
        *missing = 36; *doubling = 6;
    } else {                                               /* extension (9th) */
        *missing = 24; *doubling = 6;
    }
}

/* Exhaustive search over every row -> tone assignment (at most 5^5). Each row's
 * candidate for a tone is that pitch class's instance nearest the row's ref
 * (ties low), so the search only picks WHICH tone each row takes. Cost = total
 * distance moved, plus the per-role missing/doubling penalties, plus 8 per
 * extra row on one MIDI note (a unison double adds nothing audible). */
void quantizer_voice_chord(const uint8_t *refs, uint8_t n, uint8_t root,
                           chord_type_t chord_type, uint8_t *out)
{
    if (refs == NULL || out == NULL || n == 0) return;

    const int8_t *intervals = quantizer_chord_intervals(chord_type);
    int m = 0;
    if (intervals != NULL) {
        while (m < QUANTIZER_CHORD_ROW && intervals[m] >= 0) m++;
    }
    if (m == 0 || n > QUANTIZER_VOICE_MAX) {
        for (uint8_t r = 0; r < n; r++) {
            out[r] = quantizer_snap_to_chord(refs[r], root, chord_type);
        }
        return;
    }

    bool sus = true;
    for (int t = 0; t < m; t++) {
        if (intervals[t] == 3 || intervals[t] == 4) sus = false;
    }
    int32_t missing[QUANTIZER_CHORD_ROW];
    int32_t doubling[QUANTIZER_CHORD_ROW];
    for (int t = 0; t < m; t++) {
        voice_tone_penalties(intervals[t], sus, &missing[t], &doubling[t]);
    }

    /* Candidate note and its distance for every (row, tone). */
    int32_t root_pc = (int32_t)(root % 12);
    uint8_t cand[QUANTIZER_VOICE_MAX][QUANTIZER_CHORD_ROW];
    int32_t dist[QUANTIZER_VOICE_MAX][QUANTIZER_CHORD_ROW];
    for (uint8_t r = 0; r < n; r++) {
        int32_t ref = (int32_t)(refs[r] > 127 ? 127 : refs[r]);
        for (int t = 0; t < m; t++) {
            int32_t pc = (root_pc + intervals[t]) % 12;
            int32_t below = (ref - pc + 120) % 12;     /* ref - lower instance */
            int32_t lo = ref - below;
            int32_t hi = lo + 12;
            int32_t note = (below <= 12 - below) ? lo : hi;
            if (note < 0) note = hi;
            if (note > 127) note = lo;
            cand[r][t] = (uint8_t)note;
            dist[r][t] = (note > ref) ? note - ref : ref - note;
        }
    }

    uint8_t idx[QUANTIZER_VOICE_MAX] = {0};
    uint8_t best_idx[QUANTIZER_VOICE_MAX] = {0};
    int32_t best_cost = INT32_MAX;
    for (;;) {
        int32_t cost = 0;
        uint8_t taken[QUANTIZER_CHORD_ROW] = {0};
        for (uint8_t r = 0; r < n; r++) {
            cost += dist[r][idx[r]];
            taken[idx[r]]++;
            for (uint8_t q = 0; q < r; q++) {
                if (cand[q][idx[q]] == cand[r][idx[r]]) {
                    cost += 8;          /* one more row on an already-taken note */
                    break;
                }
            }
        }
        for (int t = 0; t < m; t++) {
            if (taken[t] == 0) cost += missing[t];
            else               cost += (int32_t)(taken[t] - 1) * doubling[t];
        }
        if (cost < best_cost) {
            best_cost = cost;
            for (uint8_t r = 0; r < n; r++) best_idx[r] = idx[r];
        }

        /* Next assignment: the last row varies fastest, row 0 slowest. */
        int r = (int)n - 1;
        while (r >= 0 && ++idx[r] == (uint8_t)m) {
            idx[r] = 0;
            r--;
        }
        if (r < 0) break;
    }

    for (uint8_t r = 0; r < n; r++) {
        out[r] = cand[r][best_idx[r]];
    }
}
