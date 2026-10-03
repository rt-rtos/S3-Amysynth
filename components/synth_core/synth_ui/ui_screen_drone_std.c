#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "custompatches/drone_std_core.h"
#include "seq_clamp.h"
#include "amy.h"
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  NORMAL DRONE SCREEN
 * ════════════════════════════════════════════════════════════════════════
 * Free-running drone (custompatches/drone_std_core). Scrollable list; the
 * stutter screen is reached through the STUTTER row. Filter/LFO/ADSR have no
 * rows here; they live in the shared editors. Controls: CONTROLS.md. */

static uint8_t s_dstd_cursor  = 0;
static bool    s_dstd_editing = false;

/* Logical rows (superset). visible_rows() filters by source each frame. */
typedef enum {
    DSROW_STUTTER = 0,  /* dive row: opens the stutter drone screen */
    DSROW_ENABLE,
    DSROW_SOURCE,
    DSROW_WAVE,         /* WAVE only  */
    DSROW_ROOT,
    DSROW_CHORD,
    DSROW_FOLLOW,       /* chord progression follow (both modes) */
    DSROW_LEVEL,
    DSROW_SUB,
    DSROW_SUB_INTVL,
    DSROW_PATCH,        /* PATCH only */
    DSROW_ALL_COUNT
} drone_std_row_t;

static uint8_t drone_std_visible_rows(drone_std_row_t out[DSROW_ALL_COUNT])
{
    bool wave = (drone_std_get_source() == DRONE_SRC_WAVE);
    uint8_t n = 0;
    for (drone_std_row_t r = 0; r < DSROW_ALL_COUNT; r++) {
        if (wave && r == DSROW_PATCH) continue;
        if (!wave && r == DSROW_WAVE) continue;
        out[n++] = r;
    }
    return n;
}

static void drone_std_row_label_value(drone_std_row_t r,
                                      char label[DRONE_LABEL_LEN],
                                      char value[DRONE_VALUE_LEN])
{
    switch (r) {
        case DSROW_STUTTER:
            snprintf(label, DRONE_LABEL_LEN, "STUTTER");
            snprintf(value, DRONE_VALUE_LEN, "%s>",
                     drone_get_enabled() ? "ON " : "");
            break;
        case DSROW_ENABLE:
            snprintf(label, DRONE_LABEL_LEN, "DRONE");
            snprintf(value, DRONE_VALUE_LEN, "%s",
                     drone_std_get_enabled() ? "ON" : "OFF");
            break;
        case DSROW_SOURCE:
            snprintf(label, DRONE_LABEL_LEN, "SOURCE");
            snprintf(value, DRONE_VALUE_LEN, "%s",
                     drone_std_get_source() == DRONE_SRC_PATCH ? "PATCH" : "WAVE");
            break;
        case DSROW_WAVE:
            snprintf(label, DRONE_LABEL_LEN, "WAVE");
            snprintf(value, DRONE_VALUE_LEN, "%s",
                     drone_wave_name(drone_std_get_wave()));
            break;
        case DSROW_ROOT: {
            char nn[4];
            ui_note_name(drone_std_get_root_note(), nn);
            snprintf(label, DRONE_LABEL_LEN, "ROOT");
            snprintf(value, DRONE_VALUE_LEN, "%s", nn);
            break;
        }
        case DSROW_CHORD:
            /* As on the stutter screen: "(prog)" while CHORD follow supplies
             * the chord type. */
            snprintf(label, DRONE_LABEL_LEN, "CHORD");
            if (drone_std_get_follow() == DRONE_FOLLOW_CHORD)
                snprintf(value, DRONE_VALUE_LEN, "(prog)");
            else
                snprintf(value, DRONE_VALUE_LEN, "%s",
                         drone_chord_name(drone_std_get_chord()));
            break;
        case DSROW_FOLLOW:
            snprintf(label, DRONE_LABEL_LEN, "FOLLOW");
            snprintf(value, DRONE_VALUE_LEN, "%s",
                     drone_follow_name(drone_std_get_follow()));
            break;
        case DSROW_LEVEL:
            snprintf(label, DRONE_LABEL_LEN, "LEVEL");
            snprintf(value, DRONE_VALUE_LEN, "%.1f",
                     (double)drone_std_get_level());
            break;
        case DSROW_SUB:
            snprintf(label, DRONE_LABEL_LEN, "SUB");
            snprintf(value, DRONE_VALUE_LEN, "%s",
                     drone_std_get_sub_enabled() ? "ON" : "OFF");
            break;
        case DSROW_SUB_INTVL:
            snprintf(label, DRONE_LABEL_LEN, "SUB INT");
            snprintf(value, DRONE_VALUE_LEN, "%d",
                     (int)drone_std_get_sub_interval());
            break;
        case DSROW_PATCH:
            snprintf(label, DRONE_LABEL_LEN, "PATCH");
            snprintf(value, DRONE_VALUE_LEN, "%u",
                     (unsigned)drone_std_get_patch());
            break;
        default:
            label[0] = '\0'; value[0] = '\0';
            break;
    }
}

/* Static backing store for the flat view rows the renderer reads. */
static drone_row_view_t s_dstd_rows[DSROW_ALL_COUNT];

