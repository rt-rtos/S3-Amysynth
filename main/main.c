
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "iot_button.h"
#include "priv_i2c_u8g2.h"
#include "u8g2.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task.h"
#include "amy.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "rotary_encoder.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "synth_slots.h"      /* SYNTH_SLOT_COUNT for amy_cfg.max_synths */
#include "fx_bus.h"           /* FX_BUS_COUNT for amy_cfg.max_buses */
#include "seq_core_config.h"  /* AMY sequencer tag-space layout */
#include "amy_helpers.h"   /* amy_helpers_set_render_task */
#include "custompatches/sample_rec.h"
#include "custompatches/clip_bounce.h"
#include "custompatches/drum_cache.h"
#include "custompatches/wavetable_bank.h"
#if CONFIG_SYNTH_CUSTOM_WT
#include "custompatches/wt_builder.h"
#endif
#include "filter_scope.h"
#include "usb_audio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "render_clock.h"
#include "render_stats.h"
#include "dropout_stats.h"
#include "diag_heap.h"
#include "diag_report.h"
#include "diag_mem.h"
#include "status_led.h"
#include "amy_profile.h"
#include "esp_compiler.h"
#include "soc/gpio_num.h"
#include "my_buttons.h"
#include "esp_partition.h"
#include "project_fs.h"
#include "project_store.h"
#include "project_snapshot.h"
#if CONFIG_SYNTH_WIRELESS
#include "midi_core.h"
#include "radio_manager.h"
#include "live_play.h"
#endif
#include "harness.h"

#ifndef CONFIG_AMYSYNTH_INPUT_DIAGNOSTICS
#define CONFIG_AMYSYNTH_INPUT_DIAGNOSTICS 0
#endif

static const char *TAG = "main"; // For ESP_LOG and related logs in this file

// Rotary encoder pins
#define ENCODER_PIN_A GPIO_NUM_40
#define ENCODER_PIN_B GPIO_NUM_41
static i2c_u8g2_handle_t s_display;
static u8g2_t *s_u8g2 = NULL;
static volatile uint32_t s_last_seq_tick = 0;
static volatile uint32_t s_seq_tick_hook_count = 0;
static volatile uint32_t s_render_block_count = 0;
// Render overruns and USB-ring drops are counted in dropout_stats
// (diagnostics component) alongside the wire-ZLP counter.
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

static QueueHandle_t s_button_queue = NULL;

// One press yields up to three queued events across five buttons; the queue
// must absorb a simultaneous burst. Overflow drops (zero-timeout send).
#define BUTTON_QUEUE_DEPTH 16

typedef struct {
    my_button_id_t id;
    button_event_t event;
} button_msg_t;

static void main_sequencer_tick_hook(uint32_t tick_count)
{
    s_last_seq_tick = tick_count;
    s_seq_tick_hook_count++;
    /* Per-step trig engine (seq_core_trig.c): must run at sequencer-tick
     * cadence (not the 20 Hz UI task) so ratchets resolve; no-op unless a
     * step is decorated. */
    sequencer_core_service_tick();
}

#if CONFIG_USB_AUDIO_DIAGNOSTICS
static void main_log_audio_diagnostics(void)
{
    usb_audio_diag_snapshot_t diag;
    usb_audio_diag_get_snapshot(&diag);
    ESP_LOGI(TAG,
             "audio diag: init=%d fill=%u peak_fill=%u writes=%" PRIu32 " drops=%" PRIu32 " underruns=%" PRIu32 " peak_abs=%d",
             diag.initialized ? 1 : 0,
             (unsigned)diag.fill_samples,
             (unsigned)diag.peak_fill_samples,
             diag.write_calls,
             diag.write_drop_events,
             diag.underrun_events,
             (int)diag.peak_abs_sample);
}
#endif

