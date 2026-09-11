#include "prog_gen.h"
#include <stdbool.h>
#include <string.h>

/* Seven-note parent scales used for harmonisation. Scales that cannot spell
 * seven degrees (pentatonics, whole tone, chromatic) borrow the parent that
 * contains them, so every scale yields a full set of diatonic chords. */
typedef enum {
    PARENT_MAJOR = 0,
    PARENT_NAT_MINOR,
    PARENT_DORIAN,
    PARENT_PHRYGIAN,
    PARENT_LYDIAN,
    PARENT_MIXOLYDIAN,
    PARENT_HARM_MINOR,
    PARENT_LOCRIAN,
    PARENT_COUNT
} prog_gen_parent_t;

static const uint8_t s_parent_intervals[PARENT_COUNT][7] = {
    /* MAJOR      */ { 0, 2, 4, 5, 7, 9, 11 },
    /* NAT_MINOR  */ { 0, 2, 3, 5, 7, 8, 10 },
    /* DORIAN     */ { 0, 2, 3, 5, 7, 9, 10 },
    /* PHRYGIAN   */ { 0, 1, 3, 5, 7, 8, 10 },
    /* LYDIAN     */ { 0, 2, 4, 6, 7, 9, 11 },
    /* MIXOLYDIAN */ { 0, 2, 4, 5, 7, 9, 10 },
    /* HARM_MINOR */ { 0, 2, 3, 5, 7, 8, 11 },
    /* LOCRIAN    */ { 0, 1, 3, 5, 6, 8, 10 },
};

/* Quantizer scale index -> harmonisation parent. Indexed positionally: the
 * quantizer table is index-stable and append-only (scale_index is persisted),
 * so the mapping is by index, not by name lookup. */
static const uint8_t s_scale_parent[PROG_GEN_SCALE_COUNT] = {
    PARENT_MAJOR,       /*  0 Chromatic   */
    PARENT_MAJOR,       /*  1 Major       */
    PARENT_NAT_MINOR,   /*  2 Nat Minor   */
    PARENT_DORIAN,      /*  3 Dorian      */
    PARENT_PHRYGIAN,    /*  4 Phrygian    */
    PARENT_LYDIAN,      /*  5 Lydian      */
    PARENT_MIXOLYDIAN,  /*  6 Mixolydian  */
    PARENT_NAT_MINOR,   /*  7 Min Pent    */
    PARENT_MAJOR,       /*  8 Maj Pent    */
    PARENT_HARM_MINOR,  /*  9 Harm Minor  */
    PARENT_LOCRIAN,     /* 10 Locrian     */
    PARENT_MAJOR,       /* 11 Whole Tone  */
};

static const uint8_t s_scale_family[PROG_GEN_SCALE_COUNT] = {
    PROG_GEN_FAMILY_MAJOR,  /*  0 Chromatic  */
    PROG_GEN_FAMILY_MAJOR,  /*  1 Major      */
    PROG_GEN_FAMILY_MINOR,  /*  2 Nat Minor  */
    PROG_GEN_FAMILY_MINOR,  /*  3 Dorian     */
    PROG_GEN_FAMILY_MINOR,  /*  4 Phrygian   */
    PROG_GEN_FAMILY_MAJOR,  /*  5 Lydian     */
    PROG_GEN_FAMILY_MAJOR,  /*  6 Mixolydian */
    PROG_GEN_FAMILY_MINOR,  /*  7 Min Pent   */
    PROG_GEN_FAMILY_MAJOR,  /*  8 Maj Pent   */
    PROG_GEN_FAMILY_MINOR,  /*  9 Harm Minor */
    PROG_GEN_FAMILY_MINOR,  /* 10 Locrian    */
    PROG_GEN_FAMILY_MAJOR,  /* 11 Whole Tone */
};

/* A step byte carries the degree in bits 0..2; the upper bits are reserved for
 * a later per-step duration unit and must be written as 0 and masked on read. */
#define STEP_DEGREE_MASK 0x07u

typedef struct {
    const char name[7];
    uint8_t    count;
    uint8_t    steps[PROG_GEN_MAX_ENTRIES];
} prog_gen_style_t;

static const prog_gen_style_t s_styles_major[] = {
    { "Axis",   4, { 0, 4, 5, 3 } },
    { "DooWop", 4, { 0, 5, 3, 4 } },
    { "Puff",   4, { 0, 2, 3, 4 } },
    { "Hopsc",  4, { 3, 4, 5, 0 } },
    { "Canon",  8, { 0, 4, 5, 2, 3, 0, 3, 4 } },
    { "Plagal", 4, { 0, 3, 5, 4 } },
};

