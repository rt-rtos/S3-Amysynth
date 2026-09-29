#include "sdkconfig.h"
#include "synth_ui/synth_ui_internal.h"
#include "sequencer_core.h"
#include "seq_chords.h"
#include "quantizer.h"         /* quantizer_chord_intervals() - shared voicing table */
#include "seq_core_config.h"   /* SEQ_MEL_NOTE_MIN/MAX - same-component limits */
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  CHORDS PAGE (Main Menu -> "Chords")
 * ════════════════════════════════════════════════════════════════════════
 * Item model for the chord-preset editor; page state and input routing live
 * in ui_screen_menu.c (same split as the FX/Layer/Projects pages).
 *
 * Two levels inside one page: the slot list (CH1..CH8 + Back) and, after
 * clicking a slot, the edit view (Root + Type + Clear + Back). Every edit
 * commits immediately through seq_chords_set(), so the engine sweep (re-emit,
 * voice reconfig, delete fallback) rides each commit and closing the menu
 * mid-edit can never lose a voicing.
 *
 * Authoring is root + chord type, the same model the drone and progression
 * screens use; seq_chord_t still holds absolute MIDI pitches, generated from
 * root/type on every edit.
 *
 * Changes audition through the selected melodic track's actual patch; falls
 * back to the first melodic track on a drum layer, silent with none. */

typedef enum { CHORDS_MODE_LIST = 0, CHORDS_MODE_EDIT } chords_mode_t;

static chords_mode_t s_mode = CHORDS_MODE_LIST;
static uint8_t       s_slot = 0;

/* Working copy of the authoring parameters for the open slot. */
static uint8_t       s_root = 60;          /* absolute MIDI */
static chord_type_t  s_type = CHORD_MAJ;

#define CHORDS_LIST_COUNT (SEQ_CHORD_SLOTS + 1)   /* slots + Back            */
#define CHORDS_EDIT_ROOT  0
#define CHORDS_EDIT_TYPE  1
#define CHORDS_EDIT_CLEAR 2
#define CHORDS_EDIT_BACK  3
#define CHORDS_EDIT_COUNT 4

static menu_item_view_t s_items[CHORDS_LIST_COUNT];

/* ── Chord type helpers ──────────────────────────────────────────────────
 * The picker offers only types within SEQ_CHORD_MAX_SELECT tones (the 9ths sit
 * above it - see seq_chords.h for the voice-budget rationale). The fit check
 * reads the shared interval table, so the ceiling drops the wide chords from
 * the list instead of silently storing a truncated voicing under their name. */

static uint8_t chord_type_tones(chord_type_t t)
{
    const int8_t *row = quantizer_chord_intervals(t);
    if (!row) return 0;
    uint8_t n = 0;
    while (n < 6 && row[n] >= 0) n++;
    return n;
}

static bool chord_type_fits(chord_type_t t)
{
    uint8_t n = chord_type_tones(t);
    uint8_t cap = (SEQ_CHORD_MAX_SELECT < SEQ_CHORD_MAX_NOTES)
                  ? SEQ_CHORD_MAX_SELECT : SEQ_CHORD_MAX_NOTES;
    return n >= 2 && n <= cap;
}

/* Highest interval of a type, so the root can be bounded to keep the whole
 * chord inside the melodic range instead of letting storage clamp the top
 * tones together into a collapsed voicing. */
static uint8_t chord_type_span(chord_type_t t)
{
    const int8_t *row = quantizer_chord_intervals(t);
    if (!row) return 0;
    uint8_t span = 0;
    for (uint8_t i = 0; i < 6 && row[i] >= 0; i++) span = (uint8_t)row[i];
    return span;
}

static uint8_t chord_root_max(chord_type_t t)
{
    int hi = (int)SEQ_MEL_NOTE_MAX - (int)chord_type_span(t);
    return (hi < SEQ_MEL_NOTE_MIN) ? SEQ_MEL_NOTE_MIN : (uint8_t)hi;
}

/* Walk to the next fitting type, wrapping within the fitting set. */
static chord_type_t chord_type_step(chord_type_t cur, int dir)
{
    int n = (int)cur;
    for (int guard = 0; guard < CHORD_REAL_COUNT; guard++) {
        n = (n + dir + CHORD_REAL_COUNT) % CHORD_REAL_COUNT;
        if (chord_type_fits((chord_type_t)n)) return (chord_type_t)n;
    }
    return cur;
}