static void amy_usb_render_task(void *arg) {
    (void)arg;

    // Master clock: a GPTimer fires every block period (256 samples @ 48 kHz
    // = 5333.33 us); CONFIG_RENDER_CLOCK_I2S_ENABLE swaps in an I2S-DMA-paced
    // backend (render_clock.h). Started inside the task so either backend's
    // ISR registers on the render core.
    if (render_clock_start(AMY_BLOCK_SIZE, AMY_SAMPLE_RATE) != ESP_OK) {
        ESP_LOGE(TAG, "render_clock_start failed; render task aborting");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        // STRICT 1:1: render exactly one block even after an overrun (ticks
        // > 1), so amy_sysclock-derived tempo can't drift. The USB ring
        // absorbs jitter; it is not a catch-up queue.
        uint32_t ticks = render_clock_wait();
        if (unlikely(ticks > 1)) {
            dropout_count_render_overrun(ticks - 1);  // diagnostic only
        }

        render_stats_block_begin();
        uint32_t tick_hooks_pre = s_seq_tick_hook_count;

        int16_t *block = amy_update();           // synthesizes everything / advances AMY sample clock
        if (likely(block != NULL)) {
            s_render_block_count++;

            // Runtime PCM sampler: non-blocking no-op unless a recording is
            // armed. Must run after amy_update() releases amy_queue_lock.
            sample_rec_render_tick(block, AMY_BLOCK_SIZE);
            // Loop bounce: same rule, no-op while idle.
            clip_bounce_render_tick(block, AMY_BLOCK_SIZE);
            // Drum cache: counts released sample windows down to their free.
            drum_cache_render_service();

#if CONFIG_FILTER_SCOPE
            // Filter-editor overlay: reads msynth[]/synth[], so it too must
            // run after amy_update() releases amy_queue_lock. Armed only
            // while that editor is open.
            filter_scope_render_tick();
#endif

#if CONFIG_USB_AUDIO_BLOCKING_WRITE
            // Resilient path: retry until the host drains, slaving the synth
            // to the PC's consumption rate. vTaskDelay(1) waits a full tick
            // (never floors to 0), so this cannot busy-spin.
            while (usb_audio_write_stereo(block, AMY_BLOCK_SIZE) == ESP_ERR_NO_MEM) {
                vTaskDelay(1);
            }
#else
            // Real-time path: all-or-nothing write; a full ring drops the
            // whole block to keep AMY phase-aligned, with no backoff (the
            // GPTimer already paces us). Counted as a drop only while a host
            // is actually consuming.
            if (unlikely(usb_audio_write_stereo(block, AMY_BLOCK_SIZE) == ESP_ERR_NO_MEM)
                && usb_audio_consumer_active()) {
                dropout_count_ring_overrun();
            }
#endif
        }
        // Measures the full block's work (DSP + sampler/scope hooks + USB
        // ring write); the seq-tick flag feeds the worst-block snapshot.
        render_stats_block_end(s_seq_tick_hook_count != tick_hooks_pre);
    }
}
static void dispatch_button_event(my_button_id_t button_id, button_event_t event);

// Runs the full per-screen dispatch in task context. iot_button delivers on
// the esp_timer task, whose stack and latency budget can't carry the heavy
// editor/screen branches; the callback below only enqueues.
static void button_handler_task(void *pvParameters)
{
    (void)pvParameters;
    button_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_button_queue, &msg, portMAX_DELAY) == pdTRUE) {
            dispatch_button_event(msg.id, msg.event);
        }
    }
}

static void encoder_process_steps(long steps);

/* Harness injection hooks (see harness.h for the contract). The button hook
 * posts to the SAME queue as the physical callback below - same message,
 * same drop-on-full policy, same consumer task - so injected events are
 * indistinguishable at the seam. */
static void harness_inject_button_hook(int button_id, harness_btn_act_t act)
{
    if (button_id < 0 || button_id >= MY_BUTTON_MAX || s_button_queue == NULL) {
        return;
    }
    button_event_t ev;
    switch (act) {
        case HARNESS_BTN_DOWN:  ev = BUTTON_PRESS_DOWN;      break;
        case HARNESS_BTN_UP:    ev = BUTTON_PRESS_UP;        break;
        case HARNESS_BTN_CLICK: ev = BUTTON_SINGLE_CLICK;    break;
        case HARNESS_BTN_LONG:  ev = BUTTON_LONG_PRESS_START; break;
        default: return;
    }
    button_msg_t msg = { .id = (my_button_id_t)button_id, .event = ev };
    (void)xQueueSend(s_button_queue, &msg, 0);
}

