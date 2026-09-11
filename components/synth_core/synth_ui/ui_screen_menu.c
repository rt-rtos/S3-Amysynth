#include "sdkconfig.h"
#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"
#include "custompatches/sample_rec.h"
#include "custompatches/clip_player.h"
#include "quantizer.h"
#include "amy_fx.h"
#include "seq_clamp.h"
#if CONFIG_SYNTH_PROJECT_STORE
#include "project_fs.h"
#endif
#if CONFIG_SYNTH_WIRELESS
#include "radio_manager.h"
#endif
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "synth_ui";

/* ════════════════════════════════════════════════════════════════════════
 *  MENU OVERLAY
 * ════════════════════════════════════════════════════════════════════════
 * A small modal list. Items are either ACTIONS (run on click) or VALUE items
 * (click to enter editing, encoder changes the value, click to exit). The model
 * is a static table; values are read/written live from sequencer_core, arp_core
 * and the per-bus FX caches.
 *
 * This file owns the page state (main list plus the sub-pages dived into from
 * it) and routes encoder/button input to the active page; every page shares
 * seq_state.menu_cursor/menu_editing. */

typedef enum {
#if CONFIG_SYNTH_DEV_MENU
    MI_DEV,               /* first row: DEV menu (temporary controls) */
#endif
    MI_SCREEN_SEQ,
    MI_SCREEN_ARP,
    MI_SCREEN_DRONE,
    MI_SCREEN_PROG,
    MI_SCREEN_TRACKOPTS,
#if CONFIG_SYNTH_CUSTOM_FM
    MI_SCREEN_FM,
#endif
    MI_BPM,
    MI_QUANT_ENABLED,
    MI_QUANT_SCALE,
    MI_QUANT_ROOT,
    MI_ARP_QUANT,         /* arp scale source: own scale vs global quantizer */
    MI_ARP_ENABLED,
    MI_DRONE_ENABLED,     /* normal drone (drone_std_core) */
    MI_STUTTER_ENABLED,   /* stutter drone (drone_core)    */
    MI_DRUM_ENGINE,
    MI_ADD_LAYER,
    MI_REMOVE_LAYER,
    MI_CHORDS,
    MI_BOUNCE,
    MI_PROGGEN,
    MI_FX_MENU,
#if CONFIG_SYNTH_PROJECT_STORE
    MI_PROJECTS,
#endif
#if CONFIG_SYNTH_WIRELESS
    MI_WIRELESS,
#endif
    MI_VOLUME,
    MI_SAMPLE,
    MI_SAMPLE_CANCEL,
    MI_COUNT
} menu_item_id_t;

static menu_item_view_t s_menu_items[MI_COUNT];

/* Page state: false = main list, true = the FX hub (one row per AMY bus). The
 * main-page cursor is parked here while the hub is open so Back restores it. */
static bool    s_fx_page = false;
static uint8_t s_main_cursor = 0;
/* NoteFX is a sub-page of the hub (per-layer gate/glide). The hub cursor is
 * parked here while it is open so Back restores it. */
static bool    s_notefx_page = false;
static uint8_t s_fx_cursor = 0;
/* The per-bus FX page is the hub's other sub-page; it parks the hub cursor
 * separately so a Back from either sub-page lands on the row it came from. */
static bool    s_fxbus_page = false;
static uint8_t s_fxhub_cursor = 0;
#if CONFIG_SYNTH_PROJECT_STORE
static bool    s_projects_page = false;
#endif
/* Chord-preset editor page (item model in ui_screen_chords.c). */
static bool    s_chords_page = false;
/* Loop-bounce page (item model in ui_screen_bounce.c). */
static bool    s_bounce_page = false;
/* Progression-generator page (item model in ui_screen_proggen.c). */
static bool    s_proggen_page = false;
#if CONFIG_SYNTH_WIRELESS
/* BLE MIDI session page (item model in ui_screen_wireless.c). */
static bool    s_wireless_page = false;
#endif

