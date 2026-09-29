#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "custompatches/drone_core.h"
#include "seq_clamp.h"
#include "esp_log.h"
#include "amy.h"
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  DRONE SCREEN
 * ════════════════════════════════════════════════════════════════════════
 * Standalone stutter-drone synth (custompatches/drone_core): a scrollable
 * parameter list whose rows depend on the WAVE/PATCH source. Controls:
 * CONTROLS.md. */

static uint8_t s_drone_cursor   = 0;
static bool    s_drone_editing  = false;
bool           s_drone_vis_open = false;

/* Logical rows (superset). visible_rows() filters by source each frame. */
typedef enum {
    DROW_ENABLE = 0,
    DROW_SOURCE,
    DROW_WAVE,        /* WAVE only */
    DROW_ROOT,        /* drone-local root note (both modes) */
    DROW_CHORD,
    DROW_RES,
    DROW_CONST,       /* WAVE only */
    DROW_MOD,         /* WAVE only */
    DROW_VIS_POPUP,   /* always visible — opens the drone visualiser overlay */
    DROW_RATE,        /* WAVE only */
    DROW_GATE_LEN,    /* WAVE only */
    DROW_SWING,
    DROW_PATTERN,
    DROW_BLIP,
    DROW_SWEEP_LO,
    DROW_SWEEP_HI,
    DROW_SWEEP_BARS,
    DROW_SUB,
    DROW_SUB_INTVL,
    DROW_PATCH,       /* PATCH only */
    DROW_ALL_COUNT
} drone_logical_row_t;

/* Fill `out` with the logical rows visible for the current source; return count.
 * out[] must hold at least DROW_ALL_COUNT entries. */