static void harness_inject_encoder_hook(long steps)
{
    encoder_process_steps(steps);
}

// Thin enqueue shim on the esp_timer task. Inline fallback only if the queue
// was never created — never on a full queue, since that would reorder presses
// (a drop is less harmful than reordering).
static void main_button_event_cb(my_button_id_t button_id, button_event_t event, void *user_data)
{
    (void)user_data;

    if (event != BUTTON_PRESS_DOWN && event != BUTTON_PRESS_UP &&
        event != BUTTON_SINGLE_CLICK && event != BUTTON_LONG_PRESS_START) {
        return;
    }

    if (s_button_queue != NULL) {
        button_msg_t msg = { .id = button_id, .event = event };
        (void)xQueueSend(s_button_queue, &msg, 0);
    } else {
        dispatch_button_event(button_id, event);
    }
}

// Full button → sequencer/UI action dispatch. Runs on button_task (or inline
// on the esp_timer task only if queue creation failed at startup).
static void dispatch_button_event(my_button_id_t button_id, button_event_t event)
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

/* Route accumulated encoder steps to the active view. Extracted from the
 * poll loop so the serial harness can drive the identical chain; callers run
 * on Core 0 at input-dispatch priority (encoder_task, or the harness command
 * task on a board with no physical encoder - the two never legitimately run
 * concurrently, see harness.h). */
static void encoder_process_steps(long steps)
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

static void encoder_task(void *pvParameters)
{
    rotary_encoder_handle_t enc = (rotary_encoder_handle_t)pvParameters;
    long prev = rotary_encoder_get_count(enc);
    static long enc_accum = 0; // sub-step accumulator (2 raw ticks = 1 action)
    for (;;) {
        long cur = rotary_encoder_get_count(enc);
        
        if (cur != prev) {
            long delta = cur - prev;
#if CONFIG_AMYSYNTH_INPUT_DIAGNOSTICS
            ESP_LOGI(TAG,
                     "encoder count=%ld (delta=%ld)",
                     (long)cur, (long)(delta));
#endif
            prev = cur;

            enc_accum += delta;
            long steps = enc_accum / 2; // require 2 raw ticks per action
            enc_accum %= 2;
            if (steps != 0) {
                encoder_process_steps(steps);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));  // Poll at 50Hz
    }
}

static void encoder_init_task(void *pvParameters)
{
    (void)pvParameters;
    // Delay init to avoid early-boot conflicts (PSRAM, console pins, etc.)
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "[encoder_init] starting after delay");

    rotary_encoder_config_t enc_cfg = rotary_encoder_default_config(ENCODER_PIN_A, ENCODER_PIN_B);
    rotary_encoder_handle_t enc = NULL;
    esp_err_t err = rotary_encoder_new_with_config(&enc_cfg, &enc);
    ESP_LOGI(TAG, "[encoder_init] rotary_encoder_new_with_config returned %d", err);
    if (err == ESP_OK && enc) {
        // 8192: sized when the patch-toggle gesture ran amy_parse_message's
        // ~3.4 KB frame inline on this stack (4096 overflowed); not
        // re-measured since the ingest pump took the parse. Pinned to Core 0
        // so the poll never jitters the Core 1 DSP.
        xTaskCreatePinnedToCore(encoder_task,
             "encoder_task",
             8192,
             enc,
             5,
              NULL,
              0);
    }

    // Status LED last, from this Core-0 init task (the RMT backend allocates
    // its interrupt on the calling core - keep it off the DSP core), and
    // before the heap baseline so its small allocations are part of it.
    status_led_start();

    // One-time post-init heap baseline (deliberately not Kconfig-gated): this
    // runs last in init, so free-internal here is the steady-state figure
    // placement decisions budget against. Piggybacks on this dying init task.
    ESP_LOGI(TAG,
             "post-init heap: internal free=%u largest=%u min_free=%u | psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Where the notable allocations actually landed. Same reasoning as the
    // baseline above: init is done, and placement does not change after this,
    // so once here beats polling.
    diag_mem_report();

    vTaskDelete(NULL);
}

