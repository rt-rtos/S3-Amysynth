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
 * Unison (semantics in sequencer_core.h, "Melodic per-track unison"): the
 * voice of the Layer page's entry (layermenu_menu_kind), read and written
 * through ui_voice_target.h - a melodic layer's track, the arp or the live
 * voice. Count is Off or 2 up to the voice's ceiling (vt_unison_max), "n/a"
 * and not editable when its patch cannot carry unison. A stored count above a
 * since-shrunk ceiling is shown as stored; the next turn clamps it. On a
 * layer, LAYER patch scope writes every track (the title names the layer),
 * TRACK scope the one track (the title names it too).
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

const char *layer_uni_menu_title(void)
{
    static char s_title[20];
    uint8_t li = seq_state.active_layer_idx;
    const char *vt = vt_title(layermenu_menu_kind());
    if (vt)
        snprintf(s_title, sizeof(s_title), "UNISON %s", vt);
    else if (sequencer_core_get_patch_scope(li) == SEQ_PATCH_SCOPE_TRACK)
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
    voice_target_kind_t kind = layermenu_menu_kind();
    bool has = vt_has_unison(kind);
    voice_unison_t u;
    vt_get_unison(kind, &u);
    for (uint8_t i = 0; i < LU_ROWS; i++) {
        menu_item_view_t *it = &s_lu_items[i];
        snprintf(it->label, MENU_LABEL_LEN, "%s", labels[i]);
        it->value[0] = '\0';
        if (i == LU_BACK) continue;
        if (!has) { snprintf(it->value, MENU_VALUE_LEN, "--"); continue; }
        switch (i) {
        case LU_COUNT:
            if (vt_unison_max(kind) == 0u)
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
    voice_target_kind_t kind = layermenu_menu_kind();
    if (!vt_has_unison(kind) || idx >= LU_BACK) return false;
    if (idx == LU_COUNT && vt_unison_max(kind) == 0u)
        return false;
    return !seq_state.menu_editing;
}

void layer_uni_menu_edit_value(uint8_t idx, int delta)
{
    voice_target_kind_t kind = layermenu_menu_kind();
    if (!vt_has_unison(kind) || delta == 0) return;
    voice_unison_t u;
    vt_get_unison(kind, &u);
    switch (idx) {
    case LU_COUNT: {
        /* Off is count 1, so one detent up from Off is 2 and one down from 2
         * is Off. */
        int max = (int)vt_unison_max(kind);
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
    vt_set_unison(kind, &u);
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
