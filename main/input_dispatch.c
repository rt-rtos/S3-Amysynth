/* Gesture dispatcher: see input_dispatch.h for the contract. The user-facing
 * control scheme is CONTROLS.md. */

#include <stdbool.h>
#include "sdkconfig.h"
#include "input_dispatch.h"
#include "synth_ui.h"
#include "sequencer_core.h"

// MY_BUTTON_1 held: encoder turns cycle the active screen's patch instead of
// moving the selection.
static volatile bool s_patch_held = false;
// MY_BUTTON_2 held: encoder turns transpose the selected track's pitch.
static volatile bool s_drum_select_held = false;

// SHIFT hold state + per-button latch so a SHIFT-chorded button is swallowed
// for its whole press (its normal gestures never fire, no stuck latch on
// release). Written by the button-dispatch task; volatile because the encoder
// task reads it for the Shift+Turn gesture (same as s_patch_held).
static volatile bool s_shift_held = false;
static bool s_shift_chord_latched[MY_BUTTON_MAX] = { false };
/* SHIFT+0 bounce chord in flight: PRESS_DOWN seen with SHIFT held, waiting
 * for the release (press) or the long-press threshold (discard). */
static bool s_bounce_chord = false;

void input_dispatch_button(my_button_id_t button_id, button_event_t event)
{
    /* MY_BUTTON_SHOULDER, per view: SEQ toggles the step under the cursor
     * (two-handed tracker-style entry), GRAPH flips the routing depth's sign,
     * LFO flips the target checklist tab, FM flips the editor page, WT cycles
     * the focused keyframe, MENU leaves a sub-page for the main list (button 3
     * reopens the last page, so this is the way home). Step entry, the
     * polarity flip and the menu jump take PRESS_DOWN for zero tap latency;
     * the tab, page and keyframe flips are deliberate navigation gestures, so
     * they wait for the click. All events consumed. */
    if (button_id == MY_BUTTON_SHOULDER) {
        ui_view_id_t sv = synth_ui_active_view();
        if (event == BUTTON_PRESS_DOWN) {
            if (sv == UI_VIEW_SEQ) {
                synth_ui_toggle_step_at_cursor();
            } else if (sv == UI_VIEW_GRAPH) {
                /* No-op unless a routing target stop is up. */
                synth_ui_graph_flip_depth_polarity();
            } else if (sv == UI_VIEW_MENU) {
                synth_ui_menu_go_main();
            }
        } else if (event == BUTTON_SINGLE_CLICK && sv == UI_VIEW_LFO) {
            synth_ui_lfo_toggle_target_tab();
#if CONFIG_SYNTH_CUSTOM_FM
        } else if (event == BUTTON_SINGLE_CLICK && sv == UI_VIEW_FM) {
            synth_ui_fm_toggle_page();
#endif
#if CONFIG_SYNTH_CUSTOM_WT
        } else if (event == BUTTON_SINGLE_CLICK && sv == UI_VIEW_WT) {
            synth_ui_wt_next_keyframe();
#endif
        }
        return;
    }

    /* MY_BUTTON_SHIFT: hold-modifier layer; a bare tap does nothing. */
    if (button_id == MY_BUTTON_SHIFT) {
        if (event == BUTTON_PRESS_DOWN)      s_shift_held = true;
        else if (event == BUTTON_PRESS_UP)   s_shift_held = false;
        return;
    }

    /* SHIFT+0 loop-bounce chord: latched on PRESS_DOWN like the others, but
     * it acts on the release (a press: start / cancel / quantized stop) or on
     * LONG_PRESS_START (discard a running take), so one hand runs the
     * bounce from any screen. The latch below swallows the SINGLE_CLICK that
     * follows the release, so button 0's own tap gesture never fires. */
    if (button_id == MY_BUTTON_0 && s_bounce_chord) {
        if (event == BUTTON_LONG_PRESS_START) {
            s_bounce_chord = false;
            synth_ui_bounce_chord(true);
            return;
        }
        if (event == BUTTON_PRESS_UP) {
            s_bounce_chord = false;
            synth_ui_bounce_chord(false);
            return;
        }
        if (event != BUTTON_PRESS_DOWN) return;
        s_bounce_chord = false;
    }

    /* SHIFT chords, fired on the digit's PRESS_DOWN:
     *   SHIFT+0 -> loop bounce transport (see above)
     *   SHIFT+1 -> open the ADSR/graph editor, or close+commit an open one
     *   SHIFT+2 -> toggle the step probability/trig editor; inside an
     *              effects editor: release the open tab to the patch
     *   SHIFT+3 -> inside an effects editor: flip the row between its own
     *              voice block and the layer's shared one; on the sequencer
     *              screen: open the menu's Layer page, or close it
     * The chord latches the button until its next PRESS_DOWN, swallowing the
     * rest of the press so the button's normal gesture never runs. */
    if (s_shift_chord_latched[button_id]) {
        if (event == BUTTON_PRESS_DOWN) {
            s_shift_chord_latched[button_id] = false;  /* fresh press: re-evaluate */
        } else {
            return;                                    /* swallow the chorded press */
        }
    }
    if (s_shift_held && event == BUTTON_PRESS_DOWN && button_id == MY_BUTTON_0) {
        s_shift_chord_latched[MY_BUTTON_0] = true;
        s_bounce_chord = true;
        return;
    }
    if (s_shift_held && event == BUTTON_PRESS_DOWN &&
        (button_id == MY_BUTTON_1 || button_id == MY_BUTTON_2 ||
         button_id == MY_BUTTON_3)) {
        s_shift_chord_latched[button_id] = true;
        ui_view_id_t sv = synth_ui_active_view();
        if (button_id == MY_BUTTON_1) {
            bool open_from_screen = (sv == UI_VIEW_SEQ || sv == UI_VIEW_ARP ||
                                     sv == UI_VIEW_DRONE || sv == UI_VIEW_DRONE_VIS ||
                                     sv == UI_VIEW_DRONE_STD);
#if CONFIG_SYNTH_WIRELESS
            /* Wireless overlay page: same chord, bound to the BLE live-play
             * voice (synth_ui_graph_open_envelope picks the target). */
            open_from_screen = open_from_screen ||
                               (sv == UI_VIEW_MENU && synth_ui_wireless_page_is_open());
#endif
            if (open_from_screen) {
                synth_ui_graph_open_envelope();
            } else if (sv == UI_VIEW_GRAPH) {
                synth_ui_graph_close_commit();
            } else if (sv == UI_VIEW_LFO) {
                synth_ui_lfo_close_commit();
            } else if (sv == UI_VIEW_DIST) {
                synth_ui_dist_close_commit();
            } else if (sv == UI_VIEW_FILTER) {
                synth_ui_filter_close_commit();
            }
        } else if (button_id == MY_BUTTON_2) {
            bool editor_open = (sv == UI_VIEW_GRAPH || sv == UI_VIEW_LFO ||
                                sv == UI_VIEW_DIST  || sv == UI_VIEW_FILTER);
            if (editor_open) {
                /* Inside an editor the chord hands the open tab back to the
                 * patch - chorded, like the source flip, so a stray press
                 * cannot drop an authored group. */
                synth_ui_editor_release_to_patch();
            } else if (synth_ui_stepedit_is_active()) {
                /* stepedit has no discard path, so close == commit.
                 * synth_ui_stepedit_open() self-gates to the sequencer screen. */
                synth_ui_stepedit_close();
            } else if (!synth_ui_menu_is_active()) {
                synth_ui_stepedit_open();
            }
        } else { /* MY_BUTTON_3 */
            /* Chorded so an accidental bare press can't flip which voice
             * block the row reads. */
            if (sv == UI_VIEW_GRAPH || sv == UI_VIEW_LFO || sv == UI_VIEW_DIST ||
                sv == UI_VIEW_FILTER) {
                synth_ui_toggle_editor_source();
            } else if (sv == UI_VIEW_SEQ || sv == UI_VIEW_MENU) {
                synth_ui_menu_toggle_layer_page();
            }
        }
        return;
    }

    /* Project rename editor: MY_BUTTON_1 saves, MY_BUTTON_2 discards.
     * synth_ui_menu_rename_active() short-circuits instantly otherwise, so
     * the normal gestures below are undisturbed elsewhere. */
    if ((button_id == MY_BUTTON_1 || button_id == MY_BUTTON_2) &&
        synth_ui_menu_rename_active()) {
        if (event == BUTTON_PRESS_DOWN) {
            if (button_id == MY_BUTTON_1) synth_ui_menu_rename_save();
            else                          synth_ui_menu_rename_discard();
        }
        return;
    }

    /* DEV screen: no patch-select or pitch hold - they would edit the active
     * layer behind the screen. Clear both latches in case one was held when
     * the screen switched. */
    if ((button_id == MY_BUTTON_1 || button_id == MY_BUTTON_2) &&
        synth_ui_active_view() == UI_VIEW_DEV) {
        s_patch_held = false;
        synth_ui_set_patch_select_mode(false);
        s_drum_select_held = false;
        synth_ui_set_drum_select_mode(false);
        return;
    }

#if CONFIG_SYNTH_CUSTOM_WT
    /* WT screen: Button 1 copies the focused keyframe to the next one,
     * Button 2 resets it or blends M; neither is a hold (the patch-select
     * and pitch holds would edit the layer behind the screen). Clear both
     * latches in case one was held when the screen switched. */
    if ((button_id == MY_BUTTON_1 || button_id == MY_BUTTON_2) &&
        synth_ui_active_view() == UI_VIEW_WT) {
        s_patch_held = false;
        synth_ui_set_patch_select_mode(false);
        s_drum_select_held = false;
        synth_ui_set_drum_select_mode(false);
        if (event == BUTTON_PRESS_DOWN) {
            if (button_id == MY_BUTTON_1) synth_ui_wt_copy_keyframe();
            else                          synth_ui_wt_reset_keyframe();
        }
        return;
    }
#endif

    // MY_BUTTON_1, per editor: filter = enabled toggle, envelope = cycle EG
    // curve type, LFO = unused (source flip is SHIFT+3). Otherwise it is the
    // patch-select hold.
    if (button_id == MY_BUTTON_1) {
        if (synth_ui_filter_is_active()) {
            if (event == BUTTON_PRESS_DOWN) {
                synth_ui_filter_toggle_enabled();
            }
            return;
        }
        if (synth_ui_graph_is_active()) {
            if (event == BUTTON_PRESS_DOWN) {
                synth_ui_graph_cycle_eg_type();
            }
            return;
        }
        if (synth_ui_lfo_is_active() || synth_ui_dist_is_active()) {
            return;   /* bare press is a no-op; source flip is SHIFT+3 */
        }
        /* PROG screen: delete the entry at the cursor. */
        if (synth_ui_prog_is_active()) {
            if (event == BUTTON_PRESS_DOWN) {
                synth_ui_prog_delete_entry();
            }
            return;
        }
#if CONFIG_SYNTH_CUSTOM_FM
        /* FM screen: link mode (synth_ui.h), never the patch-select hold,
         * which would only move the layer off the voice being edited. Clear
         * the latch in case it was held when the screen switched. */
        if (synth_ui_active_view() == UI_VIEW_FM) {
            s_patch_held = false;
            synth_ui_set_patch_select_mode(false);
            if (event == BUTTON_PRESS_DOWN) synth_ui_fm_link_button();
            return;
        }
#endif
        if (event == BUTTON_PRESS_DOWN) {
            s_patch_held = true;
            synth_ui_set_patch_select_mode(true);
        } else if (event == BUTTON_PRESS_UP) {
            s_patch_held = false;
            synth_ui_set_patch_select_mode(false);
        }
        return;
    }

    /* Arp screen isolation: sequencer editing gestures must not leak through
     * and mutate state behind the hidden grid. The arp's own input is handled
     * elsewhere; menu toggle (3) and play/pause (0 long) stay live. */
    if (synth_ui_arp_is_active()) {
        switch (button_id) {
            case MY_BUTTON_2:
                /* Block drum-select and clear the latch in case it was held
                 * when the screen switched. */
                s_drum_select_held = false;
                synth_ui_set_drum_select_mode(false);
                return;
            case MY_BUTTON_0:
                /* Layer cycle is sequencer-only; keep play/pause. */
                if (event == BUTTON_LONG_PRESS_START) {
                    synth_ui_toggle_playing();
                }
                return;
            default:
                break;
        }
    }

    /* PROG screen: same isolation; MY_BUTTON_2 is repurposed as "+entry"
     * (MY_BUTTON_1 "del" handled above). */
    if (synth_ui_prog_is_active()) {
        switch (button_id) {
            case MY_BUTTON_2:
                s_drum_select_held = false;
                synth_ui_set_drum_select_mode(false);
                if (event == BUTTON_PRESS_DOWN) {
                    synth_ui_prog_add_entry();
                }
                return;
            case MY_BUTTON_0:
                if (event == BUTTON_LONG_PRESS_START) {
                    synth_ui_toggle_playing();
                }
                return;
            default:
                break;
        }
    }

    /* Drone screens: same isolation as the arp guard above. */
    if (synth_ui_drone_is_active() || synth_ui_drone_std_is_active()) {
        switch (button_id) {
            case MY_BUTTON_2:
                s_drum_select_held = false;
                synth_ui_set_drum_select_mode(false);
                return;
            case MY_BUTTON_0:
                if (event == BUTTON_LONG_PRESS_START) {
                    synth_ui_toggle_playing();
                }
                return;
            default:
                break;
        }
    }

    /* Below the isolation guards every button dispatches on the single
     * precedence resolver, so input always agrees with the draw switch about
     * the active view. */
    ui_view_id_t v = synth_ui_active_view();

    // MY_BUTTON_2: pitch-edit hold normally; in the graph editor it toggles
    // amp-edit mode (encoder adjusts amplitude trim instead of ADSR points);
    // on the FM screen it mutes/unmutes the selected operator.
    if (button_id == MY_BUTTON_2) {
        switch (v) {
            case UI_VIEW_FILTER:
                /* Suppress drum-select hold so the latch can't stick. */
                return;
            case UI_VIEW_GRAPH:
                if (event == BUTTON_PRESS_DOWN) synth_ui_graph_toggle_amp_mode();
                return;
            case UI_VIEW_STEPEDIT:
                /* Popup owns the encoder; suppress drum-select hold. */
                return;
#if CONFIG_SYNTH_CUSTOM_FM
            case UI_VIEW_FM:
                /* Clear the latch in case it was held when the screen switched. */
                s_drum_select_held = false;
                synth_ui_set_drum_select_mode(false);
                if (event == BUTTON_PRESS_DOWN) synth_ui_fm_toggle_mute();
                return;
#endif
            default:
                break;
        }
        if (event == BUTTON_PRESS_DOWN) {
            s_drum_select_held = true;
            synth_ui_set_drum_select_mode(true);
        } else if (event == BUTTON_PRESS_UP) {
            s_drum_select_held = false;
            synth_ui_set_drum_select_mode(false);
        }
        return;
    }

    // MY_BUTTON_3: inside an editor, click cycles editor pages (EG0 -> EG1 ->
    // filter -> LFO -> DIST); in STEPEDIT it closes; in FM link mode it ends
    // linking; otherwise it is the menu toggle.
    if (button_id == MY_BUTTON_3) {
#if CONFIG_SYNTH_CUSTOM_FM
        if (v == UI_VIEW_FM && synth_ui_fm_link_active()) {
            /* Every event consumed: the menu does not open while linking. */
            if (event == BUTTON_SINGLE_CLICK) synth_ui_fm_link_end();
            return;
        }
#endif
        if (v == UI_VIEW_GRAPH || v == UI_VIEW_FILTER || v == UI_VIEW_LFO ||
            v == UI_VIEW_DIST) {
            if (event == BUTTON_SINGLE_CLICK) {
                synth_ui_cycle_editor();
            }
            return;
        }
        if (v == UI_VIEW_STEPEDIT) {
            if (event == BUTTON_SINGLE_CLICK) {
                synth_ui_stepedit_close();
            }
            return;
        }
        if (event == BUTTON_SINGLE_CLICK) {
            synth_ui_menu_toggle();
        }
        return;
    }

    /* MY_BUTTON_ENC: in the editors a short press toggles select<->adjust
     * (commit/close is a MY_BUTTON_0 tap, open is SHIFT+1); the mode screens
     * edit their focused row/field. */
    if (button_id == MY_BUTTON_ENC) {
        switch (v) {
            case UI_VIEW_FILTER:
                if (event == BUTTON_PRESS_DOWN)  synth_ui_filter_handle_button(false);
                return;
            case UI_VIEW_LFO:
                if (event == BUTTON_PRESS_DOWN)  synth_ui_lfo_handle_button(false);
                return;
            case UI_VIEW_DIST:
                if (event == BUTTON_PRESS_DOWN)  synth_ui_dist_handle_button(false);
                return;
            case UI_VIEW_STEPEDIT:
                if (event == BUTTON_PRESS_DOWN)  synth_ui_stepedit_handle_button();
                return;
            case UI_VIEW_GRAPH:
                if (event == BUTTON_PRESS_DOWN)  synth_ui_graph_handle_button(false);
                return;
            case UI_VIEW_MENU:
                if (event == BUTTON_PRESS_DOWN) synth_ui_menu_handle_button();
                return;
            case UI_VIEW_ARP:
                if (event == BUTTON_PRESS_DOWN)  synth_ui_arp_handle_button();
                return;
            case UI_VIEW_DRONE:
            case UI_VIEW_DRONE_VIS:
                if (event == BUTTON_PRESS_DOWN)  synth_ui_drone_handle_button();
                return;
            case UI_VIEW_DRONE_STD:
                if (event == BUTTON_PRESS_DOWN)  synth_ui_drone_std_handle_button();
                return;
            case UI_VIEW_PROG:
                if (event == BUTTON_PRESS_DOWN) synth_ui_prog_handle_button();
                return;
#if CONFIG_SYNTH_DEV_MENU
            case UI_VIEW_DEV:
                if (event == BUTTON_PRESS_DOWN) synth_ui_dev_handle_button();
                return;
#endif
#if CONFIG_SYNTH_CUSTOM_FM
            case UI_VIEW_FM:
                if (event == BUTTON_PRESS_DOWN) synth_ui_fm_handle_button();
                return;
#endif
#if CONFIG_SYNTH_CUSTOM_WT
            case UI_VIEW_WT:
                if (event == BUTTON_PRESS_DOWN) synth_ui_wt_handle_button();
                return;
#endif
            default:  /* UI_VIEW_SEQ */
                break;
        }
        /* SEQ: fall through to the normal PRESS_DOWN handling below. */
    }

    /* MY_BUTTON_0 while an editor is open: tap = commit & close, long press =
     * cancel/discard (STEPEDIT has no discard path; both just close it). All
     * events consumed, so transport is unavailable until the editor closes. */
    if (button_id == MY_BUTTON_0 &&
        (v == UI_VIEW_GRAPH || v == UI_VIEW_FILTER || v == UI_VIEW_LFO ||
         v == UI_VIEW_DIST  || v == UI_VIEW_STEPEDIT)) {
        if (event == BUTTON_SINGLE_CLICK) {              /* tap = commit & close */
            if (v == UI_VIEW_FILTER)        synth_ui_filter_close_commit();
            else if (v == UI_VIEW_LFO)      synth_ui_lfo_close_commit();
            else if (v == UI_VIEW_DIST)     synth_ui_dist_close_commit();
            else if (v == UI_VIEW_STEPEDIT) synth_ui_stepedit_close();
            else                            synth_ui_graph_close_commit();
        } else if (event == BUTTON_LONG_PRESS_START) {   /* hold = cancel / discard */
            if (v == UI_VIEW_FILTER)        synth_ui_filter_handle_button(true);
            else if (v == UI_VIEW_LFO)      synth_ui_lfo_handle_button(true);
            else if (v == UI_VIEW_DIST)     synth_ui_dist_handle_button(true);
            else if (v == UI_VIEW_STEPEDIT) synth_ui_stepedit_close(); /* no discard path */
            else                            synth_ui_graph_handle_button(true);
        }
        return;
    }

    /* MY_BUTTON_0: short press = cycle active layer, long press = play/stop */
    if (button_id == MY_BUTTON_0) {
        if (event == BUTTON_SINGLE_CLICK) {
            synth_ui_cycle_active_layer();
        } else if (event == BUTTON_LONG_PRESS_START) {
            synth_ui_toggle_playing();
        }
        return;
    }

    /* All other buttons respond to PRESS_DOWN */
    if (event != BUTTON_PRESS_DOWN) return;

    switch (button_id) {
        case MY_BUTTON_ENC:
            synth_ui_handle_button();
            break;
        default:
            break;
    }
}