static const prog_gen_style_t s_styles_minor[] = {
    { "AxisM",  4, { 0, 5, 2, 6 } },
    { "Shuttl", 4, { 0, 6, 5, 6 } },
    { "AeoCad", 4, { 5, 6, 0, 0 } },
    { "PuffM",  4, { 0, 2, 3, 4 } },
    { "Andalu", 4, { 0, 6, 5, 4 } },
    { "Climb",  4, { 0, 3, 4, 0 } },
    { "Dorian", 4, { 0, 3, 0, 3 } },
};

/* Transition weights, row = from degree, col = to degree. The diagonal is 0:
 * a mutation always moves somewhere else. A zero cell means the move is not
 * offered at all, which is what keeps the walk inside the idiom. */
static const uint8_t s_weights[2][7][7] = {
    { /* major */
        /* I   */ { 0, 3, 1, 6, 7, 4, 1 },
        /* ii  */ { 2, 0, 0, 1, 8, 1, 2 },
        /* iii */ { 1, 2, 0, 4, 1, 6, 0 },
        /* IV  */ { 5, 3, 0, 0, 7, 1, 1 },
        /* V   */ { 9, 0, 1, 2, 0, 4, 0 },
        /* vi  */ { 2, 4, 1, 5, 3, 0, 0 },
        /* vii */ { 7, 0, 2, 0, 1, 1, 0 },
    },
    { /* minor */
        /* i   */ { 0, 1, 3, 5, 3, 5, 5 },
        /* ii  */ { 2, 0, 0, 1, 6, 0, 2 },
        /* III */ { 2, 1, 0, 3, 0, 5, 4 },
        /* iv  */ { 5, 0, 1, 0, 5, 2, 3 },
        /* v   */ { 8, 0, 1, 1, 0, 3, 0 },
        /* VI  */ { 2, 2, 3, 3, 1, 0, 5 },
        /* VII */ { 6, 0, 4, 1, 0, 3, 0 },
    },
};

/* Harmonic function of each degree, identical in both families. The last step
 * of a progression may only mutate within its own class, so the cadence keeps
 * its shape however much the middle wanders. */
typedef enum {
    FUNC_TONIC = 0,
    FUNC_PREDOMINANT,
    FUNC_DOMINANT
} prog_gen_func_t;

static const uint8_t s_degree_func[7] = {
    FUNC_TONIC,        /* 0 */
    FUNC_PREDOMINANT,  /* 1 */
    FUNC_TONIC,        /* 2 */
    FUNC_PREDOMINANT,  /* 3 */
    FUNC_DOMINANT,     /* 4 */
    FUNC_TONIC,        /* 5 */
    FUNC_DOMINANT,     /* 6 */
};

static const prog_gen_style_t *prog_gen_style_bank(prog_gen_family_t family, uint8_t *count_out)
{
    if (family == PROG_GEN_FAMILY_MINOR) {
        *count_out = (uint8_t)(sizeof(s_styles_minor) / sizeof(s_styles_minor[0]));
        return s_styles_minor;
    }
    *count_out = (uint8_t)(sizeof(s_styles_major) / sizeof(s_styles_major[0]));
    return s_styles_major;
}

prog_gen_family_t prog_gen_family_for_scale(uint8_t scale_index)
{
    if (scale_index >= PROG_GEN_SCALE_COUNT) return PROG_GEN_FAMILY_MAJOR;
    return (prog_gen_family_t)s_scale_family[scale_index];
}

uint8_t prog_gen_style_count(prog_gen_family_t family)
{
    uint8_t count = 0;
    (void)prog_gen_style_bank(family, &count);
    return count;
}

const char *prog_gen_style_name(prog_gen_family_t family, uint8_t style)
{
    if (family != PROG_GEN_FAMILY_MAJOR && family != PROG_GEN_FAMILY_MINOR) return "?";
    uint8_t count = 0;
    const prog_gen_style_t *bank = prog_gen_style_bank(family, &count);
    if (style >= count) return "?";
    return bank[style].name;
}

