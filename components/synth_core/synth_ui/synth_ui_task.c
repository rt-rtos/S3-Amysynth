#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"
#include "custompatches/fm_voice.h"
#include "custompatches/additive_voice.h"
#include "custompatches/sample_rec.h"
#include "custompatches/clip_bounce.h"
#include "custompatches/clip_player.h"
#include "custompatches/drum_cache.h"
#include "priv_i2c_u8g2.h"   /* i2c_u8g2_service - absent-panel recovery */
#include "display_seq.h"
#include "display_drone.h"
#include "display_prog.h"
#include "display_menu.h"
#include "display_arp.h"
#include "display_hint.h"
#include "display_badge.h"
#include "display_flush.h"
#include "synth_ui_hint.h"
#include "usb_audio_watchdog.h"
#include "amy_helpers.h"
#include "project_snapshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "seq_clamp.h"
#include "esp_log.h"
#include "diag_heap.h"
#include "sdkconfig.h"
#if CONFIG_SYNTH_WIRELESS
#include "radio_manager.h"
#include "live_play.h"      /* live_play_lfo_service (20 Hz software stepper) */
#endif
#include <string.h>

static const char *TAG_TASK = "synth_ui";

static u8g2_t *s_u8g2 = NULL;

/* Set by mode/layout transitions to force one redraw regardless of the
 * render-on-change signature (guards first-frame-after-transition staleness). */
volatile bool s_force_redraw = true;

/* Deferred layer-delete request, consumed by synth_ui_task each frame. */
static volatile bool    s_layer_delete_pending = false;
static volatile uint8_t s_layer_delete_idx     = 0;

/* Deferred layer-add request, consumed by synth_ui_task each frame. The add
 * path (memset + patch-string parse via amy_send_patch) is too heavy for the
 * esp_timer task's 3584-byte stack, so it defers here like delete. */
static volatile bool    s_layer_add_pending    = false;

/* Deferred row-count change (DEV "Rows"), consumed by synth_ui_task each
 * frame: a row-count change is a structural s_layers edit, so it runs on the
 * applier, not the encoder task the DEV menu's input arrives on. */
static volatile bool    s_layer_rows_pending   = false;
static volatile uint8_t s_layer_rows_idx       = 0;
static volatile uint8_t s_layer_rows_count     = 0;

static uint32_t s_last_sig = 0;
/* Which top-level view was rendered last frame; a change forces a redraw. */
static ui_view_id_t s_last_view = UI_VIEW_SEQ;

