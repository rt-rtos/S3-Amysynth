#include "synth_ui/synth_ui_internal.h"
#include "sequencer_core.h"
#include "transport_stop.h"
#include "amy_fx.h"
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════════
 *  SETTINGS PAGE (Main Menu -> "Settings")
 * ════════════════════════════════════════════════════════════════════════
 * Item model for the instrument-wide settings; page state and input routing
 * live in ui_screen_menu.c. Every row edits its owner's state directly
 * (amy_fx.h, transport_stop.h, sequencer_core.h, ui_clock.c); the page keeps
 * no copy. */

typedef enum {
    SET_PRESET_FX = 0,
    SET_STOP,
    SET_FADE,
    SET_START,
    SET_PHRASE,
    SET_BACK,
    SET_COUNT
} settings_item_id_t;

static const char *const s_stop_names[TSTOP_MODE_COUNT] = { "RING", "CUT", "CUT 1/4", "FADE" };
static const char *const s_fade_names[TSTOP_FADE_COUNT] = { "1/4", "1 BAR", "2 BAR", "4 BAR" };

static menu_item_view_t s_items[SET_COUNT];

uint8_t settings_menu_item_count(void)
{
    return SET_COUNT;
}

const menu_item_view_t *settings_menu_build_items(void)
{
    /* "Presets alter bus FX? y/n" - OFF makes Juno presets timbre-only. */
    snprintf(s_items[SET_PRESET_FX].label, MENU_LABEL_LEN, "Preset FX");
    snprintf(s_items[SET_PRESET_FX].value, MENU_VALUE_LEN, "%s",
             s_fx_presets_alter_global ? "ON" : "OFF");

    snprintf(s_items[SET_STOP].label, MENU_LABEL_LEN, "Stop");
    snprintf(s_items[SET_STOP].value, MENU_VALUE_LEN, "%s",
             s_stop_names[transport_stop_get_mode()]);

    /* The FADE mode's length, from the next quarter. */
    snprintf(s_items[SET_FADE].label, MENU_LABEL_LEN, "Fade");
    snprintf(s_items[SET_FADE].value, MENU_VALUE_LEN, "%s",
             s_fade_names[transport_stop_get_fade_idx()]);

    snprintf(s_items[SET_START].label, MENU_LABEL_LEN, "Start");
    snprintf(s_items[SET_START].value, MENU_VALUE_LEN, "%s",
             sequencer_core_get_silent_start() ? "SILENT" : "TAILS");

    /* Bars per phrase of the mixer header clock. */
    uint8_t bars = ui_clock_phrase_bars(ui_clock_get_phrase_idx());
    snprintf(s_items[SET_PHRASE].label, MENU_LABEL_LEN, "Phrase");
    if (bars == 0)
        snprintf(s_items[SET_PHRASE].value, MENU_VALUE_LEN, "OFF");
    else
        snprintf(s_items[SET_PHRASE].value, MENU_VALUE_LEN, "%u BAR", (unsigned)bars);

    snprintf(s_items[SET_BACK].label, MENU_LABEL_LEN, "< Back");
    s_items[SET_BACK].value[0] = '\0';

    return s_items;
}

bool settings_menu_item_is_value(uint8_t idx)
{
    return idx < SET_BACK;
}

bool settings_menu_item_is_back(uint8_t idx)
{
    return idx == SET_BACK;
}

/* Step i by dir over 0..n-1, wrapping. */
static uint8_t step_wrap(uint8_t i, int dir, uint8_t n)
{
    return (uint8_t)(((int)i + dir % (int)n + (int)n) % (int)n);
}

void settings_menu_edit_value(uint8_t idx, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    if (dir == 0) return;
    switch (idx) {
        case SET_PRESET_FX:
            s_fx_presets_alter_global = !s_fx_presets_alter_global;
            /* Re-arming the guard re-imposes the cached FX at once, on every
             * bus, undoing whatever the last preset left behind. */
            if (!s_fx_presets_alter_global) synth_ui_fx_reassert_all();
            break;
        case SET_STOP:
            transport_stop_set_mode(step_wrap((uint8_t)transport_stop_get_mode(), dir,
                                              TSTOP_MODE_COUNT));
            break;
        case SET_FADE:
            transport_stop_set_fade_idx(step_wrap(transport_stop_get_fade_idx(), dir,
                                                  TSTOP_FADE_COUNT));
            break;
        case SET_START:
            sequencer_core_set_silent_start(!sequencer_core_get_silent_start());
            break;
        case SET_PHRASE:
            ui_clock_set_phrase_idx(step_wrap(ui_clock_get_phrase_idx(), dir,
                                              UI_CLOCK_PHRASE_COUNT));
            break;
        default:
            break;
    }
}