/* Generate the stored voicing for root + type. */
static void chord_build(uint8_t root, chord_type_t type, seq_chord_t *out)
{
    memset(out, 0, sizeof(*out));
    const int8_t *row = quantizer_chord_intervals(type);
    if (!row) return;
    for (uint8_t i = 0; i < SEQ_CHORD_MAX_NOTES && row[i] >= 0; i++) {
        int n = (int)root + (int)row[i];
        if (n < SEQ_MEL_NOTE_MIN) n = SEQ_MEL_NOTE_MIN;
        if (n > SEQ_MEL_NOTE_MAX) n = SEQ_MEL_NOTE_MAX;
        out->notes[out->count++] = (uint8_t)n;
    }
}

/* Recover root + type from a stored voicing, so reopening a slot shows what was
 * authored. Exact match against the selectable types only: a voicing wider than
 * the picker's ceiling (e.g. a 9th saved while 5 tones were admitted) misses on
 * purpose and gets rewritten to a fitting chord on its first edit. */
static bool chord_match(const seq_chord_t *c, uint8_t *root, chord_type_t *type)
{
    if (!c || c->count < 2) return false;
    for (uint8_t t = 0; t < CHORD_REAL_COUNT; t++) {
        if (!chord_type_fits((chord_type_t)t)) continue;
        const int8_t *row = quantizer_chord_intervals((chord_type_t)t);
        if (chord_type_tones((chord_type_t)t) != c->count) continue;
        bool ok = true;
        for (uint8_t i = 0; i < c->count; i++) {
            if ((int)c->notes[i] - (int)c->notes[0] != (int)row[i]) { ok = false; break; }
        }
        if (ok) { *root = c->notes[0]; *type = (chord_type_t)t; return true; }
    }
    return false;
}

/* Audition target: the selected track when its layer is melodic, else the
 * first melodic layer's first track. Returns false with no melodic layer. */
static bool chords_audition_target(uint8_t *li, uint8_t *track)
{
    uint8_t a = seq_state.active_layer_idx;
    if (a < seq_state.num_layers &&
        seq_state.layers[a].type == SEQ_LAYER_MELODIC) {
        *li = a;
        *track = seq_state.selected_track;
        return true;
    }
    for (uint8_t i = 0; i < seq_state.num_layers; i++) {
        if (seq_state.layers[i].type == SEQ_LAYER_MELODIC) {
            *li = i;
            *track = 0;
            return true;
        }
    }
    return false;
}

static void chords_audition(const seq_chord_t *c)
{
    uint8_t li, track;
    if (!chords_audition_target(&li, &track)) return;
    sequencer_core_audition_chord(li, track, c);
}

/* Regenerate, commit and audition after any root/type change. */
static void chords_commit(void)
{
    seq_chord_t c;
    chord_build(s_root, s_type, &c);
    seq_chords_set(s_slot, &c);
    chords_audition(&c);
}

const char *chords_menu_title(void)
{
    static char s_title[16];
    if (s_mode == CHORDS_MODE_EDIT) {
        snprintf(s_title, sizeof(s_title), "CHORD CH%u", (unsigned)(s_slot + 1u));
        return s_title;
    }
    return "CHORDS";
}