#ifdef GAMMA9001
/* Bind the 'drums' partition (gamma9001 PCM presets 256-391) to the drum
 * cache, which windows the presets in use into PSRAM on demand. The blob
 * itself stays in flash: neither mapped (PSRAM XIP leaves the MMU no room
 * for 3.6 MB) nor copied. All failure paths are non-fatal: those presets
 * just stay disabled. */
static void gamma9001_pcm_mount(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "drums");
    if (part == NULL) {
        ESP_LOGW(TAG, "gamma9001: no 'drums' partition; PCM presets 256+ unavailable");
        return;
    }

    /* A never-flashed partition reads all 0xFF; don't play noise. */
    uint16_t probe[4];
    if (esp_partition_read(part, 0, probe, sizeof(probe)) != ESP_OK ||
        (probe[0] == 0xFFFFu && probe[1] == 0xFFFFu &&
         probe[2] == 0xFFFFu && probe[3] == 0xFFFFu)) {
        ESP_LOGW(TAG, "gamma9001: 'drums' partition is blank (flash drums.bin); "
                      "PCM presets 256+ unavailable");
        return;
    }

    size_t bytes = amy_gamma9001_pcm_bytes();
    if (bytes == 0 || bytes > part->size) {
        ESP_LOGW(TAG, "gamma9001: blob size %u exceeds partition (%u); "
                      "PCM presets 256+ unavailable",
                 (unsigned)bytes, (unsigned)part->size);
        return;
    }

    drum_cache_init(part);
    ESP_LOGI(TAG, "gamma9001: 'drums' partition mounted (%u KB); presets window into PSRAM on demand",
             (unsigned)(bytes / 1024u));
}
#endif /* GAMMA9001 */

