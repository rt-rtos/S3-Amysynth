#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "custompatches/wt_builder.h"
#include "seq_clamp.h"
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  Custom wavetable editor
 * ════════════════════════════════════════════════════════════════════════
 * Edits the builder's sixteen parameters (custompatches/wt_builder.h;
 * domains in wt_synth.h) one keyframe at a time; every edit marks the table
 * dirty and the seq_ui task rebuilds it in slices. Rendered by display_wt.c.
 * The view only formats the parameters and copies the builder's cached
 * previews: no table math here. Controls: CONTROLS.md. */

_Static_assert(WT_VIEW_POINTS == WT_PREVIEW_POINTS, "display_wt.h preview width");
_Static_assert(WT_VIEW_KEYS == WT_KEYS, "display_wt.h keyframe count");

static uint8_t s_wt_cursor  = WT_CUR_SHP;
static uint8_t s_wt_key     = 0;          /* focused keyframe: 0 = A, 1 = M, 2 = B */
static bool    s_wt_editing = false;

bool synth_ui_wt_is_active(void)
{
    return seq_state.ui_mode == UI_MODE_WT && !seq_state.menu_open;
}

bool synth_ui_wt_on_range(void)
{
    return s_wt_cursor == WT_CUR_RNG;
}

uint8_t synth_ui_wt_keyframe(void)
{
    return s_wt_key;
}

/* Cursor stops map onto wt_field_t in the same order. */
static wt_field_t wt_cursor_field(uint8_t c)
{
    return (wt_field_t)(WT_FIELD_SHAPE + (c - WT_CUR_SHP));
}

void wt_build_view(wt_view_t *out)
{
    wt_params_t p;
    wt_preview_t pv;
    /* Zeroed first: the signature hashes the whole struct. */
    memset(out, 0, sizeof(*out));
    wt_builder_get_params(&p);
    wt_builder_preview(&pv);

    out->cursor     = s_wt_cursor;
    out->editing    = s_wt_editing;
    out->key        = s_wt_key;
    out->generation = wt_builder_generation();

    const uint8_t k = s_wt_key;
    snprintf(out->cells[WT_CUR_SHP], WT_CELL_LEN, "%u", (unsigned)p.shape[k]);
    snprintf(out->cells[WT_CUR_WID], WT_CELL_LEN, "%u", (unsigned)p.width[k]);
    snprintf(out->cells[WT_CUR_BRT], WT_CELL_LEN, "%u", (unsigned)p.bright[k]);
    snprintf(out->cells[WT_CUR_SYN], WT_CELL_LEN, "%u.%u",
             (unsigned)(p.sync[k] / 10u), (unsigned)(p.sync[k] % 10u));
    if (p.peak[k] == 0u) snprintf(out->cells[WT_CUR_PK], WT_CELL_LEN, "off");
    else                 snprintf(out->cells[WT_CUR_PK], WT_CELL_LEN, "%u", (unsigned)p.peak[k]);

    char note[4];
    ui_note_name(wt_synth_clean_note(p.range), note);
    snprintf(out->cells[WT_CUR_RNG], WT_CELL_LEN, "%s", note);

    memcpy(out->wave, pv.frame, sizeof(out->wave));
}

uint32_t wt_view_signature(wt_view_t *out)
{
    wt_build_view(out);
    return fnv1a_bytes(FNV1A_OFFSET, out, sizeof(*out));
}

/* One step per detent in the field's own unit; Peak steps Off <-> 2 across
 * the gap at 1. */
static void wt_edit(int delta)
{
    wt_params_t p;
    wt_builder_get_params(&p);
    uint8_t k = s_wt_key;
    wt_field_t f = wt_cursor_field(s_wt_cursor);
    int v;
    switch (f) {
        case WT_FIELD_SHAPE:  v = SEQ_CLAMP_INT((int)p.shape[k] + delta, 0, 100);  break;
        case WT_FIELD_WIDTH:  v = SEQ_CLAMP_INT((int)p.width[k] + delta, 10, 90);  break;
        case WT_FIELD_BRIGHT: v = SEQ_CLAMP_INT((int)p.bright[k] + delta, 0, 10); break;
        case WT_FIELD_SYNC:   v = SEQ_CLAMP_INT((int)p.sync[k] + delta, 10, 80);  break;
        case WT_FIELD_PEAK:
            v = (p.peak[k] == 0u) ? ((delta > 0) ? 1 + delta : 0) : (int)p.peak[k] + delta;
            v = (v < 2) ? 0 : SEQ_CLAMP_INT(v, 2, 63);
            break;
        case WT_FIELD_RANGE:  v = SEQ_CLAMP_INT((int)p.range + delta, 0, 3);      break;
        default: return;
    }
    wt_builder_set_field(f, k, (uint8_t)v);
}

bool synth_ui_wt_handle_encoder(int delta)
{
    if (!synth_ui_wt_is_active()) return false;
    if (delta == 0) return true;
    if (s_wt_editing) {
        wt_edit(delta);
    } else {
        int c = (int)s_wt_cursor + delta;
        s_wt_cursor = (uint8_t)SEQ_CLAMP_INT(c, 0, (int)WT_CUR_COUNT - 1);
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_wt_handle_button(void)
{
    if (!synth_ui_wt_is_active()) return false;
    s_wt_editing = !s_wt_editing;
    s_force_redraw = true;
    return true;
}

bool synth_ui_wt_next_keyframe(void)
{
    if (!synth_ui_wt_is_active()) return false;
    s_wt_key = (uint8_t)((s_wt_key + 1u) % WT_KEYS);
    s_force_redraw = true;
    return true;
}

bool synth_ui_wt_copy_keyframe(void)
{
    if (!synth_ui_wt_is_active()) return false;
    if (s_wt_cursor == WT_CUR_RNG) return true;
    wt_builder_copy_keyframe(s_wt_key);
    s_force_redraw = true;
    return true;
}

bool synth_ui_wt_reset_keyframe(void)
{
    if (!synth_ui_wt_is_active()) return false;
    if (s_wt_cursor == WT_CUR_RNG) {
        wt_params_t d;
        wt_params_default(&d);
        wt_builder_set_field(WT_FIELD_RANGE, 0, d.range);
    } else if (s_wt_key == 1u) {
        wt_builder_blend_mid();
    } else {
        wt_builder_reset_keyframe(s_wt_key);
    }
    s_force_redraw = true;
    return true;
}
