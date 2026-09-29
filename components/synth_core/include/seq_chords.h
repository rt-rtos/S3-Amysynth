#pragma once

/* ── Chord presets ─────────────────────────────────────────────────────────
 * A global table of user-authored chords (absolute MIDI pitches) that melodic
 * tracks can play instead of a single note. Assignment rides the per-track
 * source-note path: SEQ_CHORD_BASE..+SLOTS-1 in track_base_note / step_note[]
 * are sentinels meaning "play chord slot N", passing through
 * sequencer_resolve_track_note() untouched. Expansion to real pitches happens
 * at emit time (the decorated one-shot path in seq_core_trig.c), where the
 * progression's transpose offset is applied per fire - intervals preserved,
 * chord tones never individually re-quantized.
 *
 * The sentinel range starts at 200 so 97..199 stays free for future sentinel
 * families; step_note is uint8_t and the playable range tops out at
 * SEQ_MEL_NOTE_MAX (96). */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEQ_CHORD_SLOTS      8
/* Storage width: wide enough for the 9th chords. Authoring is capped below
 * that at SEQ_CHORD_MAX_SELECT, because a chord-carrying row widens its voice
 * count to the tone count and seq_clamp_patch_voices caps voices at
 * SEQ_TRACK_OSC_BUDGET / oscs_per_voice (seq_core_config.h), which is 4 for the
 * DX7 family: a 9th on a DX7 patch would clamp to four voices and lose its top
 * tone.
 * Storage stays 5-wide so admitting 5-tone voicings again (e.g. by spreading
 * tones across a track pair) is a picker-ceiling change, not a format change. */
#define SEQ_CHORD_MAX_NOTES  5
/* Widest chord the picker offers. Raising this past 4 re-admits the 9ths and
 * re-opens the top-tone clamp described above. */
#define SEQ_CHORD_MAX_SELECT 4
#define SEQ_CHORD_BASE       200u

#define SEQ_NOTE_IS_CHORD(n) ((uint8_t)(n) >= SEQ_CHORD_BASE && \
                              (uint8_t)(n) <  SEQ_CHORD_BASE + SEQ_CHORD_SLOTS)
#define SEQ_CHORD_INDEX(n)   ((uint8_t)((n) - SEQ_CHORD_BASE))
#define SEQ_CHORD_NOTE(idx)  ((uint8_t)(SEQ_CHORD_BASE + (idx)))

typedef struct {
    uint8_t notes[SEQ_CHORD_MAX_NOTES];  /* absolute MIDI, ascending; 0 = empty */
    uint8_t count;                       /* 0 = preset undefined */
} seq_chord_t;

/* Table accessors. set() normalizes (drops empty positions, sorts ascending,
 * clamps to the melodic range) then calls sequencer_core_chord_slot_changed()
 * so referencing tracks re-emit and, if the tone count outgrew the layer's
 * voice budget, reconfigure - callers never do that bookkeeping. */
bool    seq_chords_get(uint8_t idx, seq_chord_t *out);
void    seq_chords_set(uint8_t idx, const seq_chord_t *chord);
void    seq_chords_clear(uint8_t idx);
uint8_t seq_chords_count(uint8_t idx);
bool    seq_chords_is_defined(uint8_t idx);
uint8_t seq_chords_defined_count(void);
void    seq_chords_reset_all(void);

/* Bulk table replace for the project loader: normalizes and copies all slots
 * (NULL = clear) WITHOUT the per-slot engine sweep - the loader re-imports
 * every layer right afterwards, rebuilding voices/emits from scratch. */
void    seq_chords_import(const seq_chord_t table[SEQ_CHORD_SLOTS]);

/* Expand slot `idx` into absolute pitches with `transpose` semitones added,
 * each tone clamped (never dropped) to the melodic range. Returns the tone
 * count (0 = undefined slot: fire nothing). */
uint8_t seq_chords_resolve(uint8_t idx, int transpose,
                           uint8_t out[SEQ_CHORD_MAX_NOTES]);

/* One step through the melodic note-selector domain:
 * [SEQ_MEL_NOTE_MIN..SEQ_MEL_NOTE_MAX] ++ [defined chord sentinels], wrapping
 * at both ends (undefined slots are skipped; with no chords defined the plain
 * range simply wraps C1<->C7). `cur` may be a plain note or a sentinel. */
uint8_t seq_chords_selector_step(uint8_t cur, int dir);

#ifdef __cplusplus
}
#endif