void app_main(void)
{
   
    ESP_LOGI(TAG, "Hello world!");
    ESP_LOGI(TAG, "[startup] before i2c_u8g2_init");
    i2c_u8g2_config_t display_cfg = i2c_u8g2_config_default();
    ESP_LOGI(TAG, "[startup] display i2c cfg: SDA=%d SCL=%d Freq=%d Addr=0x%02X Timeout=%dms",
             (int)display_cfg.sda_io_num,
             (int)display_cfg.scl_io_num,
             (int)display_cfg.scl_speed_hz,
             (unsigned int)display_cfg.device_address,
             (int)display_cfg.timeout_ms);

    esp_err_t display_err = i2c_u8g2_init(&s_display, &display_cfg);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "[startup] i2c_u8g2_init failed: %s", esp_err_to_name(display_err));
        return;
    }
    s_u8g2 = i2c_u8g2_get_u8g2(&s_display);
    if (s_u8g2 == NULL) {
        ESP_LOGE(TAG, "[startup] i2c_u8g2_get_u8g2 returned NULL");
        return;
    }
    ESP_LOGI(TAG, "[startup] after i2c_u8g2_init");

  
    // Configure and start AMY
    amy_config_t amy_cfg = amy_default_config();
    amy_cfg.audio = AMY_AUDIO_IS_NONE;
    /* Our amy_usb_render_task owns the render loop; multicore/multithread
     * must be 0 or amy_platform_init spawns its own fill task, which then
     * notifies app_main instead of our render task - permanent deadlock. */
    amy_cfg.platform.multicore = 0;
    amy_cfg.platform.multithread = 0;
    amy_cfg.amy_external_sequencer_hook = main_sequencer_tick_hook;
    /* Loop bounce capture: sums the source buses post-FX, under the lock. */
    amy_cfg.amy_external_bus_postprocess_hook = clip_bounce_bus_hook;
    /* Every AMY arena gets an explicit cap: CONFIG_SPIRAM_USE_MALLOC is off,
     * so plain malloc never reaches PSRAM and nothing auto-places by size -
     * the caps below are the placement decision. Render-hot state stays
     * internal: synth[]/msynth[], the delta pool and the block buffers are
     * all dereferenced every block. */
    amy_cfg.ram_caps_events = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    amy_cfg.ram_caps_synth  = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    amy_cfg.ram_caps_block  = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    amy_cfg.ram_caps_fbl    = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    /* Per-osc synthinfo/msynth blocks: read per-BLOCK (control state), not
     * per-sample - the sample-hot buffers are fbl/block above. The osc arena
     * grows lazily and frees a voice's oscs on release, and a 25-osc/voice
     * patch (built-in piano) grows it by ~46 KB in one apply, too much for
     * the internal pool. PSRAM is cached and per-block access is cheap. */
    amy_cfg.ram_caps_oscs = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    /* Sequencer wire strings are cold control-plane data (read once per
     * fire, parse is us-scale) - PSRAM-first keeps them out of the internal
     * pool (~107 KB free post-init); the sites fall back to ram_caps_events
     * if PSRAM is ever exhausted. */
    amy_cfg.ram_caps_sequencer = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    /* FX delay lines (~108 KB reverb + 256 KB echo) don't fit internal -
     * pinning them internal made allocation fail (and once crashed on the
     * NULL deref). PSRAM latency in the FX stage is acceptable. */
    amy_cfg.ram_caps_delay  = MALLOC_CAP_SPIRAM;
    /* Sample recordings (~140 KB): not render-hot and too large to spend
     * internal on. */
    amy_cfg.ram_caps_sample = MALLOC_CAP_SPIRAM;
    /* Default 256 covers only layer 0. The tag space is laid out in
     * seq_core_config.h (sequencer, arp, ratchet trigs, chord one-shots, chord
     * previews, clip starts) and its top depends on SEQ_CHORD_MAX_NOTES and
     * CLIP_SLOT_COUNT, so this is derived rather than a literal.
     * sequencer_add_wire rejects tag >= this, and the layout comment requires
     * two above the top tag. */
    amy_cfg.max_sequencer_tags = SEQ_CLIP_TAG_MAX + 2;
    /* Slot map lives in synth_slots.h: statics pack 1..SEQ_MEL_SYNTH_BASE-1, melodic is the
     * open-ended arena SEQ_MEL_SYNTH_BASE..SYNTH_SLOT_COUNT-1. This is the polyphony knob. */
    amy_cfg.max_synths = SYNTH_SLOT_COUNT;
    /* One bus per synth group (fx_bus.h): the buses beyond 0 carry a group
     * only while its Split toggle is on, but the tables are preallocated
     * either way, so this is a fixed cost, not a per-split one. */
    amy_cfg.max_buses = FX_BUS_COUNT;
    /* Karplus-Strong rings. One ring is one physical string: two voices on a
     * ring damp each other's decay, and a note-on refills the ring under any
     * voice still reading it (a silent note-on measured +134% on the voices
     * already sounding). So ks_oscs is the count of KS voices that can sound
     * at once before ks_alloc_row() starts stealing the quietest ring. All
     * rings are allocated in amy_start() and held for the life of AMY; a
     * ring is AMY_SAMPLE_RATE/55+1 SAMPLEs, ~3.5 KB at 48 kHz.
     * CONFIG_SEQ_KS_RINGS overrides; 0 sizes for SEQ_KS_LAYERS melodic layers
     * with every row at the SEQ_KS_VOICES_MAX ceiling, plus - in PSRAM,
     * where the room is - the arp's and the live voice's KS voices. */
#if CONFIG_SEQ_KS_RINGS_IN_PSRAM
    amy_cfg.ram_caps_ks = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    amy_cfg.ram_caps_ks = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
#if CONFIG_SEQ_KS_RINGS > 0
    amy_cfg.ks_oscs = CONFIG_SEQ_KS_RINGS;
#else
    {
        uint16_t rings = sequencer_core_ks_row_demand(CONFIG_SEQ_KS_LAYERS);
#if CONFIG_SEQ_KS_RINGS_IN_PSRAM
        rings += sequencer_core_arp_voices();
#if CONFIG_SYNTH_WIRELESS
        rings += live_play_num_voices();
#endif
#endif
        amy_cfg.ks_oscs = (uint8_t)rings;
    }
