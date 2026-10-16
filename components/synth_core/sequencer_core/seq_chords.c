#include "sequencer_core/seq_core_internal.h"
#include "seq_chords.h"

/* ── Chord preset table ────────────────────────────────────────────────────
 * Plain static state alongside s_prog / s_quantizer. All mutation happens on
 * the synth_ui task (chord editor and project load), the same single-applier
 * discipline as every other chord edit. Readers on the trig service task only
 * ever see a whole uint8_t tone or count, so a mid-edit fire at worst plays the
 * half-updated voicing for one step. */
static seq_chord_t s_chords[SEQ_CHORD_SLOTS];

bool seq_chords_get(uint8_t idx, seq_chord_t *out)
{
    if (idx >= SEQ_CHORD_SLOTS || out == NULL) return false;
    *out = s_chords[idx];
    return true;
}

/* Normalize a raw voicing: keep non-zero tones clamped to the melodic range,
 * sort ascending, zero-fill the tail; count derives from what survived. */
static seq_chord_t chord_normalize(const seq_chord_t *chord)
{
    seq_chord_t c = { .notes = {0}, .count = 0 };
    for (uint8_t i = 0; i < SEQ_CHORD_MAX_NOTES; i++) {
        if (chord->notes[i] == 0) continue;
        c.notes[c.count++] = sequencer_core_clamp_melodic_note(chord->notes[i]);
    }
    for (uint8_t i = 1; i < c.count; i++) {         /* insertion sort, n<=4 */
        uint8_t v = c.notes[i];
        int8_t  j = (int8_t)(i - 1);
        while (j >= 0 && c.notes[j] > v) { c.notes[j + 1] = c.notes[j]; j--; }
        c.notes[j + 1] = v;
    }
    return c;
}

void seq_chords_set(uint8_t idx, const seq_chord_t *chord)
{
    if (idx >= SEQ_CHORD_SLOTS || chord == NULL) return;

    s_chords[idx] = chord_normalize(chord);
    ESP_LOGI(TAG, "chord CH%u -> %u tone(s)", idx + 1u, s_chords[idx].count);

    /* The single owner of the referencing-track sweep: re-emit, reconfigure
     * voices, and fall back to the last plain note for undefined slots. */
    sequencer_core_chord_slot_changed(idx);
}

void seq_chords_import(const seq_chord_t table[SEQ_CHORD_SLOTS])
{
    if (table == NULL) {
        seq_chords_reset_all();
        return;
    }
    for (uint8_t i = 0; i < SEQ_CHORD_SLOTS; i++) {
        s_chords[i] = chord_normalize(&table[i]);
    }
}

void seq_chords_clear(uint8_t idx)
{
    if (idx >= SEQ_CHORD_SLOTS) return;
    memset(&s_chords[idx], 0, sizeof(s_chords[idx]));
    sequencer_core_chord_slot_changed(idx);
}

uint8_t seq_chords_count(uint8_t idx)
{
    if (idx >= SEQ_CHORD_SLOTS) return 0;
    return s_chords[idx].count;
}

bool seq_chords_is_defined(uint8_t idx)
{
    return seq_chords_count(idx) > 0;
}

uint8_t seq_chords_defined_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < SEQ_CHORD_SLOTS; i++) {
        if (s_chords[i].count > 0) n++;
    }
    return n;
}

void seq_chords_params_defaults(seq_chord_t out[SEQ_CHORD_SLOTS])
{
    memset(out, 0, SEQ_CHORD_SLOTS * sizeof(out[0]));
}

void seq_chords_reset_all(void)
{
    seq_chords_params_defaults(s_chords);
}

uint8_t seq_chords_resolve(uint8_t idx, int transpose,
                           uint8_t out[SEQ_CHORD_MAX_NOTES])
{
    if (idx >= SEQ_CHORD_SLOTS) return 0;
    const seq_chord_t *c = &s_chords[idx];
    for (uint8_t i = 0; i < c->count; i++) {
        /* Clamp, never drop: extreme progressions compress the voicing at the
         * range edges instead of thinning it. */
        out[i] = sequencer_core_clamp_melodic_note((int32_t)c->notes[i] + transpose);
    }
    return c->count;
}

uint8_t seq_chords_selector_step(uint8_t cur, int dir)
{
    int step = (dir >= 0) ? 1 : -1;

    if (SEQ_NOTE_IS_CHORD(cur)) {
        /* Walk the chord zone, skipping undefined slots; off either end we
         * re-enter the plain range at the matching edge. */
        int i = (int)SEQ_CHORD_INDEX(cur);
        for (i += step; i >= 0 && i < SEQ_CHORD_SLOTS; i += step) {
            if (seq_chords_is_defined((uint8_t)i)) return SEQ_CHORD_NOTE(i);
        }
        return (step > 0) ? SEQ_MEL_NOTE_MIN : SEQ_MEL_NOTE_MAX;
    }

    /* Plain range. Normalize a stray out-of-range value first. */
    if (cur < SEQ_MEL_NOTE_MIN) cur = SEQ_MEL_NOTE_MIN;
    if (cur > SEQ_MEL_NOTE_MAX) cur = SEQ_MEL_NOTE_MAX;

    int next = (int)cur + step;
    if (next >= SEQ_MEL_NOTE_MIN && next <= SEQ_MEL_NOTE_MAX) return (uint8_t)next;

    /* Off the top: first defined chord, else wrap to the bottom.
     * Off the bottom: last defined chord, else wrap to the top. */
    if (step > 0) {
        for (uint8_t i = 0; i < SEQ_CHORD_SLOTS; i++) {
            if (seq_chords_is_defined(i)) return SEQ_CHORD_NOTE(i);
        }
        return SEQ_MEL_NOTE_MIN;
    }
    for (int i = SEQ_CHORD_SLOTS - 1; i >= 0; i--) {
        if (seq_chords_is_defined((uint8_t)i)) return SEQ_CHORD_NOTE(i);
    }
    return SEQ_MEL_NOTE_MAX;
}