static void ui_services(void)
{
    /* Absent-panel recovery: no-op while the display is present, one
     * sparse probe otherwise. A (re)attached panel comes back with blank
     * RAM, so it forces a full redraw/flush. */
    if (i2c_u8g2_service()) {
        s_force_redraw = true;
    }
    /* Coalesced arp re-emit: setters mark the arp dirty, at most one full
     * re-emit per frame lands here, collapsing fast encoder edits. */
    arp_core_service();
    /* Drone: advance the tempo-locked filter sweep and keep the LFO in
     * sync. Cheap no-op while the drone is disabled. */
    drone_core_service();
    /* Normal drone: drain its coalesced rebuild (no tick machinery). */
    drone_std_core_service();
    sequencer_core_lfo_service();
    {
        /* A Prog Gen row request drained here grows layers in the core;
         * mirror them back. */
        uint8_t grown = sequencer_core_progression_service();
        for (uint8_t li = 0; grown && li < seq_state.num_layers; li++) {
            if (grown & (1u << li)) synth_ui_reexport_layer(li);
        }
    }
    clip_bounce_service();
    clip_player_service();
    drum_cache_service();
#if CONFIG_SEQ_OOM_RESYNC
    /* Re-emit schedules once an AMY OOM burst settles (dropped wire
     * events otherwise leave tracks mute); cheap counter poll otherwise. */
    sequencer_core_oom_service();
#endif
#if CONFIG_SYNTH_WIRELESS
    /* Live voice: 20 Hz software LFO for patch-string (non-native)
     * patches; cheap no-op otherwise. */
    live_play_lfo_service();
#endif

    /* Deferred layer-delete: must run here so the array compaction is
     * serialized against the other seq_state readers on Core 0. Drained
     * BEFORE add, so no add lands in a slot delete has not yet freed. */
    if (s_layer_delete_pending) {
        s_layer_delete_pending = false;
        uint8_t del_idx = s_layer_delete_idx;
        if (sequencer_core_delete_layer(del_idx)) {
            /* Mirror compaction in the UI-side seq_state. */
            uint8_t tail = (uint8_t)(seq_state.num_layers - del_idx - 1);
            if (tail > 0) {
                memmove(&seq_state.layers[del_idx],
                        &seq_state.layers[del_idx + 1],
                        tail * sizeof(seq_state.layers[0]));
            }
            seq_state.num_layers--;
            /* Clamp indices that may now point past the end. */
            if (seq_state.active_layer_idx >= seq_state.num_layers)
                seq_state.active_layer_idx = (uint8_t)(seq_state.num_layers - 1);
            if (s_graph_layer >= seq_state.num_layers)
                s_graph_layer = (uint8_t)(seq_state.num_layers - 1);
            /* Drop the Step Trig overlay: after compaction its cached
             * (layer,track,step) may name a different layer's steps. */
            synth_ui_stepedit_close();
            ESP_LOGI(TAG_TASK, "UI delete layer L%u (%u layers remain)",
                     del_idx + 1u, seq_state.num_layers);
        }
    }

    /* Deferred layer-add (see synth_ui_request_add_layer()). */
    if (s_layer_add_pending) {
        s_layer_add_pending = false;
        /* Re-check the cap; num_layers may have moved since the request. */
        if (seq_state.num_layers < MAX_LAYERS) {
            synth_ui_add_layer(SEQ_LAYER_MELODIC, SEQ_STEPS);
        }
    }

    if (s_layer_rows_pending) {
        s_layer_rows_pending = false;
        uint8_t li = s_layer_rows_idx;
        if (li < seq_state.num_layers &&
            sequencer_core_set_layer_tracks(li, s_layer_rows_count)) {
            synth_ui_reexport_layer(li);
        }
    }

#if CONFIG_SYNTH_PROJECT_STORE
    /* Project load/save queued by the Projects menu (radio_manager.h
     * explains the queued-click pattern). Runs here as the s_layers applier
     * (sequencer_core.h), after the drains above so pending structural
     * edits resolve before a load replaces them. */
    projects_menu_service();
#endif

    /* Bounce page redraw pump: the recorder and the clip slots advance on
     * the render task, so the page polls them here. No-op while closed. */
    bounce_menu_service();

#if CONFIG_SYNTH_WIRELESS
    /* Radio session start/stop queued by the Wireless page (see
     * radio_manager.h for why clicks defer to this task). */
    radio_manager_service();
#endif

    /* Output-level watchdog: peeks the USB ring on this core/task (see
     * usb_audio_watchdog.h). Compiles out with its Kconfig gate, as do the
     * badge draw and signature mix below. */
    output_wd_poll();

#if CONFIG_SYNTH_DEV_MENU
    /* DEV status bars (heap, dropout): throttled sampling; their text
     * joins the signature mix below so the bar refreshes on change. */
    synth_ui_dev_heapbar_poll();
    synth_ui_dev_dropbar_poll();
#endif

    /* Trailing flush for throttled editor live-previews (amp trim), so the
     * last encoder value lands even after the user stops turning. */
    synth_ui_editors_live_service();

    /* Decay the FM-algorithm banner; the frame after it expires must
     * repaint to clear it (the render-gate hash doesn't see it). */
    if (seq_state.algo_banner_ticks > 0) {
        if (--seq_state.algo_banner_ticks == 0) s_force_redraw = true;
    }
}