chord_type_t prog_gen_chord_for_degree(uint8_t scale_index, uint8_t degree,
                                       prog_gen_ext_t ext, uint8_t *rel_root_out)
{
    if (scale_index >= PROG_GEN_SCALE_COUNT) scale_index = 1; /* Major */
    if ((unsigned)ext >= PROG_GEN_EXT_COUNT) ext = PROG_GEN_EXT_TRIAD;

    const uint8_t *iv = s_parent_intervals[s_scale_parent[scale_index]];
    uint8_t d = (uint8_t)(degree % 7);

    /* Stacked thirds within the parent scale; the ninth is the degree above
     * the root, an octave up, so it is natural only when it lands on 2. */
    uint8_t t3 = (uint8_t)((iv[(d + 2) % 7] - iv[d] + 12) % 12);
    uint8_t t5 = (uint8_t)((iv[(d + 4) % 7] - iv[d] + 12) % 12);
    uint8_t t7 = (uint8_t)((iv[(d + 6) % 7] - iv[d] + 12) % 12);
    uint8_t t9 = (uint8_t)((iv[(d + 1) % 7] - iv[d] + 12) % 12);

    chord_type_t triad;
    if (t3 == 4 && t5 == 7)      triad = CHORD_MAJ;
    else if (t3 == 3 && t5 == 7) triad = CHORD_MIN;
    else if (t3 == 3 && t5 == 6) triad = CHORD_DIM;
    else if (t3 == 4 && t5 == 8) triad = CHORD_AUG;
    else                         triad = CHORD_MAJ;

    chord_type_t out = triad;
    if (ext >= PROG_GEN_EXT_7TH) {
        /* No m7b5, dim7 or mMaj7 type exists, so those degrees stay triads. */
        if (triad == CHORD_MAJ && t7 == 11)      out = CHORD_MAJ7;
        else if (triad == CHORD_MAJ && t7 == 10) out = CHORD_DOM7;
        else if (triad == CHORD_MIN && t7 == 10) out = CHORD_MIN7;
    }
    if (ext == PROG_GEN_EXT_9TH && t9 == 2) {
        if (out == CHORD_MAJ7)      out = CHORD_MAJ9;
        else if (out == CHORD_MIN7) out = CHORD_MIN9;
        else if (out == CHORD_DOM7) out = CHORD_DOM9;
    }

    if (rel_root_out != NULL) *rel_root_out = iv[d];
    return out;
}

/* xorshift32 kept on the stack: the generator is a pure function of its
 * arguments, so no draw may come from state that outlives the call. */
static uint32_t prog_gen_draw(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s >> 8;
}

uint8_t prog_gen_generate(const prog_gen_params_t *p, uint8_t root_pc,
                          uint8_t scale_index, prog_gen_entry_t out[PROG_GEN_MAX_ENTRIES])
{
    if (p == NULL || out == NULL) return 0;

    uint8_t len = p->len;
    if (len < 1) len = 1;
    if (len > PROG_GEN_MAX_ENTRIES) len = PROG_GEN_MAX_ENTRIES;

    uint8_t bars = (p->bars >= 4) ? 4 : (p->bars >= 2) ? 2 : 1;

    prog_gen_ext_t ext = ((unsigned)p->ext < PROG_GEN_EXT_COUNT)
                             ? (prog_gen_ext_t)p->ext
                             : PROG_GEN_EXT_TRIAD;

    uint8_t var = (p->var > 3) ? 3 : p->var;

    if (scale_index >= PROG_GEN_SCALE_COUNT) scale_index = 1; /* Major */
    prog_gen_family_t family = prog_gen_family_for_scale(scale_index);

    uint8_t style_count = 0;
    const prog_gen_style_t *bank = prog_gen_style_bank(family, &style_count);
    uint8_t style = (p->style >= style_count) ? (uint8_t)(style_count - 1) : p->style;
    const prog_gen_style_t *preset = &bank[style];

    uint8_t deg[PROG_GEN_MAX_ENTRIES];
    for (uint8_t i = 0; i < len; i++) {
        deg[i] = (uint8_t)(preset->steps[i % preset->count] & STEP_DEGREE_MASK);
    }

    if (var > 0) {
        uint32_t s = (uint32_t)p->seed * 2654435761u;
        s |= 1u;
        /* Step 0 is never mutated: the progression must keep its home chord. */
        for (uint8_t i = 1; i < len; i++) {
            uint32_t u = prog_gen_draw(&s) % 100u;
            if (u >= (uint32_t)(25u * var)) continue;

            bool last = (i == (uint8_t)(len - 1));
            uint32_t total = 0;
            for (uint8_t d = 0; d < 7; d++) {
                if (d == deg[i]) continue;
                if (last && s_degree_func[d] != s_degree_func[deg[i]]) continue;
                total += s_weights[family][deg[i - 1]][d];
            }
            if (total == 0) continue;

            uint32_t r = prog_gen_draw(&s) % total;
            for (uint8_t d = 0; d < 7; d++) {
                if (d == deg[i]) continue;
                if (last && s_degree_func[d] != s_degree_func[deg[i]]) continue;
                uint32_t w = s_weights[family][deg[i - 1]][d];
                if (r < w) {
                    deg[i] = d;
                    break;
                }
                r -= w;
            }
        }
    }

    for (uint8_t i = 0; i < len; i++) {
        uint8_t rel = 0;
        out[i].chord_type    = prog_gen_chord_for_degree(scale_index, deg[i], ext, &rel);
        out[i].root          = (uint8_t)((root_pc + rel) % 12);
        out[i].duration_bars = bars;
    }
    return len;
}