/* Title for the menu overlay's header bar (drawn by ui_view_resolve.c). */
const char *menu_page_title(void)
{
    if (s_notefx_page) {
        /* NoteFX is bound to the ACTIVE layer, so the title names it - always,
         * even at "L1/1", so it stays discoverable that each layer has its own
         * Gate/Glide values. */
        static char s_nfx_title[20];
        snprintf(s_nfx_title, sizeof(s_nfx_title), "NOTE FX  L%u/%u",
                 (unsigned)(seq_state.active_layer_idx + 1u),
                 (unsigned)seq_state.num_layers);
        return s_nfx_title;
    }
    if (s_fxbus_page) return fx_menu_title();
    if (s_fx_page) return "GLOBAL FX";
#if CONFIG_SYNTH_PROJECT_STORE
    if (s_projects_page) return "PROJECTS";
#endif
    if (s_chords_page) return chords_menu_title();
    if (s_bounce_page) return bounce_menu_title();
    if (s_proggen_page) return proggen_menu_title();
#if CONFIG_SYNTH_WIRELESS
    if (s_wireless_page) return "WIRELESS";
#endif
    return "MENU";
}

/* True while the overlay is showing the Bounce page; the page's redraw pump
 * runs only then (see bounce_menu_service). */
bool menu_bounce_page_open(void)
{
    return seq_state.menu_open && s_bounce_page;
}

/* True while the overlay is showing its Wireless page. The editors bind to the
 * live-play voice on this, since the page is not a ui_mode. It stays true while
 * an editor draws over the overlay (the menu is still open underneath), which
 * is what makes the filter tab and the editor cycle route to the same target as
 * the ADSR tab. Defined unconditionally so callers need no build-time guard. */
bool synth_ui_wireless_page_is_open(void)
{
#if CONFIG_SYNTH_WIRELESS
    return seq_state.menu_open && s_wireless_page;
#else
    return false;
#endif
}