void drone_std_build_view(drone_view_t *out)
{
    drone_std_row_t vis[DSROW_ALL_COUNT];
    uint8_t n = drone_std_visible_rows(vis);
    for (uint8_t i = 0; i < n; i++) {
        drone_std_row_label_value(vis[i], s_dstd_rows[i].label, s_dstd_rows[i].value);
    }
    if (s_dstd_cursor >= n) s_dstd_cursor = (n ? (uint8_t)(n - 1) : 0);
    out->rows    = s_dstd_rows;
    out->count   = n;
    out->cursor  = s_dstd_cursor;
    out->editing = s_dstd_editing;
}

static void drone_std_edit_row(drone_std_row_t r, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    switch (r) {
        case DSROW_ENABLE:
            if (dir != 0) drone_std_set_enabled(!drone_std_get_enabled());
            break;
        case DSROW_SOURCE:
            if (dir != 0)
                drone_std_set_source(drone_std_get_source() == DRONE_SRC_WAVE
                                     ? DRONE_SRC_PATCH : DRONE_SRC_WAVE);
            break;
        case DSROW_WAVE: {
            /* Same wave cycle as the stutter drone (NOISE/KS excluded). */
            static const uint16_t waves[] = { SAW_DOWN, SAW_UP, PULSE, TRIANGLE, SINE };
            const int wn = (int)(sizeof(waves) / sizeof(waves[0]));
            int idx = 0;
            for (int i = 0; i < wn; i++) {
                if (waves[i] == drone_std_get_wave()) { idx = i; break; }
            }
            idx = (idx + dir + wn) % wn;
            drone_std_set_wave(waves[idx]);
            break;
        }
        case DSROW_ROOT: {
            int rv = SEQ_CLAMP_INT((int)drone_std_get_root_note() + dir, 24, 72);
            drone_std_set_root_note((uint8_t)rv);
            break;
        }
        case DSROW_CHORD: {
            /* Wraps, unlike the clamped rows around it: CHORD_OFF is the
             * default and sits last, so wrapping keeps it one detent from the
             * first chord instead of stranding it at the end. */
            int c = ((int)drone_std_get_chord() + dir + CHORD_TYPE_COUNT) % CHORD_TYPE_COUNT;
            drone_std_set_chord((chord_type_t)c);
            break;
        }
        case DSROW_FOLLOW: {
            int f = ((int)drone_std_get_follow() + dir + DRONE_FOLLOW_COUNT) % DRONE_FOLLOW_COUNT;
            drone_std_set_follow((drone_follow_t)f);
            break;
        }
        case DSROW_LEVEL:
            drone_std_set_level(drone_std_get_level() + (float)dir * 0.1f);
            break;
        case DSROW_SUB:
            if (dir != 0) drone_std_set_sub_enabled(!drone_std_get_sub_enabled());
            break;
        case DSROW_SUB_INTVL:
            drone_std_set_sub_interval((int8_t)SEQ_CLAMP(
                (int)drone_std_get_sub_interval() + dir, -36, 0));
            break;
        case DSROW_PATCH:
            synth_ui_drone_std_cycle_patch(dir);
            break;
        case DSROW_STUTTER:
        default:
            break;   /* dive row: handled on button press, not encoder turn */
    }
}

/* True only when this screen itself owns the display: resolving through view
 * precedence excludes the filter/LFO overlays, which sit on top without
 * clearing ui_mode. main.c's input isolation guard keys off this, so a stale
 * "screen is up" would swallow the editors' own MY_BUTTON_0 taps. */
bool synth_ui_drone_std_is_active(void)
{
    return synth_ui_active_view() == UI_VIEW_DRONE_STD;
}

void synth_ui_drone_std_handle_encoder(long delta)
{
    if (delta == 0) return;
    drone_std_row_t vis[DSROW_ALL_COUNT];
    uint8_t n = drone_std_visible_rows(vis);
    if (n == 0) return;
    if (s_dstd_cursor >= n) s_dstd_cursor = (uint8_t)(n - 1);

    if (s_dstd_editing) {
        drone_std_edit_row(vis[s_dstd_cursor], (int)delta);
    } else {
        int c = (int)s_dstd_cursor + (int)delta;
        c = SEQ_CLAMP_INT(c, 0, (int)n - 1);
        s_dstd_cursor = (uint8_t)c;
    }
    s_force_redraw = true;
}

void synth_ui_drone_std_handle_button(void)
{
    drone_std_row_t vis[DSROW_ALL_COUNT];
    uint8_t n = drone_std_visible_rows(vis);
    if (n > 0 && s_dstd_cursor < n && vis[s_dstd_cursor] == DSROW_STUTTER) {
        /* Dive into the stutter drone screen; menu brings the user back. */
        seq_state.ui_mode = UI_MODE_DRONE;
        s_dstd_editing = false;
        s_force_redraw = true;
        return;
    }
    s_dstd_editing = !s_dstd_editing;
    s_force_redraw = true;
}

/* Signature of the normal drone screen (everything the renderer reads). */
uint32_t drone_std_view_signature(drone_view_t *out)
{
    uint32_t h = FNV1A_OFFSET;
    drone_std_build_view(out);
    h = fnv1a_bytes(h, &out->cursor, sizeof(out->cursor));
    h = fnv1a_bytes(h, &out->editing, sizeof(out->editing));
    h = fnv1a_bytes(h, &out->count, sizeof(out->count));
    for (uint8_t i = 0; i < out->count; i++) {
        h = fnv1a_bytes(h, out->rows[i].label, sizeof(out->rows[i].label));
        h = fnv1a_bytes(h, out->rows[i].value, sizeof(out->rows[i].value));
    }
    return h;
}