static void ui_render(void)
{
    seq_state.current_step =
        sequencer_core_get_current_step(seq_state.active_layer_idx);
    if (s_u8g2) {
        /* Precedence: synth_ui_active_view() (synth_ui.h). signature()
         * builds the active view into vw and returns the FNV render-gate
         * hash; the draw below reuses vw. Adding a view is one row in
         * ui_view_table[]. */
        ui_view_id_t view = synth_ui_active_view();
        const ui_view_desc_t *desc = &ui_view_table[view];
        ui_view_vw_t vw;
        uint32_t sig = desc->signature(&vw);
        /* The watchdog badge participates in the render gate so it
         * appears/clears without needing any other screen change. */
        sig ^= (uint32_t)output_wd_state() * 0x9E3779B9u;
        /* REC badge: a bounce arming, recording or folding its tail. */
        sig ^= (uint32_t)clip_bounce_get_state() * 0xC2B2AE35u;
#if CONFIG_SYNTH_WIRELESS
        /* BLE badge participates too: appears/changes on session or
         * connection state without any other screen change. */
        sig ^= ((uint32_t)radio_manager_state() * 2u +
                (radio_manager_connected() ? 1u : 0u)) * 0x85EBCA6Bu;
#endif
#if CONFIG_SYNTH_DEV_MENU
        /* DEV status bars participate too (0 while off). */
        sig ^= synth_ui_dev_heapbar_sig();
        sig ^= synth_ui_dev_dropbar_sig();
#endif
        bool force = s_force_redraw || (view != s_last_view);

        if (force || sig != s_last_sig) {
            desc->draw(s_u8g2, &vw);
            /* Persistent button-hint strip, composited over whatever the
             * view drew, so no per-screen renderer needs to know about it.
             * INVARIANT: every screen's draw_frame only FILLS the buffer;
             * the single flush below is the one physical transfer per
             * redraw. Sending twice (without the hint, then with) makes the
             * strip visibly flicker on every redraw. */
#if CONFIG_SYNTH_DEV_MENU
            /* A DEV status bar (ui_screen_dev.c) claims the strip on every
             * screen while on. */
            if (synth_ui_dev_heapbar_active()) {
                display_hint_draw(s_u8g2, synth_ui_dev_heapbar_text());
            } else if (synth_ui_dev_dropbar_active()) {
                display_hint_draw(s_u8g2, synth_ui_dev_dropbar_text());
            } else
#endif
            if (synth_ui_hint_visible()) {
                display_hint_draw(s_u8g2, synth_ui_hint_text());
            }
            /* Output-level warning badge, top-right, composited last so it
             * overlays every screen; part of the single physical send. */
            output_wd_state_t owd = output_wd_state();
            if (owd != OUTPUT_WD_OK) {
                const char *owd_txt =
                    (owd == OUTPUT_WD_CLIPPING) ? "CLIP" : "LOUD";
                u8g2_SetFont(s_u8g2, u8g2_font_4x6_tr);
                u8g2_SetDrawColor(s_u8g2, 1);
                u8g2_DrawBox(s_u8g2, 108, 0, 20, 8);
                u8g2_SetDrawColor(s_u8g2, 0);
                u8g2_DrawStr(s_u8g2, 110, 7, owd_txt);
                u8g2_SetDrawColor(s_u8g2, 1);
            }
#if CONFIG_SYNTH_WIRELESS
            /* BLE badge, last: it reads the finished buffer back
             * (display_badge.h). */
            if (radio_manager_state() == RADIO_ACTIVE) {
                display_badge_draw(s_u8g2, ui_view_table[view].badge_x,
                                   radio_manager_connected());
            }
#endif
            /* Loop-bounce REC badge while a bounce is armed, recording or
             * folding its tail; same placement rules as the BLE badge. */
            if (clip_bounce_get_state() != CLIP_BOUNCE_IDLE) {
                display_badge_draw_text(s_u8g2, ui_view_table[view].badge_x, "REC");
            }
            if (force) display_flush_invalidate();
            display_flush(s_u8g2);
            s_last_sig = sig;
            s_last_view = view;
            s_force_redraw = false;
        }
    }
}