/* Format the current value of each menu item into the flat view array. */
void menu_build_view(menu_view_t *out)
{
    if (s_notefx_page) {
        out->items   = notefx_menu_build_items();
        out->count   = notefx_menu_item_count();
        out->cursor  = seq_state.menu_cursor;
        out->editing = seq_state.menu_editing;
        return;
    }
    if (s_fxbus_page) {
        out->items   = fx_menu_build_items();
        out->count   = fx_menu_item_count();
        out->cursor  = seq_state.menu_cursor;
        out->editing = seq_state.menu_editing;
        return;
    }
    if (s_fx_page) {
        out->items   = fxhub_build_items();
        out->count   = fxhub_item_count();
        out->cursor  = seq_state.menu_cursor;
        out->editing = seq_state.menu_editing;
        return;
    }
#if CONFIG_SYNTH_PROJECT_STORE
    if (s_projects_page) {
        out->items   = projects_menu_build_items();
        out->count   = projects_menu_item_count();
        out->cursor  = seq_state.menu_cursor;
        out->editing = seq_state.menu_editing;
        return;
    }
#endif
    if (s_chords_page) {
        out->items   = chords_menu_build_items();
        out->count   = chords_menu_item_count();
        out->cursor  = seq_state.menu_cursor;
        out->editing = seq_state.menu_editing;
        return;
    }
    if (s_bounce_page) {
        out->items   = bounce_menu_build_items();
        out->count   = bounce_menu_item_count();
        out->cursor  = seq_state.menu_cursor;
        out->editing = seq_state.menu_editing;
        return;
    }
    if (s_proggen_page) {
        out->items   = proggen_menu_build_items();
        out->count   = proggen_menu_item_count();
        out->cursor  = seq_state.menu_cursor;
        out->editing = seq_state.menu_editing;
        return;
    }
#if CONFIG_SYNTH_WIRELESS
    if (s_wireless_page) {
        out->items   = wireless_menu_build_items();
        out->count   = wireless_menu_item_count();
        out->cursor  = seq_state.menu_cursor;
        out->editing = seq_state.menu_editing;
        return;
    }
#endif

    for (int i = 0; i < MI_COUNT; i++) {
        s_menu_items[i].value[0] = '\0';
    }

#if CONFIG_SYNTH_DEV_MENU
    snprintf(s_menu_items[MI_DEV].label, MENU_LABEL_LEN, "DEV");
#endif
    snprintf(s_menu_items[MI_SCREEN_SEQ].label, MENU_LABEL_LEN, "Screen: Seq");
    snprintf(s_menu_items[MI_SCREEN_ARP].label, MENU_LABEL_LEN, "Screen: Arp");
    snprintf(s_menu_items[MI_SCREEN_DRONE].label, MENU_LABEL_LEN, "Screen: Drone");
    snprintf(s_menu_items[MI_SCREEN_PROG].label, MENU_LABEL_LEN, "Screen: Prog");
    snprintf(s_menu_items[MI_SCREEN_TRACKOPTS].label, MENU_LABEL_LEN, "Screen: TrackOpts");
#if CONFIG_SYNTH_CUSTOM_FM
    snprintf(s_menu_items[MI_SCREEN_FM].label, MENU_LABEL_LEN, "Screen: FM");
#endif

    snprintf(s_menu_items[MI_BPM].label, MENU_LABEL_LEN, "BPM");
    snprintf(s_menu_items[MI_BPM].value, MENU_VALUE_LEN, "%u",
             (unsigned)seq_get_bpm());

    snprintf(s_menu_items[MI_QUANT_ENABLED].label, MENU_LABEL_LEN, "Quant");
    snprintf(s_menu_items[MI_QUANT_ENABLED].value, MENU_VALUE_LEN, "%s",
             sequencer_core_get_quantizer_enabled() ? "ON" : "OFF");

    snprintf(s_menu_items[MI_QUANT_SCALE].label, MENU_LABEL_LEN, "Scale");
    {
        const musical_scale_t *sc =
            quantizer_get_scale(sequencer_core_get_quantizer_scale());
        snprintf(s_menu_items[MI_QUANT_SCALE].value, MENU_VALUE_LEN, "%s",
                 sc ? sc->name : "?");
    }

    snprintf(s_menu_items[MI_QUANT_ROOT].label, MENU_LABEL_LEN, "Root");
    /* Pitch class only: the snapper searches all octaves relative to the root,
     * so showing "C4" would imply a choice that does nothing. */
    snprintf(s_menu_items[MI_QUANT_ROOT].value, MENU_VALUE_LEN, "%s",
             chord_root_name(sequencer_core_get_quantizer_root_note() % 12));

    /* GLOB = arp snaps to the Quant/Scale/Root above (chromatic while Quant is
     * OFF; progression chords still win). OWN = the arp's private scale. */
    snprintf(s_menu_items[MI_ARP_QUANT].label, MENU_LABEL_LEN, "ArpQ");
    snprintf(s_menu_items[MI_ARP_QUANT].value, MENU_VALUE_LEN, "%s",
             arp_get_follow_quant() ? "GLOB" : "OWN");

    snprintf(s_menu_items[MI_ARP_ENABLED].label, MENU_LABEL_LEN, "Arp");
    snprintf(s_menu_items[MI_ARP_ENABLED].value, MENU_VALUE_LEN, "%s",
             arp_get_enabled() ? "ON" : "OFF");

    snprintf(s_menu_items[MI_DRONE_ENABLED].label, MENU_LABEL_LEN, "Drone");
    snprintf(s_menu_items[MI_DRONE_ENABLED].value, MENU_VALUE_LEN, "%s",
             drone_std_get_enabled() ? "ON" : "OFF");

    snprintf(s_menu_items[MI_STUTTER_ENABLED].label, MENU_LABEL_LEN, "Stutter");
    snprintf(s_menu_items[MI_STUTTER_ENABLED].value, MENU_VALUE_LEN, "%s",
             drone_get_enabled() ? "ON" : "OFF");

    snprintf(s_menu_items[MI_DRUM_ENGINE].label, MENU_LABEL_LEN, "Drum Bank");
    snprintf(s_menu_items[MI_DRUM_ENGINE].value, MENU_VALUE_LEN, "%s",
             sequencer_core_drum_source_name(sequencer_core_get_drum_source()));

    snprintf(s_menu_items[MI_ADD_LAYER].label, MENU_LABEL_LEN, "Add Layer");
    snprintf(s_menu_items[MI_ADD_LAYER].value, MENU_VALUE_LEN, "%u/%u",
             (unsigned)seq_state.num_layers, (unsigned)MAX_LAYERS);

    {
        uint8_t li = seq_state.active_layer_idx;
        bool can_del = (li > 0 && seq_state.num_layers > 1);
        snprintf(s_menu_items[MI_REMOVE_LAYER].label, MENU_LABEL_LEN, "Del Layer");
        if (can_del)
            snprintf(s_menu_items[MI_REMOVE_LAYER].value, MENU_VALUE_LEN,
                     "L%u", (unsigned)(li + 1));
        else
            snprintf(s_menu_items[MI_REMOVE_LAYER].value, MENU_VALUE_LEN, "--");
    }

    /* Chord presets live on their own page (ui_screen_chords.c). */
    snprintf(s_menu_items[MI_CHORDS].label, MENU_LABEL_LEN, "Chords");
    snprintf(s_menu_items[MI_CHORDS].value, MENU_VALUE_LEN, "%u/%u",
             (unsigned)seq_chords_defined_count(), (unsigned)SEQ_CHORD_SLOTS);

    /* Loop bounce and the clip players live on their own page
     * (ui_screen_bounce.c); the row counts the slots holding a clip. */
    {
        unsigned loaded = 0;
        for (uint8_t s = 0; s < CLIP_SLOT_COUNT; s++) {
            if (clip_player_slot_state(s) == CLIP_SLOT_LOADED) loaded++;
        }
        snprintf(s_menu_items[MI_BOUNCE].label, MENU_LABEL_LEN, "Bounce");
        snprintf(s_menu_items[MI_BOUNCE].value, MENU_VALUE_LEN, "%u/%u",
                 loaded, (unsigned)CLIP_SLOT_COUNT);
    }

    /* The progression generator lives on its own page (ui_screen_proggen.c). */
    snprintf(s_menu_items[MI_PROGGEN].label, MENU_LABEL_LEN, "Prog Gen");
    snprintf(s_menu_items[MI_PROGGEN].value, MENU_VALUE_LEN, ">");

    /* FX live on their own hub page, one row per bus (ui_screen_fxmenu.c). */
    snprintf(s_menu_items[MI_FX_MENU].label, MENU_LABEL_LEN, "FX");
    snprintf(s_menu_items[MI_FX_MENU].value, MENU_VALUE_LEN, ">");

#if CONFIG_SYNTH_PROJECT_STORE
    /* Persistent project storage lives on its own page (ui_screen_projects.c). */
    snprintf(s_menu_items[MI_PROJECTS].label, MENU_LABEL_LEN, "Projects");
    snprintf(s_menu_items[MI_PROJECTS].value, MENU_VALUE_LEN, "%s",
             project_fs_ok() ? ">" : "ERR");
#endif

#if CONFIG_SYNTH_WIRELESS
    /* BLE MIDI session lives on its own page (ui_screen_wireless.c). */
    snprintf(s_menu_items[MI_WIRELESS].label, MENU_LABEL_LEN, "Wireless");
    snprintf(s_menu_items[MI_WIRELESS].value, MENU_VALUE_LEN, "%s",
             radio_manager_state() == RADIO_ACTIVE ? "ON" : ">");
#endif

    /* Master output volume (0..200%, unity 100%), written to amy_global.volume[]. */
    snprintf(s_menu_items[MI_VOLUME].label, MENU_LABEL_LEN, "Volume");
    snprintf(s_menu_items[MI_VOLUME].value, MENU_VALUE_LEN, "%.0f%%",
             (double)(amy_fx_get_master_volume() * 100.0f));

    /* Runtime PCM sampler (custompatches/sample_rec): the label previews what
     * ARM will target, since the track selection is snapshotted only at the
     * moment the user arms. */
    snprintf(s_menu_items[MI_SAMPLE].label, MENU_LABEL_LEN, "Sample");
    switch (sample_rec_get_state()) {
        case SAMPLE_REC_ARMED:
            snprintf(s_menu_items[MI_SAMPLE].value, MENU_VALUE_LEN, "Rec!");
            break;
        case SAMPLE_REC_RECORDING:
            snprintf(s_menu_items[MI_SAMPLE].value, MENU_VALUE_LEN, "Rec %u%%",
                     (unsigned)sample_rec_get_progress_pct());
            break;
        case SAMPLE_REC_READY:
            snprintf(s_menu_items[MI_SAMPLE].value, MENU_VALUE_LEN, "Assign?");
            break;
        case SAMPLE_REC_IDLE:
        default:
            snprintf(s_menu_items[MI_SAMPLE].value, MENU_VALUE_LEN, "Arm T%u",
                     (unsigned)(seq_state.selected_track + 1));
            break;
    }
    snprintf(s_menu_items[MI_SAMPLE_CANCEL].label, MENU_LABEL_LEN, "Smp Cancel");

    out->items   = s_menu_items;
    out->count   = MI_COUNT;
    out->cursor  = seq_state.menu_cursor;
    out->editing = seq_state.menu_editing;
}

