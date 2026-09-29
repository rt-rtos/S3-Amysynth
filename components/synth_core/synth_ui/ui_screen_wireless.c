#include "sdkconfig.h"

#if CONFIG_SYNTH_WIRELESS

#include "synth_ui/synth_ui_internal.h"
#include "radio_manager.h"
#include "live_play.h"
#include "patch_names.h"
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════════
 *  WIRELESS PAGE (BLE MIDI session)
 * ════════════════════════════════════════════════════════════════════════
 * Item model for the Wireless page of the menu overlay (page state and input
 * routing live in ui_screen_menu.c, mirroring the Projects/Chords pages).
 *
 * The BLE MIDI row only queues radio_manager_request_start/stop
 * (radio_manager.h). */

enum {
    WI_BACK = 0,
    WI_TOGGLE,     /* BLE MIDI on/off (session) */
    WI_STATUS,     /* read-only session/connection status */
    WI_SOURCE,     /* live slot browse group: WAVE / PATCH (click toggles) */
    WI_PATCH,      /* live slot patch (encoder cycles while editing) */
    WI_GLIDE,      /* live slot portamento, ms (encoder edits while editing) */
    WI_COUNT
};

static menu_item_view_t s_items[WI_COUNT];

void wireless_menu_reset(void)
{
}

uint8_t wireless_menu_item_count(void)
{
    return WI_COUNT;
}

bool wireless_menu_item_is_back(uint8_t idx)
{
    return idx == WI_BACK;
}

const menu_item_view_t *wireless_menu_build_items(void)
{
    radio_state_t st = radio_manager_state();

    snprintf(s_items[WI_BACK].label, MENU_LABEL_LEN, "< Back");
    s_items[WI_BACK].value[0] = '\0';

    snprintf(s_items[WI_TOGGLE].label, MENU_LABEL_LEN, "BLE MIDI");
    const char *tv;
    switch (st) {
        case RADIO_ACTIVE:     tv = "ON";     break;
        case RADIO_STARTING:
        case RADIO_STOPPING:   tv = "..";     break;
        case RADIO_FAILED_RAM: tv = "NO RAM"; break;
        case RADIO_FAILED_ERR: tv = "ERR";    break;
        case RADIO_OFF:
        default:               tv = "OFF";    break;
    }
    snprintf(s_items[WI_TOGGLE].value, MENU_VALUE_LEN, "%s", tv);

    snprintf(s_items[WI_STATUS].label, MENU_LABEL_LEN, "Status");
    const char *sv;
    if (st == RADIO_ACTIVE) {
        sv = radio_manager_connected() ? "connected" : "advertising";
    } else if (st == RADIO_FAILED_RAM) {
        sv = "free RAM?";
    } else {
        sv = "off";
    }
    snprintf(s_items[WI_STATUS].value, MENU_VALUE_LEN, "%s", sv);

    snprintf(s_items[WI_SOURCE].label, MENU_LABEL_LEN, "Source");
    snprintf(s_items[WI_SOURCE].value, MENU_VALUE_LEN, "%s",
             live_play_get_wave_mode() ? "WAVE" : "PATCH");

    snprintf(s_items[WI_PATCH].label, MENU_LABEL_LEN, "Patch");
    {
        uint16_t patch = live_play_get_patch();
        const char *name = patch_name_for(patch);
        if (name) {
            snprintf(s_items[WI_PATCH].value, MENU_VALUE_LEN, "%s", name);
        } else {
            snprintf(s_items[WI_PATCH].value, MENU_VALUE_LEN, "%u",
                     (unsigned)patch);
        }
    }

    snprintf(s_items[WI_GLIDE].label, MENU_LABEL_LEN, "Glide");
    {
        uint16_t g = live_play_get_glide_ms();
        if (g == 0) {
            snprintf(s_items[WI_GLIDE].value, MENU_VALUE_LEN, "OFF");
        } else {
            snprintf(s_items[WI_GLIDE].value, MENU_VALUE_LEN, "%ums",
                     (unsigned)g);
        }
    }

    return s_items;
}

/* Click on item idx. Returns the desired seq_state.menu_editing value (the
 * menu router feeds it back), matching the Chords/Projects click contract. */
bool wireless_menu_handle_click(uint8_t idx)
{
    if (idx == WI_TOGGLE) {
        radio_state_t st = radio_manager_state();
        if (st == RADIO_ACTIVE) {
            radio_manager_request_stop();
        } else {
            radio_manager_request_start();
        }
        return false;
    }
    if (idx == WI_SOURCE) {
        /* Immediate toggle, no edit mode - the two-state row pattern. */
        live_play_set_wave_mode(!live_play_get_wave_mode());
        return false;
    }
    if (idx == WI_PATCH || idx == WI_GLIDE) {
        return !seq_state.menu_editing;   /* click in, click out */
    }
    return false;
}

void wireless_menu_edit_value(uint8_t idx, int delta)
{
    if (idx == WI_PATCH) {
        synth_ui_cycle_live_patch(delta);
    } else if (idx == WI_GLIDE) {
        int g = (int)live_play_get_glide_ms() + delta;   /* 1 ms per detent */
        if (g < 0) g = 0;
        if (g > (int)LIVE_PLAY_GLIDE_MAX_MS) g = (int)LIVE_PLAY_GLIDE_MAX_MS;
        live_play_set_glide_ms((uint16_t)g);
    }
}

#endif /* CONFIG_SYNTH_WIRELESS */
