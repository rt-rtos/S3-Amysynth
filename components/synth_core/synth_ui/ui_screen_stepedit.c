#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "display_stepedit.h"
#include "seq_core_config.h"  /* nudge / velocity / taper ranges */
#include "seq_clamp.h"

/* ════════════════════════════════════════════════════════════════════════
 *  Step Trig editor — per-step pitch offset / probability / ratchet / cond
 * ════════════════════════════════════════════════════════════════════════
 * Addressed by the sequencer grid's cursor (active_layer_idx / selected_track /
 * selected_step), not a parallel one. Opened from main.c's dispatch; controls
 * are in CONTROLS.md. The last field, Frame (the step's wavetable frame lock,
 * sequencer_core_set_step_frame), is listed only while the track plays a
 * wavetable patch. */

static bool    s_se_active  = false;
static uint8_t s_se_field   = SE_FIELD_PITCH;
/* Select/adjust phase. Prev is a boolean: click toggles it directly, with no
 * adjust phase. */
static bool    s_se_editing = false;
/* Top of the visible window: the field list outgrew the panel, so navigation
 * drags the window along instead of the list being drawn whole. */
static uint8_t s_se_first   = 0;

/* Fields listed for the cursor's track: Frame only on a wavetable patch. */
static uint8_t se_field_count(void)
{
    uint8_t li = seq_state.active_layer_idx;
    if (li < seq_state.num_layers && seq_state.layers[li].type == SEQ_LAYER_MELODIC &&
        sequencer_core_is_wavetable_patch(
            sequencer_core_get_melodic_track_patch(li, seq_state.selected_track)))
        return SE_FIELD_COUNT;
    return SE_FIELD_FRAME;
}

/* Keep the cursor inside the window after a navigation step; a wrap-around
 * jumps the window to the other end. */
static void se_window_follow(void)
{
    if (s_se_field < s_se_first) {
        s_se_first = s_se_field;
    } else if (s_se_field >= s_se_first + SE_VISIBLE_ROWS) {
        s_se_first = (uint8_t)(s_se_field - SE_VISIBLE_ROWS + 1);
    }
}

bool synth_ui_stepedit_is_active(void)
{
    /* The overlay decorates the grid's own cursor, so it is active only while
     * that grid is the live screen. Gating here (as every sibling
     * *_is_active() does) means draw cascade, input routers and hint strip all
     * drop it the instant the user leaves the sequencer or opens the menu - no
     * per-consumer teardown. */
    return s_se_active
        && seq_state.ui_mode == UI_MODE_SEQUENCER
        && !seq_state.menu_open;
}

void synth_ui_stepedit_open(void)
{
    /* Mirrors the is_active() gate: opening elsewhere (or with the menu up)
     * would latch s_se_active onto a hidden grid cursor, editing an unseen
     * step. */
    if (seq_state.ui_mode != UI_MODE_SEQUENCER || seq_state.menu_open) {
        return;
    }
    s_se_active  = true;
    s_se_field   = SE_FIELD_PITCH;
    s_se_first   = 0;
    s_se_editing = false;
    s_force_redraw = true;
}

void synth_ui_stepedit_close(void)
{
    s_se_active  = false;
    s_se_editing = false;
    s_force_redraw = true;
}