#endif
    ESP_LOGI(TAG, "KS rings: %u in %s", (unsigned)amy_cfg.ks_oscs,
             (amy_cfg.ram_caps_ks & MALLOC_CAP_SPIRAM) ? "PSRAM" : "internal");
    /* Disable AMY's CPU-overload failsafe (v1.2.121+): its per-block timing
     * span wraps amy_render(), whose body holds amy_queue_lock, so time the
     * render task spends BLOCKED on the lock while the ingest pump applies
     * patch loads is billed as render load. Sustained patch scrolling pins
     * the smoothed estimate past the 98% threshold on wall time alone and
     * the failsafe wipes every osc, note, and sequencer tag out from under
     * the app. Real overload on this platform is already observable: the
     * strict 1:1 GPTimer render pacing plus the USB output-level watchdog.
     * threshold 0 => overload_threshold_us 0 => amy_overload_check no-ops. */
    amy_cfg.overload_threshold = 0.0f;
#ifdef GAMMA9001
    gamma9001_pcm_mount();
#endif

    ESP_LOGI(TAG, "Starting AMY synth engine... (audio=%d, Fs=%d)", amy_cfg.audio, AMY_SAMPLE_RATE);
    ESP_LOGI(TAG, "[startup] before amy_start");
    DIAG_HEAP_CHECK("before amy_start");
    amy_start(amy_cfg);
    DIAG_HEAP_CHECK("after amy_start");

    /* App wavetables become AMY memory presets; pcm_init() inside amy_start
     * empties that list, so this must follow it. */
    wavetable_bank_init();
    DIAG_HEAP_CHECK("after wavetable_bank_init");
#if CONFIG_SYNTH_CUSTOM_WT
    /* The custom table's preset follows the bank's (wavetable_bank.h). */
    wt_builder_init();
    DIAG_HEAP_CHECK("after wt_builder_init");
#endif

    // Characterize profiler timestamp cost before the dumps below; compiles
    // out unless AMY profiling is on.
    amy_profile_overhead_selftest();

    // Our USB Audio (must be after TinyUSB init)
    ESP_ERROR_CHECK(usb_audio_init());
    DIAG_HEAP_CHECK("after usb_audio_init");

    /* Project storage: non-fatal if absent/corrupt. Mounted before
     * synth_ui_init so the snapshot selftest runs single-threaded against the
     * boot layers. */
    project_fs_init();
    if (project_fs_ok()) project_store_cleanup_tmp();
#if CONFIG_SYNTH_PROJECT_SELFTEST
    project_store_selftest();
#endif

    /* synth_ui_init adds the boot layers (drum + first melodic) itself,
     * single-threaded on this task's stack, before the UI task starts. */
    synth_ui_init(s_u8g2);
    DIAG_HEAP_CHECK("after synth_ui_init");

#if CONFIG_SYNTH_WIRELESS
    /* Wire the MIDI transports to the live-play voice. wireless knows
     * nothing of synth_core (P4 portability seam); stop/disconnect hooks
     * release held notes (stuck-note safety). Radio sessions start on demand
     * from the Wireless menu page. */
    static const midi_sink_t s_live_sink = {
        .note_on  = live_play_note_on,
        .note_off = live_play_note_off,
    };
    static const radio_hooks_t s_radio_hooks = {
        .session_start   = live_play_ensure_ready,
        .session_stop    = live_play_all_notes_off,
        .peer_disconnect = live_play_all_notes_off,
    };
    midi_core_set_sink(&s_live_sink);
    radio_manager_init(&s_radio_hooks);
#endif
    ESP_LOGI(TAG, "[startup] after amy_start");
    
    TaskHandle_t amy_render_task_handle = NULL;
    BaseType_t render_task_ok;
#if CONFIG_FREERTOS_UNICORE
    render_task_ok = xTaskCreatePinnedToCore(amy_usb_render_task,
         "amy_render",
         8192,
         NULL,
         23,
         &amy_render_task_handle,
         0);
#else
    // All synthesis runs inside amy_update() in this task; pin it to Core 1,
    // away from USB and esp_timer on Core 0.
    render_task_ok = xTaskCreatePinnedToCore(amy_usb_render_task,
         "amy_render",
         8192,
          NULL,
          22,
          &amy_render_task_handle,
          1); 
