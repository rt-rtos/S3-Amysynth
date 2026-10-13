#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_common.h"   /* DRONE_ROOT_*, DRONE_SUB_INTERVAL_* */
#include "patch_names.h"
#include "seq_clamp.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  DRONE SCREEN
 * ════════════════════════════════════════════════════════════════════════
 * Standalone stutter-drone synth (custompatches/drone_core): a scrollable
 * parameter list. Controls: CONTROLS.md. */

static uint8_t s_drone_cursor   = 0;
static bool    s_drone_editing  = false;

/* Rows, in screen order; all always visible. */
typedef enum {
    DROW_ENABLE = 0,
    DROW_PATCH,
    DROW_ROOT,        /* drone-local root note */
    DROW_CHORD,
    DROW_FOLLOW,      /* chord progression follow */
    DROW_CONST,       /* gate on level */
    DROW_MOD,         /* gate duck depth */
    DROW_RATE,
    DROW_GATE_LEN,
    DROW_SWING,
    DROW_PATTERN,
    DROW_BLIP,
    DROW_SUB,
    DROW_SUB_INTVL,
    DROW_ALL_COUNT
} drone_logical_row_t;

/* Fill `out` with the rows in screen order; return count. out[] must hold at
 * least DROW_ALL_COUNT entries. */
static uint8_t drone_visible_rows(drone_logical_row_t out[DROW_ALL_COUNT])
{
    for (drone_logical_row_t r = 0; r < DROW_ALL_COUNT; r++) out[r] = r;
    return DROW_ALL_COUNT;
}

static void drone_row_label_value(drone_logical_row_t r,
                                   char label[DRONE_LABEL_LEN],
                                   char value[DRONE_VALUE_LEN])
{
    switch (r) {
        case DROW_ENABLE:
            snprintf(label, DRONE_LABEL_LEN, "DRONE");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_get_enabled() ? "ON" : "OFF");
            break;
        case DROW_PATCH: {
            /* The patch name where the table has one, else the number. */
            uint16_t patch = drone_get_patch();
            const char *name = patch_name_for(patch);
            snprintf(label, DRONE_LABEL_LEN, "PATCH");
            if (name) snprintf(value, DRONE_VALUE_LEN, "%s", name);
            else      snprintf(value, DRONE_VALUE_LEN, "%u", (unsigned)patch);
            break;
        }
        case DROW_ROOT: {
            char nn[4];
            ui_note_name(drone_get_root_note(), nn);
            snprintf(label, DRONE_LABEL_LEN, "ROOT");
            snprintf(value, DRONE_VALUE_LEN, "%s", nn);
            break;
        }
        case DROW_CHORD:
            /* Under CHORD follow the progression supplies the chord type; the
             * own chord stays editable for when follow is switched off. */
            snprintf(label, DRONE_LABEL_LEN, "CHORD");
            if (drone_get_follow() == DRONE_FOLLOW_CHORD)
                snprintf(value, DRONE_VALUE_LEN, "(prog)");
            else
                snprintf(value, DRONE_VALUE_LEN, "%s", drone_chord_name(drone_get_chord()));
            break;
        case DROW_FOLLOW:
            snprintf(label, DRONE_LABEL_LEN, "FOLLOW");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_follow_name(drone_get_follow()));
            break;
        case DROW_CONST:
            snprintf(label, DRONE_LABEL_LEN, "LEVEL");
            snprintf(value, DRONE_VALUE_LEN, "%.1f", (double)drone_get_amp_peak());
            break;
        case DROW_MOD:
            snprintf(label, DRONE_LABEL_LEN, "DUCK");
            snprintf(value, DRONE_VALUE_LEN, "%.1f", (double)drone_get_amp_duck());
            break;
        case DROW_RATE:
            snprintf(label, DRONE_LABEL_LEN, "STUTTER");
            snprintf(value, DRONE_VALUE_LEN, "%s", note_div_label(drone_get_rate()));
            break;
        case DROW_GATE_LEN:
            snprintf(label, DRONE_LABEL_LEN, "GATE");
            snprintf(value, DRONE_VALUE_LEN, "%.2f", (double)drone_get_gate_len());
            break;
        case DROW_SWING:
            snprintf(label, DRONE_LABEL_LEN, "SWING");
            snprintf(value, DRONE_VALUE_LEN, "%u%%", (unsigned)drone_get_swing());
            break;
        case DROW_PATTERN:
            snprintf(label, DRONE_LABEL_LEN, "PATTERN");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_pattern_name(drone_get_pattern()));
            break;
        case DROW_BLIP:
            snprintf(label, DRONE_LABEL_LEN, "BLIP");
            snprintf(value, DRONE_VALUE_LEN, "%.2f", (double)drone_get_blip());
            break;
        case DROW_SUB:
            snprintf(label, DRONE_LABEL_LEN, "SUB");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_get_sub_enabled() ? "ON" : "OFF");
            break;
        case DROW_SUB_INTVL:
            snprintf(label, DRONE_LABEL_LEN, "SUB INT");
            snprintf(value, DRONE_VALUE_LEN, "%d", (int)drone_get_sub_interval());
            break;
        default:
            label[0] = '\0'; value[0] = '\0';
            break;
    }
}

/* Static backing store for the flat view rows the renderer reads. */
static drone_row_view_t s_drone_rows[DROW_ALL_COUNT];

