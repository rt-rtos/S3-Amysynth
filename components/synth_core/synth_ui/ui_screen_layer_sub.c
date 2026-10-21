#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "voice_config.h"
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
 * voice. Unison toggles On/Off with one detent (the spec's bypass); every
 * other row keeps its value and stays editable while it is Off. Count runs 1
 * up to the voice's ceiling (vt_unison_max), "n/a" and not editable when its
 * patch cannot carry unison. A stored count above a since-shrunk ceiling is
 * shown as stored; the next turn clamps it. Detune is cents (1 per detent).
 * Stack picks the notes the copies are dealt over (voice_unison_stack_t:
 * Off, Oct, Sub, Oct+Sub, 5th, Power); Count then steps in multiples of the
 * stack's group count and snaps down to one when the stack changes, so every
 * note gets the same copies. Shift slides each group from -100 % (all copies
 * flat) to +100 % (all sharp), 1 % per detent. Stereo toggles On/Off with one detent; while Off,
 * Spread reads "n/a" and does not edit (its value is kept). On a layer, LAYER
 * patch scope writes every track (the title names the layer), TRACK scope the
 * one track (the title names it too).
 *
 * PCM (drum layer, PCM engine): the track's playback mode
 * (sequencer_core_set_drum_pcm_mode). */

/* ── Unison page ──────────────────────────────────────────────────────── */

typedef enum {
    LU_ENABLE = 0,
    LU_COUNT,
    LU_DETUNE,
    LU_STACK,
    LU_SHIFT,
    LU_SPREAD,
    LU_BLEND,
    LU_STEREO,
    LU_BACK,
    LU_ROWS
} layer_uni_item_id_t;

static menu_item_view_t s_lu_items[LU_ROWS];

/* Indexed by voice_unison_stack_t. */
static const char *const UNISON_STACK_NAMES[VOICE_UNISON_STACK_COUNT] = {
    "Off", "Oct", "Sub", "Oct+Sub", "5th", "Power", "2xOct", "4th", "Major", "Minor", "Harm3", "Harm4"
};

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
        "Unison", "Count", "Detune", "Stack", "Shift", "Spread", "Blend", "Stereo",
        "< Back"
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
        case LU_ENABLE:
            snprintf(it->value, MENU_VALUE_LEN, "%s", u.bypass ? "Off" : "On");
            break;
        case LU_COUNT:
            if (vt_unison_max(kind) == 0u)
                snprintf(it->value, MENU_VALUE_LEN, "n/a");
            else
                snprintf(it->value, MENU_VALUE_LEN, "%u", (unsigned)u.count);
            break;
        case LU_DETUNE:
            snprintf(it->value, MENU_VALUE_LEN, "%uc", (unsigned)u.detune_cents);
            break;
        case LU_STACK:
            snprintf(it->value, MENU_VALUE_LEN, "%s",
                     UNISON_STACK_NAMES[u.stack < VOICE_UNISON_STACK_COUNT ? u.stack : 0u]);
            break;
        case LU_SHIFT:
            snprintf(it->value, MENU_VALUE_LEN, "%+d%%", (int)u.shift - 100);
            break;
        case LU_SPREAD:
            if (!u.stereo)
                snprintf(it->value, MENU_VALUE_LEN, "n/a");
            else
                snprintf(it->value, MENU_VALUE_LEN, "%u%%", (unsigned)u.spread_pct);
            break;
        case LU_STEREO:
            snprintf(it->value, MENU_VALUE_LEN, "%s", u.stereo ? "On" : "Off");
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
    if (idx == LU_SPREAD) {
        voice_unison_t u;
        vt_get_unison(kind, &u);
        if (!u.stereo) return false;
    }
    return !seq_state.menu_editing;
}

void layer_uni_menu_edit_value(uint8_t idx, int delta)
{
    voice_target_kind_t kind = layermenu_menu_kind();
    if (!vt_has_unison(kind) || delta == 0) return;
    voice_unison_t u;
    vt_get_unison(kind, &u);
    switch (idx) {
    case LU_ENABLE:
        u.bypass = u.bypass ? 0u : 1u;
        break;
    case LU_COUNT: {
        int max = (int)vt_unison_max(kind);
        if (max == 0) return;
        int g   = (int)voice_unison_groups(u.stack);
        int dir = (delta > 0) ? 1 : -1;
        int c   = ((int)u.count / g + dir) * g;
        if (c > max) c = (max / g) * g;
        if (c < g) c = g;
        if (c > max) return;            /* the ceiling is below one copy per note */
        u.count = (uint8_t)c;
        break;
    }
    case LU_DETUNE:
        u.detune_cents = (uint8_t)SEQ_CLAMP_INT((int)u.detune_cents + delta,
                                                0, (int)VOICE_UNISON_MAX_DETUNE);
        break;
    case LU_STACK: {
        u.stack = (uint8_t)SEQ_CLAMP_INT((int)u.stack + delta,
                                         0, (int)VOICE_UNISON_MAX_STACK);
        /* Count snaps down to a multiple of the new group count, at least
         * one copy per note, within the voice's ceiling. */
        int max = (int)vt_unison_max(kind);
        int g   = (int)voice_unison_groups(u.stack);
        int c   = ((int)u.count / g) * g;
        if (c < g) c = g;
        if (max >= g) {
            if (c > max) c = (max / g) * g;
            u.count = (uint8_t)c;
        }
        break;
    }
    case LU_SHIFT:
        u.shift = (uint8_t)SEQ_CLAMP_INT((int)u.shift + delta,
                                         0, (int)VOICE_UNISON_MAX_SHIFT);
        break;
    case LU_SPREAD:
        if (!u.stereo) return;
        u.spread_pct = (uint8_t)SEQ_CLAMP_INT((int)u.spread_pct + delta, 0, 100);
        break;
    case LU_BLEND:
        u.blend_pct = (uint8_t)SEQ_CLAMP_INT((int)u.blend_pct + delta, 0, 100);
        break;
    case LU_STEREO:
        u.stereo = u.stereo ? 0u : 1u;
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