/* Signature of the menu overlay. Builds the view into *out so the caller draws
 * from it without a second full snprintf pass. */
uint32_t menu_view_signature(menu_view_t *out)
{
    uint32_t h = FNV1A_OFFSET;
    menu_build_view(out);
    h = fnv1a_bytes(h, &out->cursor, sizeof(out->cursor));
    h = fnv1a_bytes(h, &out->editing, sizeof(out->editing));
    for (uint8_t i = 0; i < out->count; i++) {
        h = fnv1a_bytes(h, out->items[i].label, sizeof(out->items[i].label));
        h = fnv1a_bytes(h, out->items[i].value, sizeof(out->items[i].value));
    }
    return h;
}

/* True for items that hold an adjustable value (vs. one-shot actions). */
static bool menu_item_is_value(menu_item_id_t id)
{
    switch (id) {
        case MI_BPM:
        case MI_QUANT_ENABLED:
        case MI_QUANT_SCALE:
        case MI_QUANT_ROOT:
        case MI_ARP_QUANT:
        case MI_ARP_ENABLED:
        case MI_DRONE_ENABLED:
        case MI_STUTTER_ENABLED:
        case MI_DRUM_ENGINE:
        case MI_VOLUME:
            return true;
        default:
            return false;
    }
}

/* Apply an encoder delta to the currently-entered menu value. */
static void menu_edit_value(menu_item_id_t id, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    switch (id) {
        case MI_BPM:
            synth_ui_set_bpm((uint16_t)((int)seq_get_bpm() + delta));
            break;
        case MI_QUANT_ENABLED:
            if (dir != 0)
                sequencer_core_set_quantizer_enabled(
                    !sequencer_core_get_quantizer_enabled());
            break;
        case MI_QUANT_SCALE: {
            int n = (int)quantizer_scale_count();
            int cur = (int)sequencer_core_get_quantizer_scale();
            int ni = (cur + dir + n) % n;
            sequencer_core_set_quantizer_scale((uint8_t)ni);
            break;
        }
        case MI_QUANT_ROOT: {
            /* Cycle the 12 pitch classes - only the class matters to the
             * snapper - but store as a MIDI note in octave 4 (60..71) so
             * snapshots and the Kconfig default keep their meaning. */
            int pc = ((int)sequencer_core_get_quantizer_root_note() % 12 + delta) % 12;
            if (pc < 0) pc += 12;
            sequencer_core_set_quantizer_root_note((uint8_t)(60 + pc));
            break;
        }
        case MI_ARP_QUANT:
            if (dir != 0) arp_set_follow_quant(!arp_get_follow_quant());
            break;
        case MI_ARP_ENABLED:
            if (dir != 0) arp_set_enabled(!arp_get_enabled());
            break;
        case MI_DRONE_ENABLED:
            if (dir != 0) drone_std_set_enabled(!drone_std_get_enabled());
            break;
        case MI_STUTTER_ENABLED:
            if (dir != 0) drone_set_enabled(!drone_get_enabled());
            break;
        case MI_DRUM_ENGINE:
            if (dir != 0) {
                int n = (int)sequencer_core_drum_source_count();
                int ni = ((int)sequencer_core_get_drum_source() + dir + n) % n;
                sequencer_core_set_drum_source((uint8_t)ni);
                /* The bank seed rewrote the core's per-track presets; refresh
                 * the UI mirror so the drum screen's labels follow. */
                for (uint8_t i = 0; i < seq_state.num_layers; i++) {
                    if (seq_state.layers[i].type != SEQ_LAYER_DRUM) continue;
                    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
                        seq_state.layers[i].track_pcm_preset[t] =
                            sequencer_core_get_drum_pcm_preset(i, t);
                    }
                }
            }
            break;
        case MI_VOLUME: {
            /* 5% steps; the setter clamps and writes amy_global.volume[]. */
            amy_fx_set_master_volume(amy_fx_get_master_volume() + (float)dir * 0.05f);
            break;
        }
        default:
            break;
    }
}