const menu_item_view_t *chords_menu_build_items(void)
{
    char nb[4];
    if (s_mode == CHORDS_MODE_LIST) {
        for (uint8_t i = 0; i < SEQ_CHORD_SLOTS; i++) {
            snprintf(s_items[i].label, MENU_LABEL_LEN, "CH%u", (unsigned)(i + 1u));
            seq_chord_t c;
            if (seq_chords_get(i, &c) && c.count > 0) {
                /* Voicings authored here always match; the tone-count summary
                 * covers slots wider than the current picker ceiling (or
                 * otherwise malformed) until they are re-authored. */
                uint8_t r; chord_type_t t;
                ui_note_name(c.notes[0], nb);
                if (chord_match(&c, &r, &t)) {
                    snprintf(s_items[i].value, MENU_VALUE_LEN, "%s%s",
                             nb, chord_type_name(t));
                } else {
                    snprintf(s_items[i].value, MENU_VALUE_LEN, "%s x%u",
                             nb, (unsigned)c.count);
                }
            } else {
                snprintf(s_items[i].value, MENU_VALUE_LEN, "--");
            }
        }
        snprintf(s_items[SEQ_CHORD_SLOTS].label, MENU_LABEL_LEN, "< Back");
        s_items[SEQ_CHORD_SLOTS].value[0] = '\0';
        return s_items;
    }

    snprintf(s_items[CHORDS_EDIT_ROOT].label, MENU_LABEL_LEN, "Root");
    ui_note_name(s_root, nb);
    snprintf(s_items[CHORDS_EDIT_ROOT].value, MENU_VALUE_LEN, "%s", nb);

    snprintf(s_items[CHORDS_EDIT_TYPE].label, MENU_LABEL_LEN, "Type");
    snprintf(s_items[CHORDS_EDIT_TYPE].value, MENU_VALUE_LEN, "%s",
             chord_type_name(s_type));

    snprintf(s_items[CHORDS_EDIT_CLEAR].label, MENU_LABEL_LEN, "Clear");
    s_items[CHORDS_EDIT_CLEAR].value[0] = '\0';
    snprintf(s_items[CHORDS_EDIT_BACK].label, MENU_LABEL_LEN, "< Back");
    s_items[CHORDS_EDIT_BACK].value[0] = '\0';
    return s_items;
}

uint8_t chords_menu_item_count(void)
{
    return (s_mode == CHORDS_MODE_LIST) ? CHORDS_LIST_COUNT : CHORDS_EDIT_COUNT;
}

bool chords_menu_item_is_back(uint8_t idx)
{
    /* Only the LIST-level Back leaves the page; the edit view's Back returns
     * to the list inside handle_click. */
    return s_mode == CHORDS_MODE_LIST && idx == SEQ_CHORD_SLOTS;
}

/* Returns the new menu_editing state (mirrors projects_menu_handle_click). */
bool chords_menu_handle_click(uint8_t idx)
{
    if (s_mode == CHORDS_MODE_LIST) {
        if (idx < SEQ_CHORD_SLOTS) {
            s_slot = idx;
            /* Zeroed up front: the audition below reads c.count on every path,
             * including the one where the getter never filled it. */
            seq_chord_t c = { 0 };
            if (seq_chords_get(s_slot, &c) && c.count > 0) {
                /* Generated voicings always match; a miss can only come from a
                 * malformed slot, so fall back to its lowest tone as root. */
                if (!chord_match(&c, &s_root, &s_type)) s_root = c.notes[0];
            } else {
                /* Fresh slot: a middle-register major, audible immediately. */
                s_root = 60;
                s_type = CHORD_MAJ;
            }
            if (!chord_type_fits(s_type)) s_type = chord_type_step(s_type, 1);
            s_mode = CHORDS_MODE_EDIT;
            seq_state.menu_cursor = 0;
            if (c.count > 0) chords_audition(&c);   /* hear it on open */
        }
        return false;
    }

    if (idx == CHORDS_EDIT_ROOT || idx == CHORDS_EDIT_TYPE) {
        return !seq_state.menu_editing;   /* toggle value editing */
    }
    if (idx == CHORDS_EDIT_CLEAR) {
        seq_chords_clear(s_slot);
        return false;
    }
    /* Back: voicing is already committed per edit; just pop to the list. */
    s_mode = CHORDS_MODE_LIST;
    seq_state.menu_cursor = s_slot;
    return false;
}

void chords_menu_edit_value(uint8_t idx, int delta)
{
    if (s_mode != CHORDS_MODE_EDIT || delta == 0) return;

    int dir = (delta > 0) ? 1 : -1;
    int steps = (delta > 0) ? delta : -delta;

    if (idx == CHORDS_EDIT_ROOT) {
        int n = (int)s_root + dir * steps;
        int hi = (int)chord_root_max(s_type);
        if (n < SEQ_MEL_NOTE_MIN) n = SEQ_MEL_NOTE_MIN;
        if (n > hi)               n = hi;
        s_root = (uint8_t)n;
    } else if (idx == CHORDS_EDIT_TYPE) {
        for (int k = 0; k < steps; k++) s_type = chord_type_step(s_type, dir);
        /* A wider chord can push the top tone out of range - pull the root
         * down rather than let the voicing collapse against the ceiling. */
        uint8_t hi = chord_root_max(s_type);
        if (s_root > hi) s_root = hi;
    } else {
        return;
    }

    chords_commit();
}

void chords_menu_reset(void)
{
    s_mode = CHORDS_MODE_LIST;
}
