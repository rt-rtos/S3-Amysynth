#pragma once

#include <stdint.h>
#include "chord_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* == Chord-progression generator ==========================================
 * Pure function from (style preset, key, scale, variation seed) to a list of
 * diatonic chords. It owns its own parent-scale table rather than reading the
 * quantizer's: harmonisation needs seven degrees, so the pentatonic, whole
 * tone and chromatic scales are mapped onto a seven-note parent here.
 *
 * Obligations: p and out are non-NULL; out has room for PROG_GEN_MAX_ENTRIES
 * entries. root_pc is taken mod 12, so any value is legal. A scale_index at or
 * above PROG_GEN_SCALE_COUNT is treated as Major. Every params field is
 * clamped as documented on the struct - out-of-range values are legal input.
 *
 * Guarantees: the output is a deterministic function of the inputs alone.
 * Every chord_type is a real chord type, never CHORD_OFF; every root is
 * diatonic to the parent scale transposed to root_pc; every duration_bars is
 * 1, 2 or 4; the returned count is 1..PROG_GEN_MAX_ENTRIES and exactly that
 * many entries are written.
 *
 * Execution context: pure and reentrant - no static state, no locks, no
 * allocation. Callable from any task; not meant for the render path, but safe
 * there. */

#define PROG_GEN_MAX_ENTRIES 8      /* equals CHORD_PROG_MAX_ENTRIES */
#define PROG_GEN_SCALE_COUNT 12     /* quantizer s_scales[] size, index-stable by contract */

typedef enum { PROG_GEN_FAMILY_MAJOR = 0, PROG_GEN_FAMILY_MINOR = 1 } prog_gen_family_t;

typedef enum { PROG_GEN_EXT_TRIAD = 0, PROG_GEN_EXT_7TH, PROG_GEN_EXT_9TH, PROG_GEN_EXT_COUNT } prog_gen_ext_t;

typedef struct {
    uint8_t  style;  /* index within the family's preset list; clamped */
    uint8_t  len;    /* entries wanted, clamped to 1..PROG_GEN_MAX_ENTRIES */
    uint8_t  bars;   /* bars per chord: >=4 -> 4, >=2 -> 2, else 1 */
    uint8_t  ext;    /* prog_gen_ext_t; out of range -> TRIAD */
    uint8_t  var;    /* 0..3 mutation level; >3 -> 3 */
    uint16_t seed;   /* any value; same inputs -> same output */
} prog_gen_params_t;

typedef struct {
    uint8_t      root;           /* pitch class 0..11 */
    chord_type_t chord_type;     /* never CHORD_OFF */
    uint8_t      duration_bars;  /* 1, 2 or 4 */
} prog_gen_entry_t;

/* Which preset bank a scale harmonises into. Out-of-range index -> MAJOR. */
prog_gen_family_t prog_gen_family_for_scale(uint8_t scale_index);

/* Number of style presets in a family; at least 1. */
uint8_t prog_gen_style_count(prog_gen_family_t family);

/* Short display name for a style, at most 6 characters. "?" when family or
 * style is out of range. The returned pointer is static storage. */
const char *prog_gen_style_name(prog_gen_family_t family, uint8_t style);

/* Chord built on scale degree 0..6 of the harmonisation parent of
 * scale_index. *rel_root_out (may be NULL) receives the degree's semitone
 * offset from the tonic. Degrees above 6 wrap; the result is never CHORD_OFF.
 * An extension is only taken when the parent scale actually spells it - a
 * degree whose seventh or ninth has no matching chord type falls back to the
 * plainer chord rather than inventing one. */
chord_type_t prog_gen_chord_for_degree(uint8_t scale_index, uint8_t degree,
                                       prog_gen_ext_t ext, uint8_t *rel_root_out);

/* Fills out[0..n-1] and returns n (1..PROG_GEN_MAX_ENTRIES). */
uint8_t prog_gen_generate(const prog_gen_params_t *p, uint8_t root_pc,
                          uint8_t scale_index, prog_gen_entry_t out[PROG_GEN_MAX_ENTRIES]);

#ifdef __cplusplus
}
#endif
