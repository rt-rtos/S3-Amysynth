#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "seq_clamp.h"

/* ════════════════════════════════════════════════════════════════════════
 *  PROG SCREEN
 * ════════════════════════════════════════════════════════════════════════ */

static uint8_t s_prog_cursor  = 0;   /* 0=enabled toggle, 1..count=entry rows */
static bool    s_prog_editing = false;
static uint8_t s_prog_field   = 0;   /* edit sub-field: 0=root, 1=type, 2=duration */

/* Allowed per-entry durations, in bars. */
static const uint8_t s_prog_durations[] = { 1, 2, 3, 4, 8, 16 };
#define PROG_NUM_DURATIONS ((int)(sizeof(s_prog_durations) / sizeof(s_prog_durations[0])))

void prog_build_view(prog_view_t *out)
{
    uint8_t count = sequencer_core_progression_get_count();
    out->enabled       = sequencer_core_progression_get_enabled();
    out->apply_at_bar  = sequencer_core_progression_get_apply_at_bar();
    out->count         = count;
    out->current_entry = sequencer_core_progression_get_current();
    out->cursor        = s_prog_cursor;
    out->editing       = s_prog_editing;
    out->edit_field    = s_prog_field;
    out->bars_in_current = sequencer_core_progression_bars_in_current();

    uint8_t n = (count < PROG_VIEW_MAX_ENTRIES) ? count : PROG_VIEW_MAX_ENTRIES;
    for (uint8_t i = 0; i < n; i++) {
        sequencer_core_progression_get_entry(i,
            &out->entries[i].root,
            &out->entries[i].chord_type,
            &out->entries[i].duration_bars);
    }
}

uint32_t prog_view_signature(prog_view_t *out)
{
    uint32_t h = FNV1A_OFFSET;
    prog_build_view(out);
    h = fnv1a_bytes(h, &out->enabled,       sizeof(out->enabled));
    h = fnv1a_bytes(h, &out->apply_at_bar,  sizeof(out->apply_at_bar));
    h = fnv1a_bytes(h, &out->count,         sizeof(out->count));
    h = fnv1a_bytes(h, &out->current_entry, sizeof(out->current_entry));
    h = fnv1a_bytes(h, &out->cursor,        sizeof(out->cursor));
    h = fnv1a_bytes(h, &out->editing,       sizeof(out->editing));
    h = fnv1a_bytes(h, &out->edit_field,    sizeof(out->edit_field));
    h = fnv1a_bytes(h, &out->bars_in_current, sizeof(out->bars_in_current));
    for (uint8_t i = 0; i < out->count; i++) {
        h = fnv1a_bytes(h, &out->entries[i], sizeof(out->entries[i]));
    }
    return h;
}

bool synth_ui_prog_is_active(void)
{
    return seq_state.ui_mode == UI_MODE_PROG && !seq_state.menu_open;
}

/* Find the index of `dur` in s_prog_durations[], or the nearest-not-greater. */
static int prog_duration_index(uint8_t dur)
{
    int idx = 0;
    for (int i = 0; i < PROG_NUM_DURATIONS; i++) {
        if (s_prog_durations[i] <= dur) idx = i;
    }
    return idx;
}

bool synth_ui_prog_handle_encoder(int delta)
{
    if (!synth_ui_prog_is_active()) return false;
    uint8_t count = sequencer_core_progression_get_count();
    /* Cursor range: 0=enable toggle, 1..count=entries, count+1=apply mode. */
    uint8_t max_cursor = (uint8_t)(count + 1);

    if (s_prog_editing && s_prog_cursor >= 1 && s_prog_cursor <= count) {
        /* Editing the focused field of the selected entry. */
        uint8_t ei = (uint8_t)(s_prog_cursor - 1);
        uint8_t root; chord_type_t ct; uint8_t dur;
        sequencer_core_progression_get_entry(ei, &root, &ct, &dur);
        switch (s_prog_field) {
            case 0: {  /* root: chromatic 0–11 */
                int nr = ((int)root + delta) % 12;
                if (nr < 0) nr += 12;
                root = (uint8_t)nr;
                break;
            }
            case 1: {  /* chord type */
                /* Real chords only - CHORD_OFF is a drone affordance. */
                ct = chord_type_next_real(ct, delta);
                break;
            }
            default: { /* duration: cycle the allowed set */
                int di = prog_duration_index(dur) + delta;
                while (di < 0) di += PROG_NUM_DURATIONS;
                di %= PROG_NUM_DURATIONS;
                dur = s_prog_durations[di];
                break;
            }
        }
        sequencer_core_progression_set_entry(ei, root, ct, dur);
    } else {
        int nc = (int)s_prog_cursor + delta;
        if (nc < 0) nc = (int)max_cursor;
        if (nc > (int)max_cursor) nc = 0;
        s_prog_cursor = (uint8_t)nc;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_prog_handle_button(void)
{
    if (!synth_ui_prog_is_active()) return false;
    uint8_t count = sequencer_core_progression_get_count();
    if (s_prog_cursor == 0) {
        /* Toggle row: enable/disable the progression. */
        sequencer_core_progression_set_enabled(
            !sequencer_core_progression_get_enabled());
    } else if (s_prog_cursor == (uint8_t)(count + 1)) {
        /* Apply-mode row: INST <-> BAR launch quantization. */
        sequencer_core_progression_set_apply_at_bar(
            !sequencer_core_progression_get_apply_at_bar());
    } else if (!s_prog_editing) {
        /* Enter field-edit on the focused entry, starting at root. */
        s_prog_editing = true;
        s_prog_field   = 0;
    } else {
        /* Advance root → type → duration, then exit edit. */
        if (s_prog_field >= 2) {
            s_prog_editing = false;
            s_prog_field   = 0;
        } else {
            s_prog_field++;
        }
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_prog_add_entry(void)
{
    if (!synth_ui_prog_is_active()) return false;
    if (sequencer_core_progression_add_entry()) {
        /* Move the cursor to the freshly-appended row so it can be edited. */
        s_prog_cursor  = sequencer_core_progression_get_count();
        s_prog_editing = false;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_prog_delete_entry(void)
{
    if (!synth_ui_prog_is_active()) return false;
    if (s_prog_cursor >= 1 &&
        s_prog_cursor <= sequencer_core_progression_get_count()) {
        sequencer_core_progression_delete_entry((uint8_t)(s_prog_cursor - 1));
        uint8_t count = sequencer_core_progression_get_count();
        if (s_prog_cursor > count) s_prog_cursor = count;  /* clamp to last row */
        s_prog_editing = false;
    }
    s_force_redraw = true;
    return true;
}