void drone_build_view(drone_view_t *out)
{
    drone_logical_row_t vis[DROW_ALL_COUNT];
    uint8_t n = drone_visible_rows(vis);
    for (uint8_t i = 0; i < n; i++) {
        drone_row_label_value(vis[i], s_drone_rows[i].label, s_drone_rows[i].value);
        /* The Pattern row draws its 8 steps beside the name. */
        s_drone_rows[i].has_glyph = (vis[i] == DROW_PATTERN);
        s_drone_rows[i].glyph     = drone_pattern_mask(drone_get_pattern());
    }
    if (s_drone_cursor >= n) s_drone_cursor = (n ? (uint8_t)(n - 1) : 0);
    out->rows    = s_drone_rows;
    out->count   = n;
    out->cursor  = s_drone_cursor;
    out->editing = s_drone_editing;
}

static void drone_edit_row(drone_logical_row_t r, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    switch (r) {
        case DROW_ENABLE:
            if (dir != 0) drone_set_enabled(!drone_get_enabled());
            break;
        case DROW_PATCH:
            synth_ui_drone_cycle_patch(dir);
            break;
        case DROW_ROOT: {
            int rv = SEQ_CLAMP_INT((int)drone_get_root_note() + dir,
                                   DRONE_ROOT_MIN, DRONE_ROOT_MAX);
            drone_set_root_note((uint8_t)rv);
            break;
        }
        case DROW_CHORD: {
            /* Wraps, unlike the clamped rows around it: CHORD_OFF is the
             * default and sits last, so wrapping keeps it one detent from the
             * first chord instead of stranding it at the end. */
            int c = ((int)drone_get_chord() + dir + CHORD_TYPE_COUNT) % CHORD_TYPE_COUNT;
            drone_set_chord((chord_type_t)c);
            break;
        }
        case DROW_FOLLOW: {
            int f = ((int)drone_get_follow() + dir + DRONE_FOLLOW_COUNT) % DRONE_FOLLOW_COUNT;
            drone_set_follow((drone_follow_t)f);
            break;
        }
        case DROW_CONST:
            drone_set_amp_peak(drone_get_amp_peak() + (float)dir * 0.1f);
            break;
        case DROW_MOD:
            drone_set_amp_duck(drone_get_amp_duck() + (float)dir * 0.1f);
            break;
        case DROW_RATE: {
            int v = SEQ_CLAMP((int)drone_get_rate() + dir, 0, NOTE_DIV_STD_COUNT - 1);
            drone_set_rate((note_div_t)v);
            break;
        }
        case DROW_GATE_LEN:
            drone_set_gate_len(drone_get_gate_len() + (float)dir * 0.05f);
            break;
        case DROW_SWING:
            drone_set_swing((uint8_t)SEQ_CLAMP(
                (int)drone_get_swing() + dir * 2, 0, 66));
            break;
        case DROW_PATTERN: {
            int v = SEQ_CLAMP((int)drone_get_pattern() + dir, 0, DRONE_PAT_COUNT - 1);
            drone_set_pattern((drone_pattern_t)v);
            break;
        }
        case DROW_BLIP:
            drone_set_blip(drone_get_blip() + (float)dir * 0.05f);
            break;
        case DROW_SUB:
            if (dir != 0) drone_set_sub_enabled(!drone_get_sub_enabled());
            break;
        case DROW_SUB_INTVL:
            drone_set_sub_interval((int8_t)SEQ_CLAMP(
                (int)drone_get_sub_interval() + dir,
                DRONE_SUB_INTERVAL_MIN, DRONE_SUB_INTERVAL_MAX));
            break;
        default:
            break;
    }
}

/* View-resolved: true only while this screen is what the display shows, so
 * the filter/LFO overlays keep their own buttons. */
bool synth_ui_drone_is_active(void)
{
    return synth_ui_active_view() == UI_VIEW_DRONE;
}

void synth_ui_drone_handle_encoder(long delta)
{
    if (delta == 0) return;
    drone_logical_row_t vis[DROW_ALL_COUNT];
    uint8_t n = drone_visible_rows(vis);
    if (n == 0) return;
    if (s_drone_cursor >= n) s_drone_cursor = (uint8_t)(n - 1);

    if (s_drone_editing) {
        drone_edit_row(vis[s_drone_cursor], (int)delta);
    } else {
        int c = (int)s_drone_cursor + (int)delta;
        c = SEQ_CLAMP_INT(c, 0, (int)n - 1);
        s_drone_cursor = (uint8_t)c;
    }
    s_force_redraw = true;
}

void synth_ui_drone_handle_button(void)
{
    s_drone_editing = !s_drone_editing;
    s_force_redraw = true;
}

/* Signature of the drone screen. Builds the view into *out so the caller draws
 * from it without a second build. */
uint32_t drone_view_signature(drone_view_t *out)
{
    uint32_t h = FNV1A_OFFSET;
    drone_build_view(out);
    h = fnv1a_bytes(h, &out->cursor, sizeof(out->cursor));
    h = fnv1a_bytes(h, &out->editing, sizeof(out->editing));
    h = fnv1a_bytes(h, &out->count, sizeof(out->count));
    for (uint8_t i = 0; i < out->count; i++) {
        h = fnv1a_bytes(h, out->rows[i].label, sizeof(out->rows[i].label));
        h = fnv1a_bytes(h, out->rows[i].value, sizeof(out->rows[i].value));
    }
    return h;
}