bool synth_ui_stepedit_handle_button(void)
{
    if (!s_se_active) return false;
    if (s_se_field == SE_FIELD_PREV) {
        /* Boolean: toggle on click, never enter adjust mode. */
        uint8_t li = seq_state.active_layer_idx;
        uint8_t t  = seq_state.selected_track;
        uint8_t s  = seq_state.selected_step;
        sequencer_core_set_step_prev(li, t, s,
                                     !sequencer_core_get_step_prev(li, t, s));
    } else {
        s_se_editing = !s_se_editing;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_stepedit_handle_encoder(long delta)
{
    if (!s_se_active) return false;
    uint8_t n = se_field_count();
    if (s_se_field >= n) {   /* Frame vanished with a patch change */
        s_se_field   = (uint8_t)(n - 1u);
        s_se_editing = false;
    }
    if (!s_se_editing) {
        /* Navigate: one field per event, direction only. */
        if (delta > 0)      s_se_field = (uint8_t)((s_se_field + 1) % n);
        else if (delta < 0) s_se_field = (uint8_t)((s_se_field + n - 1) % n);
        se_window_follow();
        s_force_redraw = true;
        return true;
    }
    uint8_t li = seq_state.active_layer_idx;
    uint8_t t  = seq_state.selected_track;
    uint8_t s  = seq_state.selected_step;
    int d = (int)delta;

    switch (s_se_field) {
        case SE_FIELD_PITCH: {
            int v = (int)sequencer_core_get_step_pitch_ofs(li, t, s) + d;
            v = SEQ_CLAMP_INT(v, -SEQ_STEP_PITCH_OFS_MAX, SEQ_STEP_PITCH_OFS_MAX);
            sequencer_core_set_step_pitch_ofs(li, t, s, (int8_t)v);
            break;
        }
        case SE_FIELD_PROB: {
            int v = (int)sequencer_core_get_step_prob(li, t, s) + d * 5;
            sequencer_core_set_step_prob(li, t, s, SEQ_CLAMP_U8(v, 0, 100));
            break;
        }
        case SE_FIELD_RATCHET: {
            int v = (int)sequencer_core_get_step_ratchet(li, t, s) + d;
            sequencer_core_set_step_ratchet(li, t, s, SEQ_CLAMP_U8(v, 1, SEQ_MAX_RATCHET));
            break;
        }
        case SE_FIELD_EVERY: {
            int v = (int)sequencer_core_get_step_every(li, t, s) + d;
            sequencer_core_set_step_every(li, t, s, SEQ_CLAMP_U8(v, 1, SEQ_STEP_EVERY_MAX));
            break;
        }
        case SE_FIELD_VEL: {
            int v = (int)sequencer_core_get_step_velocity_adj(li, t, s) + d * 5;
            v = SEQ_CLAMP_INT(v, -SEQ_STEP_VEL_ADJ_MAX, SEQ_STEP_VEL_ADJ_MAX);
            sequencer_core_set_step_velocity_adj(li, t, s, (int8_t)v);
            break;
        }
        case SE_FIELD_NUDGE: {
            int v = (int)sequencer_core_get_step_nudge(li, t, s) + d;
            v = SEQ_CLAMP_INT(v, -SEQ_STEP_NUDGE_MAX, SEQ_STEP_NUDGE_MAX);
            sequencer_core_set_step_nudge(li, t, s, (int8_t)v);
            break;
        }
        case SE_FIELD_TAPER: {
            int v = (int)sequencer_core_get_step_ratchet_taper(li, t, s) + d * 5;
            v = SEQ_CLAMP_INT(v, -SEQ_STEP_TAPER_MAX, SEQ_STEP_TAPER_MAX);
            sequencer_core_set_step_ratchet_taper(li, t, s, (int8_t)v);
            break;
        }
        case SE_FIELD_FRAME: {
            /* One frame per detent; no lock ("--", 0) sits below frame 0. */
            int v = (int)sequencer_core_get_step_frame(li, t, s) + d;
            sequencer_core_set_step_frame(li, t, s, (uint8_t)SEQ_CLAMP_INT(v, 0, 64));
            break;
        }
        default:
            /* SE_FIELD_PREV never enters adjust mode (click-toggle). */
            break;
    }
    s_force_redraw = true;
    return true;
}

void stepedit_build_view(stepedit_view_t *out)
{
    uint8_t li = seq_state.active_layer_idx;
    uint8_t t  = seq_state.selected_track;
    uint8_t s  = seq_state.selected_step;

    out->layer_idx    = li;
    out->track_idx    = t;
    out->step_idx     = s;
    out->pitch_ofs    = sequencer_core_get_step_pitch_ofs(li, t, s);
    out->prob         = sequencer_core_get_step_prob(li, t, s);
    out->ratchet      = sequencer_core_get_step_ratchet(li, t, s);
    out->every        = sequencer_core_get_step_every(li, t, s);
    out->prev         = sequencer_core_get_step_prev(li, t, s) ? 1u : 0u;
    out->field_cursor = s_se_field;
    out->editing      = s_se_editing ? 1u : 0u;
    out->vel_adj      = sequencer_core_get_step_velocity_adj(li, t, s);
    out->nudge        = sequencer_core_get_step_nudge(li, t, s);
    out->taper        = sequencer_core_get_step_ratchet_taper(li, t, s);
    out->first_row    = s_se_first;
    out->has_frame    = (se_field_count() == SE_FIELD_COUNT) ? 1u : 0u;
    out->frame        = sequencer_core_get_step_frame(li, t, s);
}

uint32_t stepedit_view_signature(stepedit_view_t *out)
{
    stepedit_build_view(out);
    return fnv1a_bytes(FNV1A_OFFSET, out, sizeof(*out));
}