static uint8_t drone_visible_rows(drone_logical_row_t out[DROW_ALL_COUNT])
{
    bool wave = (drone_get_source() == DRONE_SRC_WAVE);
    uint8_t n = 0;
    for (drone_logical_row_t r = 0; r < DROW_ALL_COUNT; r++) {
        if (wave && r == DROW_PATCH) continue;
        if (!wave && (r == DROW_WAVE || r == DROW_CONST || r == DROW_MOD
                      || r == DROW_RATE || r == DROW_GATE_LEN)) continue;
        out[n++] = r;
    }
    return n;
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
        case DROW_SOURCE:
            snprintf(label, DRONE_LABEL_LEN, "SOURCE");
            snprintf(value, DRONE_VALUE_LEN, "%s",
                     drone_get_source() == DRONE_SRC_PATCH ? "PATCH" : "WAVE");
            break;
        case DROW_WAVE:
            snprintf(label, DRONE_LABEL_LEN, "WAVE");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_wave_name(drone_get_wave()));
            break;
        case DROW_ROOT: {
            char nn[4];
            ui_note_name(drone_get_root_note(), nn);
            snprintf(label, DRONE_LABEL_LEN, "ROOT");
            snprintf(value, DRONE_VALUE_LEN, "%s", nn);
            break;
        }
        case DROW_CHORD:
            snprintf(label, DRONE_LABEL_LEN, "CHORD");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_chord_name(drone_get_chord()));
            break;
        case DROW_RES:
            snprintf(label, DRONE_LABEL_LEN, "RES");
            snprintf(value, DRONE_VALUE_LEN, "%.2f", (double)drone_get_resonance());
            break;
        case DROW_CONST:
            snprintf(label, DRONE_LABEL_LEN, "PEAK");
            snprintf(value, DRONE_VALUE_LEN, "%.1f", (double)drone_get_amp_peak());
            break;
        case DROW_MOD:
            snprintf(label, DRONE_LABEL_LEN, "DUCK");
            snprintf(value, DRONE_VALUE_LEN, "%.1f", (double)drone_get_amp_duck());
            break;
        case DROW_VIS_POPUP:
            snprintf(label, DRONE_LABEL_LEN, "VISUALISE");
            snprintf(value, DRONE_VALUE_LEN, "[ OPEN ]");
            break;
        case DROW_RATE:
            snprintf(label, DRONE_LABEL_LEN, "STUTTER");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_rate_name(drone_get_rate()));
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
        case DROW_SWEEP_LO:
            snprintf(label, DRONE_LABEL_LEN, "SWEEP LO");
            snprintf(value, DRONE_VALUE_LEN, "%dHz", (int)(drone_get_sweep_lo() + 0.5f));
            break;
        case DROW_SWEEP_HI:
            snprintf(label, DRONE_LABEL_LEN, "SWEEP HI");
            snprintf(value, DRONE_VALUE_LEN, "%dHz", (int)(drone_get_sweep_hi() + 0.5f));
            break;
        case DROW_SWEEP_BARS:
            snprintf(label, DRONE_LABEL_LEN, "SWEEP SPD");
            snprintf(value, DRONE_VALUE_LEN, "%ubar", (unsigned)drone_get_sweep_bars());
            break;
        case DROW_SUB:
            snprintf(label, DRONE_LABEL_LEN, "SUB");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_get_sub_enabled() ? "ON" : "OFF");
            break;
        case DROW_SUB_INTVL:
            snprintf(label, DRONE_LABEL_LEN, "SUB INT");
            snprintf(value, DRONE_VALUE_LEN, "%d", (int)drone_get_sub_interval());
            break;
        case DROW_PATCH:
            snprintf(label, DRONE_LABEL_LEN, "PATCH");
            snprintf(value, DRONE_VALUE_LEN, "%u", (unsigned)drone_get_patch());
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
        case DROW_SOURCE:
            if (dir != 0)
                drone_set_source(drone_get_source() == DRONE_SRC_WAVE
                                 ? DRONE_SRC_PATCH : DRONE_SRC_WAVE);
            break;
        case DROW_WAVE: {
            /* NOISE and KS are excluded: their excitation model misbehaves
             * with the drone's one-shot trigger style. */
            static const uint16_t waves[] = { SAW_DOWN, SAW_UP, PULSE, TRIANGLE, SINE };
            const int wn = (int)(sizeof(waves) / sizeof(waves[0]));
            int idx = 0;
            for (int i = 0; i < wn; i++) if (waves[i] == drone_get_wave()) { idx = i; break; }
            idx = (idx + dir + wn) % wn;
            drone_set_wave(waves[idx]);
            break;
        }
        case DROW_ROOT: {
            int rv = SEQ_CLAMP_INT((int)drone_get_root_note() + dir, 24, 72);
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
        case DROW_RES: {
            /* Geometric step: Q is perceptually multiplicative, so a linear
             * step is too coarse low and too fine high. ~15% per detent feels
             * even across the whole span. */
            float rv = drone_get_resonance();
            rv *= (dir > 0) ? 1.15f : (1.0f / 1.15f);
            drone_set_resonance(rv);
            break;
        }
        case DROW_CONST:
            drone_set_amp_peak(drone_get_amp_peak() + (float)dir * 0.1f);
            break;
        case DROW_MOD:
            drone_set_amp_duck(drone_get_amp_duck() + (float)dir * 0.1f);
            break;
        case DROW_VIS_POPUP:
            break; /* encoder turns consumed; popup opened/closed via button */
        case DROW_RATE: {
            int v = SEQ_CLAMP((int)drone_get_rate() + dir, 0, DRONE_RATE_COUNT - 1);
            drone_set_rate((drone_rate_t)v);
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
        case DROW_SWEEP_LO:
            drone_set_sweep_lo(drone_get_sweep_lo() + (float)dir * 25.0f);
            break;
        case DROW_SWEEP_HI:
            drone_set_sweep_hi(drone_get_sweep_hi() + (float)dir * 25.0f);
            break;
        case DROW_SWEEP_BARS:
            drone_set_sweep_bars((uint8_t)SEQ_CLAMP(
                (int)drone_get_sweep_bars() + dir, 1, 16));
            break;
        case DROW_SUB:
            if (dir != 0) drone_set_sub_enabled(!drone_get_sub_enabled());
            break;
        case DROW_SUB_INTVL:
            drone_set_sub_interval((int8_t)SEQ_CLAMP(
                (int)drone_get_sub_interval() + dir, -36, 0));
            break;
        case DROW_PATCH:
            synth_ui_drone_cycle_patch(dir);
            break;
        default:
            break;
    }
}

/* View-resolved: true only while this screen (or its visualiser page) is what
 * the display shows, so the filter/LFO overlays keep their own buttons. */
bool synth_ui_drone_is_active(void)
{
    ui_view_id_t v = synth_ui_active_view();
    return v == UI_VIEW_DRONE || v == UI_VIEW_DRONE_VIS;
}

void synth_ui_drone_handle_encoder(long delta)
{
    if (delta == 0) return;
    if (s_drone_vis_open) {
        s_force_redraw = true; /* consume turn; popup has nothing to scroll */
        return;
    }
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
    if (s_drone_vis_open) {
        s_drone_vis_open = false;
        s_force_redraw = true;
        return;
    }
    drone_logical_row_t vis[DROW_ALL_COUNT];
    uint8_t n = drone_visible_rows(vis);
    if (n > 0 && s_drone_cursor < n && vis[s_drone_cursor] == DROW_VIS_POPUP) {
        s_drone_vis_open = true;
        s_force_redraw = true;
        return;
    }
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