void synth_ui_menu_toggle(void)
{
    /* The graph editor is the top overlay; don't let the menu fight it. */
    if (synth_ui_graph_is_active()) return;
    seq_state.menu_open    = !seq_state.menu_open;
    seq_state.menu_editing = false;
    if (seq_state.menu_open) {
        /* Always reopen on the main page so the menu lands somewhere known. */
        s_fx_page = false;
        s_fxbus_page = false;
        s_notefx_page = false;
#if CONFIG_SYNTH_PROJECT_STORE
        s_projects_page = false;
#endif
        s_chords_page = false;
        s_bounce_page = false;
        s_proggen_page = false;
#if CONFIG_SYNTH_WIRELESS
        s_wireless_page = false;
#endif
        if (seq_state.menu_cursor >= MI_COUNT) seq_state.menu_cursor = 0;
    }
    s_force_redraw = true;
    ESP_LOGI(TAG, "menu %s", seq_state.menu_open ? "open" : "closed");
}

bool synth_ui_menu_is_active(void)
{
    return seq_state.menu_open;
}

/* Projects-page rename hooks (see synth_ui.h). The overlay owns the page state,
 * so it gates the projects module's rename flag with menu_open +
 * s_projects_page - both reset on menu-open, so an uncommitted rename cannot
 * leak onto another screen and callers poll one always-coherent predicate.
 * Compiles to false/no-op when the project store is off. */
