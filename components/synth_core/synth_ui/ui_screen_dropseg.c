#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "drop_core.h"
#include "display_dropseg.h"
#include "seq_core_config.h"  /* velocity range */
#include "seq_clamp.h"

/* ════════════════════════════════════════════════════════════════════════
 *  Ball drop segment popup - per-segment pitch offset / velocity
 * ════════════════════════════════════════════════════════════════════════
 * Addressed by the cell the drop cursor was on at open, not by segment index:
 * drop_remove_seg() moves the tail of the dense segment array, so an index
 * would slide onto a neighbour. Opened from the gesture dispatcher; controls
 * are in CONTROLS.md. */

static bool    s_ds_active  = false;
static uint8_t s_ds_col     = 0;
static uint8_t s_ds_row     = 0;
static uint8_t s_ds_field   = DS_FIELD_PITCH;
static bool    s_ds_editing = false;

bool synth_ui_dropseg_is_active(void)
{
    if (!s_ds_active) return false;
    if (seq_state.ui_mode == UI_MODE_DROP && !seq_state.menu_open &&
        drop_find_seg(s_ds_col, s_ds_row) >= 0) {
        return true;
    }
    /* Latched only while it can be seen: leaving the screen, opening the
     * menu or losing the segment closes it, so a later visit to the drop
     * screen never finds it open again. */
    s_ds_active  = false;
    s_ds_editing = false;
    return false;
}

void synth_ui_dropseg_open(void)
{
    uint8_t col, row;
    if (!synth_ui_drop_cursor_cell(&col, &row) || drop_find_seg(col, row) < 0) {
        return;
    }
    s_ds_active  = true;
    s_ds_col     = col;
    s_ds_row     = row;
    s_ds_field   = DS_FIELD_PITCH;
    s_ds_editing = false;
    s_force_redraw = true;
}

void synth_ui_dropseg_close(void)
{
    s_ds_active  = false;
    s_ds_editing = false;
    s_force_redraw = true;
}

bool synth_ui_dropseg_handle_button(void)
{
    if (!s_ds_active) return false;
    s_ds_editing = !s_ds_editing;
    s_force_redraw = true;
    return true;
}

bool synth_ui_dropseg_handle_encoder(long delta)
{
    if (!s_ds_active) return false;
    if (!s_ds_editing) {
        /* Navigate: one field per event, direction only. */
        if (delta > 0)      s_ds_field = (uint8_t)((s_ds_field + 1u) % DS_FIELD_COUNT);
        else if (delta < 0) s_ds_field = (uint8_t)((s_ds_field + DS_FIELD_COUNT - 1u) % DS_FIELD_COUNT);
        s_force_redraw = true;
        return true;
    }
    int idx = drop_find_seg(s_ds_col, s_ds_row);
    drop_seg_t s;
    if (idx < 0 || !drop_get_seg((uint8_t)idx, &s)) {
        synth_ui_dropseg_close();
        return true;
    }
    int d = (int)delta;
    if (s_ds_field == DS_FIELD_PITCH) {
        int v = (int)s.pitch_ofs + d;
        s.pitch_ofs = (int8_t)SEQ_CLAMP_INT(v, -SEQ_STEP_PITCH_OFS_MAX, SEQ_STEP_PITCH_OFS_MAX);
    } else {
        int v = (int)s.vel_adj + d * 5;
        s.vel_adj = (int8_t)SEQ_CLAMP_INT(v, -SEQ_STEP_VEL_ADJ_MAX, SEQ_STEP_VEL_ADJ_MAX);
    }
    (void)drop_set_seg(&s);
    s_force_redraw = true;
    return true;
}

void dropseg_build_view(dropseg_view_t *out)
{
    drop_seg_t s = { 0 };
    int idx = drop_find_seg(s_ds_col, s_ds_row);
    if (idx >= 0) (void)drop_get_seg((uint8_t)idx, &s);

    out->col       = s_ds_col;
    out->row       = s_ds_row;
    out->field     = s_ds_field;
    out->editing   = s_ds_editing ? 1u : 0u;
    out->pitch_ofs = s.pitch_ofs;
    out->vel_adj   = s.vel_adj;
}

uint32_t dropseg_view_signature(dropseg_view_t *out)
{
    dropseg_build_view(out);
    return fnv1a_bytes(FNV1A_OFFSET, out, sizeof(*out));
}