#define UI_STEP_POLL_MS       10
#define UI_SLICES_PER_FRAME   5     /* services and the render gate keep their 50 ms frame */

static void synth_ui_task(void *pvParameters)
{
    (void)pvParameters;
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t slice = pdMS_TO_TICKS(UI_STEP_POLL_MS);
    uint8_t phase = 0;
    for (;;) {
        if (phase == 0) {
            ui_services();
            ui_render();
        } else if (sequencer_core_get_current_step(seq_state.active_layer_idx)
                   != seq_state.current_step) {
            /* Playhead moved between frames: render now instead of at the next 50 ms slot. */
            ui_render();
        }
        phase = (uint8_t)((phase + 1) % UI_SLICES_PER_FRAME);
        vTaskDelayUntil(&last_wake_time, slice);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

/* Sequencer solo is global, and the arp and drones are not sequencer tracks -
 * they run off their own modules, which sequencer_core does not (and should
 * not) know about. This is the join: the core reports "something is soloed",
 * and the app layer silences the voices it owns. The live-play voice is left
 * sounding on purpose - playing over a soloed row is why one solos it. */
static void ui_solo_change(bool any_solo)
{
    arp_set_solo_muted(any_solo);
    drone_set_solo_muted(any_solo);
    drone_std_set_solo_muted(any_solo);
}

/* The same join for the transport: the arp is re-armed by the core itself,
 * the drones are paused here. */
static void ui_transport_change(bool playing)
{
    drone_set_paused(!playing);
    drone_std_set_paused(!playing);
}

void synth_ui_init(u8g2_t *u8g2)
{
    s_u8g2 = u8g2;
    synth_ui_state_alloc();   /* must precede every seq_state access */
    seq_state.playing  = true;
    seq_state.ui_mode  = UI_MODE_SEQUENCER;
    seq_state.menu_open = false;
    seq_state.menu_cursor = 0;
    seq_state.menu_editing = false;

    DIAG_HEAP_CHECK("ui_init: entry");
    amy_helpers_init();
    sequencer_core_init();
    DIAG_HEAP_CHECK("ui_init: after sequencer_core_init");
    arp_core_init();
    DIAG_HEAP_CHECK("ui_init: after arp_core_init");
    drone_core_init();
    DIAG_HEAP_CHECK("ui_init: after drone_core_init");
    drone_std_core_init();
    DIAG_HEAP_CHECK("ui_init: after drone_std_core_init");
    /* After the three module inits: the hook may fire as soon as it is set. */
    sequencer_core_set_solo_change_cb(ui_solo_change);
    sequencer_core_set_transport_change_cb(ui_transport_change);
#if CONFIG_SYNTH_CUSTOM_FM
    fm_voice_default(&s_fm_voice);
#endif
#if CONFIG_SYNTH_ADDITIVE
    additive_voice_default(&s_additive_voice);
#endif
    sample_rec_init();
    clip_player_init();
    clip_bounce_init();
    DIAG_HEAP_CHECK("ui_init: after sample_rec_init");

    /* Add drum layer (index 0). */
    synth_ui_add_layer(SEQ_LAYER_DRUM, SEQ_STEPS);
    DIAG_HEAP_CHECK("ui_init: after add_layer(drum)");

    /* Default pattern: 4-on-the-floor house groove across all 4 tracks so the
     * boot loop is immediately musical; the velocity accent/jitter engine adds
     * the feel.
     *   track 0 kick : every quarter
     *   track 1 snare: backbeat (2 & 4)
     *   track 2 hat  : off-beat 8ths
     *   track 3 perc : light syncopation */
    seq_layer_t *drum = &seq_state.layers[0];
    drum->grid[0][0]  = drum->grid[0][4]  =
    drum->grid[0][8]  = drum->grid[0][12] = true;   /* kick  */
    drum->grid[1][4]  = drum->grid[1][12] = true;   /* snare */
    drum->grid[2][0]  = drum->grid[2][2]  = drum->grid[2][4]  = drum->grid[2][6]  =
    drum->grid[2][8]  = drum->grid[2][10] = drum->grid[2][12] = drum->grid[2][14] = true; /* hats: all 8ths */
    drum->grid[3][7]  = drum->grid[3][15] = true;   /* perc  */

    sync_layer_to_core(0);
    DIAG_HEAP_CHECK("ui_init: after sync_layer_to_core(0)");
    sequencer_core_set_playing(true);
    DIAG_HEAP_CHECK("ui_init: after set_playing");

    /* First melodic layer. Added HERE, before the UI task exists: running
     * single-threaded on the main stack, before the applier is registered,
     * satisfies the single-applier contract with no cross-task handoff. */
    synth_ui_add_layer(SEQ_LAYER_MELODIC, SEQ_STEPS);
    DIAG_HEAP_CHECK("ui_init: after add_layer(melodic)");

#if CONFIG_SYNTH_PROJECT_SELFTEST
    /* Snapshot round-trip against the boot-default state. Must run HERE -
     * after the boot layers exist, before seq_ui is registered as the applier -
     * because the load phase rebuilds layer topology and would trip the applier
     * assert from any other task. The load leaves the transport stopped. */
    project_snapshot_selftest();
    sequencer_core_set_playing(true);
    seq_state.playing = true;
#endif

    /* Pin to Core 0: the OLED refresh does blocking I2C and is not latency
     * critical, so keep it off Core 1 where the AMY DSP runs. 8192 stack:
     * sized when the deferred Add-Layer drain ran amy_parse_message's ~3.4 KB
     * frame inline here (4096 could not absorb it); not re-measured since the
     * ingest pump took the parse. */
    TaskHandle_t ui_task = NULL;
    xTaskCreatePinnedToCore(synth_ui_task, "seq_ui", 8192, NULL, 5, &ui_task, 0);
    /* From here this task is the single applier for structural s_layers edits;
     * debug builds assert any add/delete_layer from another task. Other
     * contexts use synth_ui_request_add_layer()/_delete_active_layer(). */
    sequencer_core_set_layers_applier(ui_task);
    ESP_LOGI(TAG_TASK, "Sequencer UI + Core initialized");
}

uint8_t synth_ui_add_layer(seq_layer_type_t type, uint8_t num_steps)
{
    uint8_t li = sequencer_core_add_layer(type, num_steps);
    if (li == 0xFF) return 0xFF;

    seq_layer_t *layer = &seq_state.layers[li];
    memset(layer, 0, sizeof(seq_layer_t));
    layer->type       = type;
    layer->num_steps  = (num_steps == SEQ_MAX_STEPS) ? SEQ_MAX_STEPS : SEQ_STEPS;
    layer->num_tracks = sequencer_core_get_layer_tracks(li);

    if (type == SEQ_LAYER_MELODIC) {
        layer->patch       = sequencer_core_get_layer_patch(li);
        layer->patch_scope = sequencer_core_get_patch_scope(li);
        /* Default voicing from the core, the single source of the seeded
         * base notes (Cmaj7 C4 E4 G4 B4, D5 on a fifth row). */
        for (int t = 0; t < SEQ_TRACKS; t++) {
            uint8_t note = sequencer_core_get_track_source_note(li, t);
            layer->track_base_note[t] = note;
            /* track_patch[] is the per-row store in both scopes; mirror it so
             * the header and the patch overlay read the row the cursor is on. */
            layer->track_patch[t] = sequencer_core_get_melodic_track_patch(li, t);
            for (int s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = note;
            }
        }
    } else {
        /* Drums are per-track patches with role-based default pitches. Read
         * both from the core (the single source of truth) rather than mirroring
         * them here, so labels and pitch display cannot drift. */
        for (int t = 0; t < SEQ_TRACKS; t++) {
            uint8_t note = sequencer_core_get_track_source_note(li, t);
            layer->track_base_note[t] = note;
            layer->track_patch[t] = sequencer_core_get_drum_patch(li, t);
            for (int s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = note;
            }
        }
        layer->patch = layer->track_patch[0];
    }

    seq_state.num_layers = li + 1;
    ESP_LOGI(TAG_TASK, "UI layer L%d added (type=%d steps=%d)",
             li + 1, type, layer->num_steps);
    return li;
}

/* Refresh one layer of the UI mirror from the core after the core changed its
 * shape (row count), and keep the cursors on a row that exists. */
void synth_ui_reexport_layer(uint8_t li)
{
    if (li >= seq_state.num_layers) return;
    sequencer_core_export_layer(li, &seq_state.layers[li]);
    uint8_t n = seq_state.layers[li].num_tracks;
    if (li == seq_state.active_layer_idx && seq_state.selected_track >= n) {
        seq_state.selected_track = (uint8_t)(n - 1);
    }
    s_force_redraw = true;
}

/* Request a row-count change for layer li; synth_ui_task applies it next
 * frame and re-exports the layer. Safe from any Core-0 context. */
void synth_ui_request_layer_tracks(uint8_t li, uint8_t num_tracks)
{
    if (li >= seq_state.num_layers) return;
    s_layer_rows_idx     = li;
    s_layer_rows_count   = num_tracks;
    s_layer_rows_pending = true;
}

/* Request a melodic layer add. synth_ui_task drains the flag next frame so the
 * heavy work (memset + patch-string parse) runs on its 8192-byte stack, not the
 * 3584-byte esp_timer stack. Safe from any Core-0 context. */
void synth_ui_request_add_layer(void)
{
    if (seq_state.num_layers >= MAX_LAYERS) return;
    s_layer_add_pending = true;
}

/* Schedule a delete of the layer the grid is showing. The pending flag
 * serializes array compaction on Core 0 against the other seq_state readers. */
void synth_ui_request_delete_active_layer(void)
{
    uint8_t layer_idx = seq_state.active_layer_idx;
    /* Drum layer (0) is permanent; must always keep at least one layer. */
    if (layer_idx == 0 || seq_state.num_layers <= 1) return;
    if (layer_idx >= seq_state.num_layers) return;
    if (seq_state.layers[layer_idx].type == SEQ_LAYER_DRUM) return;
    s_layer_delete_idx     = layer_idx;
    s_layer_delete_pending = true;
}

void synth_ui_cycle_active_layer(void)
{
    if (seq_state.num_layers <= 1) return;
    /* Close the Step Trig overlay first: it edits the (layer,track,step) under
     * the cursor that is about to move. */
    synth_ui_stepedit_close();
    seq_state.active_layer_idx =
        (uint8_t)((seq_state.active_layer_idx + 1) % seq_state.num_layers);
    seq_state.selected_track = 0;
    seq_state.selected_step  = 0;
    ESP_LOGI(TAG_TASK, "Active layer -> L%d (%s)",
             seq_state.active_layer_idx + 1,
             seq_state.layers[seq_state.active_layer_idx].type == SEQ_LAYER_DRUM
             ? "drum" : "melodic");
}

/* Move the step cursor by `delta`. Running off either end wraps to the
 * adjacent track, so a long turn scans the whole grid. */
void synth_ui_handle_encoder(long delta)
{
    if (delta == 0) return;

    uint8_t li         = seq_state.active_layer_idx;
    uint8_t num_steps  = seq_state.layers[li].num_steps;
    uint8_t num_tracks = seq_state.layers[li].num_tracks;
    int new_step       = (int)seq_state.selected_step + (int)delta;

    if (new_step < 0) {
        /* Off the start: last step of the previous track. */
        new_step = (int)num_steps - 1;
        seq_state.selected_track =
            (uint8_t)((seq_state.selected_track + num_tracks - 1) % num_tracks);
    } else if (new_step >= (int)num_steps) {
        /* Off the end: first step of the next track. */
        new_step = 0;
        seq_state.selected_track =
            (uint8_t)((seq_state.selected_track + 1) % num_tracks);
    }
    seq_state.selected_step = (uint8_t)new_step;
}

bool synth_ui_set_layer_steps(uint8_t li, uint8_t num_steps)
{
    if (li >= seq_state.num_layers) return false;
    seq_layer_t *layer = &seq_state.layers[li];
    if (layer->num_steps == num_steps) return true;
    if (!sequencer_core_set_layer_steps(li, num_steps)) return false;
    /* Mirror what the core did to its own copy. */
    if (num_steps == SEQ_MAX_STEPS) {
        for (uint8_t t = 0; t < layer->num_tracks; t++) seq_layer_copy_first_half(layer, t);
    }
    layer->num_steps = num_steps;
    if (seq_state.selected_step >= num_steps) seq_state.selected_step = (uint8_t)(num_steps - 1);
    return true;
}

/* Toggle the grid step under the cursor and mirror it to the core. Returns
 * whether a step was toggled. */
bool synth_ui_toggle_step_at_cursor(void)
{
    uint8_t li = seq_state.active_layer_idx;
    uint8_t t  = seq_state.selected_track;
    uint8_t s  = seq_state.selected_step;
    seq_state.layers[li].grid[t][s] = !seq_state.layers[li].grid[t][s];
    sequencer_core_set_step(li, t, s, seq_state.layers[li].grid[t][s]);
    return true;
}

/* Encoder push: in edit mode toggles the step under the cursor on/off;
 * otherwise it acts as a play/pause toggle. */
void synth_ui_handle_button(void)
{
    if (!synth_ui_toggle_step_at_cursor()) {
        seq_state.playing = !seq_state.playing;
        sequencer_core_set_playing(seq_state.playing);
    }
}

void synth_ui_toggle_playing(void)
{
    seq_state.playing = !seq_state.playing;
    sequencer_core_set_playing(seq_state.playing);
    ESP_LOGI(TAG_TASK, "Playback %s", seq_state.playing ? "started" : "stopped");
}

void synth_ui_set_bpm(uint16_t bpm)
{
    sequencer_core_set_bpm(bpm);
}

/* Transpose the selected track by `delta` semitones. Reads/writes the SOURCE
 * note (the user's raw choice) so repeated nudges accumulate cleanly, then
 * reads back the resolved note for display since the core may quantize.
 * Melodic tracks scroll a wrapping virtual list: C1..C7 then the defined chord
 * presets (seq_chords_selector_step). Drum tracks take a plain clamped nudge. */
void synth_ui_adjust_track_note(int delta)
{
    uint8_t li    = seq_state.active_layer_idx;
    uint8_t track = seq_state.selected_track;
    uint8_t note  = sequencer_core_get_track_source_note(li, track);

    if (li < seq_state.num_layers &&
        seq_state.layers[li].type == SEQ_LAYER_MELODIC) {
        int dir = (delta > 0) ? 1 : -1;
        for (int k = (delta > 0 ? delta : -delta); k > 0; k--) {
            note = seq_chords_selector_step(note, dir);
        }
        sequencer_core_set_track_midi_note(li, track, note);
    } else {
        uint8_t new_note = SEQ_CLAMP_U8(note + delta, 0, 127);
        sequencer_core_set_track_midi_note(li, track, new_note);
    }
    /* Sync display to the resolved note after core clamp/quantize (a chord
     * assignment reads back as its CHn sentinel). */
    seq_state.layers[li].track_base_note[track] =
        sequencer_core_get_track_midi_note(li, track);
}

void synth_ui_set_drum_select_mode(bool held)
{
    seq_state.drum_select_mode = held;
}

void synth_ui_set_patch_select_mode(bool held)
{
    seq_state.patch_select_mode = held;
}