void input_dispatch_encoder_steps(long steps)
{
    /* Same resolver as the buttons (synth_ui_active_view()): the overlays
     * capture the encoder outright; below them the mode screens dispatch by
     * view, with the patch-hold / drum-select modifiers applied. */
    ui_view_id_t v = synth_ui_active_view();
    switch (v) {
        case UI_VIEW_FILTER:   synth_ui_filter_handle_encoder(steps);   return;
        case UI_VIEW_LFO:      synth_ui_lfo_handle_encoder(steps);      return;
        case UI_VIEW_DIST:     synth_ui_dist_handle_encoder(steps);     return;
        case UI_VIEW_STEPEDIT: synth_ui_stepedit_handle_encoder(steps); return;
        case UI_VIEW_GRAPH:    synth_ui_graph_handle_encoder(steps);    return;
        case UI_VIEW_MENU:     synth_ui_menu_handle_encoder(steps);     return;
        default:               break;  /* fall through to the mode-tail */
    }

    if (s_patch_held) {
        // Patch hold+turn cycles the active screen's patch; on a drum
        // layer that is the SELECTED track's own patch.
        if (v == UI_VIEW_DRONE || v == UI_VIEW_DRONE_VIS) {
            synth_ui_drone_cycle_patch((int)steps);
        } else if (v == UI_VIEW_DRONE_STD) {
            synth_ui_drone_std_cycle_patch((int)steps);
        } else if (v == UI_VIEW_ARP) {
            synth_ui_arp_cycle_patch((int)steps);
        } else if (sequencer_core_get_layer_type(seq_get_active_layer_idx())
                   == SEQ_LAYER_DRUM) {
            synth_ui_cycle_drum_patch((int)steps);
        } else {
            synth_ui_cycle_melodic_patch((int)steps);
        }
    } else if (v == UI_VIEW_DRONE || v == UI_VIEW_DRONE_VIS) {
        synth_ui_drone_handle_encoder(steps);
    } else if (v == UI_VIEW_DRONE_STD) {
        synth_ui_drone_std_handle_encoder(steps);
    } else if (s_drum_select_held) {
        // Pitch-edit hold: transpose the selected track (drum or
        // melodic).
        synth_ui_adjust_track_note((int)steps);
    } else if (v == UI_VIEW_ARP) {
        synth_ui_arp_handle_encoder(steps);
    } else if (v == UI_VIEW_PROG) {
        synth_ui_prog_handle_encoder((int)steps);
#if CONFIG_SYNTH_DEV_MENU
    } else if (v == UI_VIEW_DEV) {
        synth_ui_dev_handle_encoder((int)steps);
#endif
#if CONFIG_SYNTH_CUSTOM_FM
    } else if (v == UI_VIEW_FM) {
        // SHIFT+Turn steps the FM Custom voice's algorithm, as on the grid.
        if (s_shift_held) synth_ui_fm_step_algorithm((int)steps);
        else              synth_ui_fm_handle_encoder((int)steps);
#endif
#if CONFIG_SYNTH_CUSTOM_WT
    } else if (v == UI_VIEW_WT) {
        // SHIFT has no layer here: the turn edits either way.
        synth_ui_wt_handle_encoder((int)steps);
#endif
    } else if (s_shift_held) {
        // SHIFT+Turn on the sequencer screen: step the active melodic
        // layer's FM algorithm live instead of moving the cursor.
        synth_ui_cycle_fm_algo((int)steps);
    } else {
        synth_ui_handle_encoder(steps);
    }
}