bool synth_ui_menu_rename_active(void)
{
#if CONFIG_SYNTH_PROJECT_STORE
    return seq_state.menu_open && s_projects_page && projects_menu_is_renaming();
#else
    return false;
#endif
}

void synth_ui_menu_rename_save(void)
{
#if CONFIG_SYNTH_PROJECT_STORE
    if (synth_ui_menu_rename_active()) projects_menu_rename_commit();
#endif
}

void synth_ui_menu_rename_discard(void)
{
#if CONFIG_SYNTH_PROJECT_STORE
    if (synth_ui_menu_rename_active()) projects_menu_rename_cancel();
#endif
}

bool synth_ui_menu_handle_encoder(long delta)
{
    if (!seq_state.menu_open) return false;
    if (delta == 0) return true;

    if (seq_state.menu_editing) {
        if (s_notefx_page) {
            notefx_menu_edit_value(seq_state.menu_cursor, (int)delta);
        } else if (s_fxbus_page) {
            fx_menu_edit_value(seq_state.menu_cursor, (int)delta);
        } else if (s_fx_page) {
            fxhub_edit_value(seq_state.menu_cursor, (int)delta);
#if CONFIG_SYNTH_PROJECT_STORE
        } else if (s_projects_page) {
            projects_menu_edit_value(seq_state.menu_cursor, (int)delta);
#endif
        } else if (s_chords_page) {
            chords_menu_edit_value(seq_state.menu_cursor, (int)delta);
        } else if (s_bounce_page) {
            bounce_menu_edit_value(seq_state.menu_cursor, (int)delta);
        } else if (s_proggen_page) {
            proggen_menu_edit_value(seq_state.menu_cursor, (int)delta);
#if CONFIG_SYNTH_WIRELESS
        } else if (s_wireless_page) {
            wireless_menu_edit_value(seq_state.menu_cursor, (int)delta);
#endif
        } else {
            menu_edit_value((menu_item_id_t)seq_state.menu_cursor, (int)delta);
        }
    } else {
        int n = s_notefx_page ? (int)notefx_menu_item_count() :
                s_fxbus_page ? (int)fx_menu_item_count() :
                s_fx_page ? (int)fxhub_item_count() :
#if CONFIG_SYNTH_PROJECT_STORE
                s_projects_page ? (int)projects_menu_item_count() :
#endif
                s_chords_page ? (int)chords_menu_item_count() :
                s_bounce_page ? (int)bounce_menu_item_count() :
                s_proggen_page ? (int)proggen_menu_item_count() :
#if CONFIG_SYNTH_WIRELESS
                s_wireless_page ? (int)wireless_menu_item_count() :
#endif
                (int)MI_COUNT;
        int c = (int)seq_state.menu_cursor + (int)delta;
        /* wrap: the lists are long enough that top-to-bottom in one click
         * beats scrolling the whole way */
        c %= n;
        if (c < 0) c += n;
        seq_state.menu_cursor = (uint8_t)c;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_menu_handle_button(void)
{
    if (!seq_state.menu_open) return false;

    if (s_notefx_page) {
        uint8_t idx = seq_state.menu_cursor;
        if (notefx_menu_item_is_value(idx)) {
            seq_state.menu_editing = !seq_state.menu_editing;
        } else if (notefx_menu_item_is_back(idx)) {
            /* Back returns to the FX hub it was dived from. */
            s_notefx_page = false;
            s_fx_page = true;
            seq_state.menu_cursor  = s_fx_cursor;
            seq_state.menu_editing = false;
        }
        s_force_redraw = true;
        return true;
    }

    if (s_fxbus_page) {
        uint8_t idx = seq_state.menu_cursor;
        if (fx_menu_item_is_value(idx)) {
            seq_state.menu_editing = !seq_state.menu_editing;
        } else if (fx_menu_item_is_back(idx)) {
            /* Back returns to the hub row this bus was dived from. */
            s_fxbus_page = false;
            s_fx_page = true;
            seq_state.menu_cursor  = s_fxhub_cursor;
            seq_state.menu_editing = false;
        }
        s_force_redraw = true;
        return true;
    }

    if (s_fx_page) {
        uint8_t idx = seq_state.menu_cursor;
        uint8_t bus = FX_BUS_HOME;
        if (fxhub_item_is_bus(idx, &bus)) {
            /* Dive into that bus's FX page; park the hub cursor. */
            s_fxhub_cursor = seq_state.menu_cursor;
            fx_menu_set_bus(bus);
            s_fx_page = false;
            s_fxbus_page = true;
            seq_state.menu_cursor  = 0;
            seq_state.menu_editing = false;
        } else if (fxhub_item_is_notefx(idx)) {
            /* Dive into the per-layer NoteFX page; park the hub cursor. */
            s_fx_cursor = seq_state.menu_cursor;
            s_fx_page = false;
            s_notefx_page = true;
            seq_state.menu_cursor  = 0;
            seq_state.menu_editing = false;
        } else if (fxhub_item_is_value(idx)) {
            seq_state.menu_editing = !seq_state.menu_editing;
        } else if (fxhub_item_is_back(idx)) {
            s_fx_page = false;
            seq_state.menu_cursor  = s_main_cursor;
            seq_state.menu_editing = false;
        }
        s_force_redraw = true;
        return true;
    }

#if CONFIG_SYNTH_PROJECT_STORE
    if (s_projects_page) {
        uint8_t idx = seq_state.menu_cursor;
        if (projects_menu_item_is_back(idx)) {
            s_projects_page = false;
            seq_state.menu_cursor  = s_main_cursor;
            seq_state.menu_editing = false;
        } else if (projects_menu_item_is_value(idx)) {
            seq_state.menu_editing = projects_menu_handle_click(idx);
        }
        s_force_redraw = true;
        return true;
    }
#endif

    if (s_chords_page) {
        uint8_t idx = seq_state.menu_cursor;
        if (chords_menu_item_is_back(idx)) {
            s_chords_page = false;
            seq_state.menu_cursor  = s_main_cursor;
            seq_state.menu_editing = false;
        } else {
            seq_state.menu_editing = chords_menu_handle_click(idx);
        }
        s_force_redraw = true;
        return true;
    }

    if (s_bounce_page) {
        uint8_t idx = seq_state.menu_cursor;
        if (bounce_menu_item_is_back(idx)) {
            s_bounce_page = false;
            seq_state.menu_cursor  = s_main_cursor;
            seq_state.menu_editing = false;
        } else {
            seq_state.menu_editing = bounce_menu_handle_click(idx);
        }
        s_force_redraw = true;
        return true;
    }

    if (s_proggen_page) {
        uint8_t idx = seq_state.menu_cursor;
        if (proggen_menu_item_is_back(idx)) {
            s_proggen_page = false;
            seq_state.menu_cursor  = s_main_cursor;
            seq_state.menu_editing = false;
        } else {
            seq_state.menu_editing = proggen_menu_handle_click(idx);
        }
        s_force_redraw = true;
        return true;
    }

#if CONFIG_SYNTH_WIRELESS
    if (s_wireless_page) {
        uint8_t idx = seq_state.menu_cursor;
        if (wireless_menu_item_is_back(idx)) {
            s_wireless_page = false;
            seq_state.menu_cursor  = s_main_cursor;
            seq_state.menu_editing = false;
        } else {
            seq_state.menu_editing = wireless_menu_handle_click(idx);
        }
        s_force_redraw = true;
        return true;
    }
#endif

    menu_item_id_t id = (menu_item_id_t)seq_state.menu_cursor;

    if (menu_item_is_value(id)) {
        /* Toggle in/out of editing this value. */
        seq_state.menu_editing = !seq_state.menu_editing;
    } else {
        /* Action item: run it and close the menu. */
        switch (id) {
            case MI_SCREEN_SEQ:
                seq_state.ui_mode = UI_MODE_SEQUENCER;
                seq_state.menu_open = false;
                break;
            case MI_SCREEN_ARP:
                seq_state.ui_mode = UI_MODE_ARP;
                seq_state.menu_open = false;
                break;
            case MI_SCREEN_DRONE:
                /* Opens the normal drone; its STUTTER row dives to the
                 * stutter screen (UI_MODE_DRONE). */
                seq_state.ui_mode = UI_MODE_DRONE_STD;
                seq_state.menu_open = false;
                break;
            case MI_SCREEN_PROG:
                seq_state.ui_mode = UI_MODE_PROG;
                seq_state.menu_open = false;
                break;
            case MI_SCREEN_TRACKOPTS:
                seq_state.ui_mode = UI_MODE_TRACKOPTS;
                seq_state.menu_open = false;
                s_to_layer = seq_state.active_layer_idx;
                s_to_track = seq_state.selected_track;
                break;
#if CONFIG_SYNTH_DEV_MENU
            case MI_DEV:
                seq_state.ui_mode = UI_MODE_DEV;
                seq_state.menu_open = false;
                break;
#endif
#if CONFIG_SYNTH_CUSTOM_FM
            case MI_SCREEN_FM:
                seq_state.ui_mode = UI_MODE_FM;
                seq_state.menu_open = false;
                break;
#endif
            case MI_ADD_LAYER:
                if (seq_state.num_layers < MAX_LAYERS)
                    synth_ui_request_add_layer();
                seq_state.menu_open = false;
                break;
            case MI_REMOVE_LAYER:
                s_to_layer = seq_state.active_layer_idx;
                synth_ui_request_delete_to_layer();
                seq_state.menu_open = false;
                break;
            case MI_CHORDS:
                /* Dive into the chord-preset page; the menu stays open. */
                s_main_cursor = seq_state.menu_cursor;
                s_chords_page = true;
                seq_state.menu_cursor = 0;
                chords_menu_reset();
                break;
            case MI_BOUNCE:
                /* Dive into the loop-bounce page; the menu stays open. */
                s_main_cursor = seq_state.menu_cursor;
                s_bounce_page = true;
                seq_state.menu_cursor = 0;
                bounce_menu_reset();
                break;
            case MI_PROGGEN:
                /* Dive into the progression-generator page; the menu stays open. */
                s_main_cursor = seq_state.menu_cursor;
                s_proggen_page = true;
                seq_state.menu_cursor = 0;
                proggen_menu_reset();
                break;
            case MI_FX_MENU:
                /* Dive into the FX hub; the menu stays open. */
                s_main_cursor = seq_state.menu_cursor;
                s_fx_page = true;
                seq_state.menu_cursor = 0;
                break;
#if CONFIG_SYNTH_PROJECT_STORE
            case MI_PROJECTS:
                /* Dive into the projects page; the menu stays open. */
                s_main_cursor = seq_state.menu_cursor;
                s_projects_page = true;
                seq_state.menu_cursor = 0;
                projects_menu_reset();
                break;
#endif
#if CONFIG_SYNTH_WIRELESS
            case MI_WIRELESS:
                /* Dive into the wireless page; the menu stays open. */
                s_main_cursor = seq_state.menu_cursor;
                s_wireless_page = true;
                seq_state.menu_cursor = 0;
                wireless_menu_reset();
                break;
#endif
            case MI_SAMPLE:
                switch (sample_rec_get_state()) {
                    case SAMPLE_REC_IDLE:
                        if (seq_state.layers[seq_state.active_layer_idx].type == SEQ_LAYER_DRUM) {
                            sample_rec_arm(seq_state.active_layer_idx, seq_state.selected_track);
                        } else {
                            ESP_LOGW(TAG, "sample_rec: select a drum track first");
                        }
                        break;
                    case SAMPLE_REC_ARMED:
                        sample_rec_start();
                        break;
                    case SAMPLE_REC_READY:
                        sample_rec_assign();
                        break;
                    case SAMPLE_REC_RECORDING:
                    default:
                        break;   /* capture runs regardless of the menu */
                }
                seq_state.menu_open = false;
                break;
            case MI_SAMPLE_CANCEL:
                sample_rec_cancel();
                seq_state.menu_open = false;
                break;
            default:
                break;
        }
        seq_state.menu_editing = false;
    }
    s_force_redraw = true;
    return true;
}
