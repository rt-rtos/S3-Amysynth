#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "seq_clamp.h"
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════════
 *  LAYER SUB-PAGES: UNISON AND PCM
 * ════════════════════════════════════════════════════════════════════════
 * Dived into from the Layer page's `Unison >` / `PCM >` rows; page state and
 * input routing live in ui_screen_menu.c, this file only builds the rows and
 * applies clicks and encoder edits. Both pages edit the active layer at the
 * Layer page's track (layermenu_menu_track), the same target as Mute/Solo, so
 * there are no selector rows.
 *
 * Unison (melodic layers; semantics in sequencer_core.h, "Melodic per-track
 * unison"): Count is Off or 2 up to the track's ceiling
 * (sequencer_core_track_unison_max), "n/a" and not editable when the track's
 * patch cannot carry unison. A stored count above a since-shrunk ceiling is
 * shown as stored; the next turn clamps it. LAYER patch scope writes every
 * track (the title names the layer), TRACK scope the one track (the title
 * names it too).
 *
 * PCM (drum layer, PCM engine): the track's playback mode
 * (sequencer_core_set_drum_pcm_mode). */

/* ── Unison page ──────────────────────────────────────────────────────── */

typedef enum {
    LU_COUNT = 0,
    LU_DETUNE,
    LU_SPREAD,
    LU_BLEND,
    LU_BACK,
    LU_ROWS
} layer_uni_item_id_t;

static menu_item_view_t s_lu_items[LU_ROWS];

/* Active layer index if it is melodic, else 0xFF. */
static uint8_t lu_layer(void)
{
    uint8_t li = seq_state.active_layer_idx;
    if (li >= seq_state.num_layers) return 0xFF;
    if (seq_state.layers[li].type != SEQ_LAYER_MELODIC) return 0xFF;
    return li;
}

const char *layer_uni_menu_title(void)
{
    static char s_title[20];
    uint8_t li = seq_state.active_layer_idx;
    if (sequencer_core_get_patch_scope(li) == SEQ_PATCH_SCOPE_TRACK)
        snprintf(s_title, sizeof(s_title), "UNISON L%u T%u", (unsigned)(li + 1u),
                 (unsigned)(layermenu_menu_track() + 1u));
    else
        snprintf(s_title, sizeof(s_title), "UNISON L%u", (unsigned)(li + 1u));
    return s_title;
}

uint8_t layer_uni_menu_item_count(void)
{
    return LU_ROWS;
}

const menu_item_view_t *layer_uni_menu_build_items(void)
{
    static const char *const labels[LU_ROWS] = {
        "Count", "Detune", "Spread", "Blend", "< Back"
    };
    uint8_t li = lu_layer();
    uint8_t tr = layermenu_menu_track();
    voice_unison_t u = sequencer_core_get_track_unison(li, tr);
    for (uint8_t i = 0; i < LU_ROWS; i++) {
        menu_item_view_t *it = &s_lu_items[i];
        snprintf(it->label, MENU_LABEL_LEN, "%s", labels[i]);
        it->value[0] = '\0';
        if (i == LU_BACK) continue;
        if (li == 0xFF) { snprintf(it->value, MENU_VALUE_LEN, "--"); continue; }
        switch (i) {
        case LU_COUNT:
            if (sequencer_core_track_unison_max(li, tr) == 0u)
                snprintf(it->value, MENU_VALUE_LEN, "n/a");
            else if (u.count <= 1u)
                snprintf(it->value, MENU_VALUE_LEN, "Off");
            else
                snprintf(it->value, MENU_VALUE_LEN, "%u", (unsigned)u.count);
            break;
        case LU_DETUNE:
            snprintf(it->value, MENU_VALUE_LEN, "%uc", (unsigned)u.detune_cents);
            break;
        case LU_SPREAD:
            snprintf(it->value, MENU_VALUE_LEN, "%u%%", (unsigned)u.spread_pct);
            break;
        case LU_BLEND:
        default:
            snprintf(it->value, MENU_VALUE_LEN, "%u%%", (unsigned)u.blend_pct);
            break;
        }
    }
    return s_lu_items;
}

bool layer_uni_menu_item_is_back(uint8_t idx)
{
    return idx == LU_BACK;
}

bool layer_uni_menu_handle_click(uint8_t idx)
{
    uint8_t li = lu_layer();
    if (li == 0xFF || idx >= LU_BACK) return false;
    if (idx == LU_COUNT &&
        sequencer_core_track_unison_max(li, layermenu_menu_track()) == 0u)
        return false;
    return !seq_state.menu_editing;
}

void layer_uni_menu_edit_value(uint8_t idx, int delta)
{
    uint8_t li = lu_layer();
    if (li == 0xFF || delta == 0) return;
    uint8_t tr = layermenu_menu_track();
    voice_unison_t u = sequencer_core_get_track_unison(li, tr);
    switch (idx) {
    case LU_COUNT: {
        /* Off is count 1, so one detent up from Off is 2 and one down from 2
         * is Off. */
        int max = (int)sequencer_core_track_unison_max(li, tr);
        if (max == 0) return;
        int dir = (delta > 0) ? 1 : -1;
        u.count = (uint8_t)SEQ_CLAMP_INT((int)u.count + dir, 1, max);
        break;
    }
    case LU_DETUNE:
        u.detune_cents = (uint8_t)SEQ_CLAMP_INT((int)u.detune_cents + delta,
                                                0, (int)VOICE_UNISON_MAX_DETUNE);
        break;
    case LU_SPREAD:
        u.spread_pct = (uint8_t)SEQ_CLAMP_INT((int)u.spread_pct + delta, 0, 100);
        break;
    case LU_BLEND:
        u.blend_pct = (uint8_t)SEQ_CLAMP_INT((int)u.blend_pct + delta, 0, 100);
        break;
    default:
        return;
    }
    if (sequencer_core_get_patch_scope(li) == SEQ_PATCH_SCOPE_TRACK)
        sequencer_core_set_track_unison(li, tr, &u);
    else
        sequencer_core_set_layer_unison(li, &u);
}

/* ── PCM page ─────────────────────────────────────────────────────────── */

typedef enum {
    LP_MODE = 0,
    LP_BACK,
    LP_ROWS
} layer_pcm_item_id_t;

static menu_item_view_t s_lp_items[LP_ROWS];

/* Indexed by the stored mode (sequencer_core.h, "Drum per-track PCM playback
 * mode"); 0 = engine default. */
static const char *const PCM_MODE_NAMES[] = { "DFLT", "PLAY", "LOOP", "LOOPST", "FRVR" };
#define PCM_MODE_COUNT 5

const char *layer_pcm_menu_title(void)
{
    static char s_title[20];
    snprintf(s_title, sizeof(s_title), "PCM L%u T%u",
             (unsigned)(seq_state.active_layer_idx + 1u),
             (unsigned)(layermenu_menu_track() + 1u));
    return s_title;
}

uint8_t layer_pcm_menu_item_count(void)
{
    return LP_ROWS;
}

const menu_item_view_t *layer_pcm_menu_build_items(void)
{
    uint8_t m = sequencer_core_get_drum_pcm_mode(seq_state.active_layer_idx,
                                                 layermenu_menu_track());
    if (m >= PCM_MODE_COUNT) m = 0;
    snprintf(s_lp_items[LP_MODE].label, MENU_LABEL_LEN, "Mode");
    snprintf(s_lp_items[LP_MODE].value, MENU_VALUE_LEN, "%s", PCM_MODE_NAMES[m]);
    snprintf(s_lp_items[LP_BACK].label, MENU_LABEL_LEN, "< Back");
    s_lp_items[LP_BACK].value[0] = '\0';
    return s_lp_items;
}

bool layer_pcm_menu_item_is_back(uint8_t idx)
{
    return idx == LP_BACK;
}

bool layer_pcm_menu_handle_click(uint8_t idx)
{
    if (idx != LP_MODE) return false;
    return !seq_state.menu_editing;
}

void layer_pcm_menu_edit_value(uint8_t idx, int delta)
{
    if (idx != LP_MODE || delta == 0) return;
    uint8_t li = seq_state.active_layer_idx;
    uint8_t tr = layermenu_menu_track();
    int m = ((int)sequencer_core_get_drum_pcm_mode(li, tr) + (delta > 0 ? 1 : -1))
            % PCM_MODE_COUNT;
    if (m < 0) m += PCM_MODE_COUNT;
    sequencer_core_set_drum_pcm_mode(li, tr, (uint8_t)m);
}