#endif
    if (render_task_ok != pdPASS) {
        ESP_LOGW(TAG, "amy_render pinned task create failed (%ld), retrying on core 0", (long)render_task_ok);
        render_task_ok = xTaskCreatePinnedToCore(amy_usb_render_task,
             "amy_render",
             8192,
             NULL,
             22,
             &amy_render_task_handle,
             0);
    }
    if (render_task_ok != pdPASS) {
        ESP_LOGE(TAG, "amy_render task create failed (%ld)", (long)render_task_ok);
    }
    /* Debug builds assert no ingress helper runs on the render body. */
    amy_helpers_set_render_task(amy_render_task_handle);

    ESP_LOGI(TAG, "AMY + USB Audio ready (48 kHz stereo to PC)");

    // Initialize push buttons (pins: my_buttons.c)
    ESP_LOGI(TAG, "[startup] before my_buttons_init");
    s_button_queue = xQueueCreate(BUTTON_QUEUE_DEPTH, sizeof(button_msg_t));
    if (s_button_queue == NULL) {
        ESP_LOGW(TAG, "Button queue creation failed; callbacks will run inline");
    } else {
        // 8192: sized when the patch-toggle path ran amy_parse_message's
        // ~3.4 KB frame inline (4096 overflowed, as for encoder_task); not
        // re-measured since the ingest pump took the parse.
        if (xTaskCreatePinnedToCore(button_handler_task,
             "button_task",
             8192,
             NULL,
              5,
              NULL,
            0) != pdPASS) {
            ESP_LOGW(TAG, "Button handler task creation failed");
            vQueueDelete(s_button_queue);
            s_button_queue = NULL;
        }
    }
    DIAG_HEAP_CHECK("before my_buttons_init");
    esp_err_t btn_err = my_buttons_init();
    if (btn_err != ESP_OK) {
        ESP_LOGE(TAG, "my_buttons_init failed: %s", esp_err_to_name(btn_err));
    } else {
        ESP_LOGI(TAG, "[startup] after my_buttons_init");
        my_buttons_register_cb(main_button_event_cb, NULL);
    }

    // Serial harness last among input paths: hooks need the button queue
    // above; degrade to "harness absent" on any failure. Compiles to nothing
    // when the harness is off (harness_init is a no-op stub there).
    static const harness_hooks_t s_harness_hooks = {
        .inject_button        = harness_inject_button_hook,
        .inject_encoder_steps = harness_inject_encoder_hook,
    };
    if (harness_init(&s_harness_hooks) != ESP_OK) {
        ESP_LOGW(TAG, "serial harness init failed; continuing without it");
    }

    // Defer rotary encoder init to a task to avoid early-boot conflicts.
    xTaskCreatePinnedToCore(encoder_init_task,
         "encoder_init_task",
         2048,
         NULL,
         5,
         NULL,
        0);

    ESP_LOGI(TAG, "Main loop started.");
   
    // Idle diagnostics loop. The heartbeat line stays here because it reports
    // this file's own counters, and the log parser treats it as the marker that
    // opens a reporting window - it must come first. Everything after it is
    // whatever the diagnostics component has registered.
    diag_init();
#if CONFIG_USB_AUDIO_DIAGNOSTICS
    diag_register_reporter("usb-audio", main_log_audio_diagnostics);
#endif
    const uint32_t idle_loop_ms = diag_report_interval_ms();
    while (1) {
        dropout_stats_t drops;
        dropout_stats_get(&drops);
        ESP_LOGI(TAG,
                 "Main loop idle... seq_tick=%" PRIu32 " tick_hook_calls=%" PRIu32 " render_blocks=%" PRIu32 " render_overruns=%" PRIu32 " usb_drops=%" PRIu32 " render_sysclock_ms=%" PRIu32,
                 s_last_seq_tick, s_seq_tick_hook_count, s_render_block_count, drops.render_overrun, drops.ring_overrun,
                 /* computed on demand here, not per render block */ amy_sysclock());
        diag_report_all();
        vTaskDelay(pdMS_TO_TICKS(idle_loop_ms));
    }
}
