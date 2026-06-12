#include "sequencer_ui.h"
#include "priv_u8g2_seq.h"
#include "seq_clamp.h"
#include "sequencer_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "sequencer_ui";

/* Runtime patch cycling list is intentionally small and musical.
 * Values map to AMY built-ins (Juno/DX7/piano). */
static const uint16_t s_melodic_patch_cycle[] = {
    138, /* DX7 E.PIANO 1 */
    135, /* DX7 PIANO 1 */
    141, /* DX7 SYN-LEAD 1 */
    151, /* DX7 FLUTE 1 */
    7,   /* Juno A18 Piano I */
    104, /* Juno B61 E. Piano with Tremolo */
    256, /* Built-in piano */
};
#define SEQ_RUNTIME_PATCH_COUNT ((int)(sizeof(s_melodic_patch_cycle) / sizeof(s_melodic_patch_cycle[0])))

sequencer_ui_state_t seq_state = {
    /* layers[] is zero-initialized by C99 partial-init rules */
    .num_layers       = 0,
    .active_layer_idx = 0,
    .bpm              = 120,
    .current_pattern  = 1,
    .current_step     = 0,
    .playing          = true,
    .selected_track   = 0,
    .selected_step    = 0,
    .edit_mode        = true,
    .drum_select_mode = false,
};

static u8g2_t *s_u8g2 = NULL;

/* Mirror the UI's grid of active steps into the audio core so the core
 * schedules notes for every step the user has toggled on. Only "on" steps are
 * pushed; "off" steps are the core's default after a fresh layer add. */
static void sync_layer_to_core(uint8_t li)
{
    seq_layer_t *layer = &seq_state.layers[li];
    for (int t = 0; t < SEQ_TRACKS; t++) {
        for (int s = 0; s < layer->num_steps; s++) {
            if (layer->grid[t][s]) {
                sequencer_core_set_step(li, t, s, true);
            }
        }
    }
}

static int sequencer_patch_cycle_index_for(uint16_t patch)
{
    for (int i = 0; i < SEQ_RUNTIME_PATCH_COUNT; i++) {
        if (s_melodic_patch_cycle[i] == patch) {
            return i;
        }
    }
    return 0;
}

static void sequencer_ui_sync_melodic_patch_cache(void)
{
    uint16_t patch = sequencer_core_get_melodic_patch();
    for (uint8_t i = 0; i < seq_state.num_layers; i++) {
        if (seq_state.layers[i].type == SEQ_LAYER_MELODIC) {
            seq_state.layers[i].patch = patch;
        }
    }
}

static void sequencer_ui_task(void *pvParameters)
{
    (void)pvParameters;
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t delay = pdMS_TO_TICKS(50); /* 20 Hz */
    for (;;) {
        seq_state.current_step =
            sequencer_core_get_current_step(seq_state.active_layer_idx);
        if (s_u8g2) {
            priv_u8g2_seq_draw_frame(s_u8g2, &seq_state);
        }
        vTaskDelayUntil(&last_wake_time, delay);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void sequencer_ui_init(u8g2_t *u8g2)
{
    s_u8g2 = u8g2;
    seq_state.playing = true;

    sequencer_core_init();

    /* Add drum layer (index 0). */
    sequencer_ui_add_layer(SEQ_LAYER_DRUM, SEQ_STEPS);

    /* Default pattern: 4-on-the-floor hi-hat + backbeat kick. */
    seq_layer_t *drum = &seq_state.layers[0];
    drum->grid[0][0] = drum->grid[0][4] =
    drum->grid[0][8] = drum->grid[0][12] = true;
    drum->grid[1][4] = drum->grid[1][12]  = true;

    sync_layer_to_core(0);
    sequencer_core_set_playing(true);

    xTaskCreate(sequencer_ui_task, "seq_ui", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Sequencer UI + Core initialized");
}

uint8_t sequencer_ui_add_layer(seq_layer_type_t type, uint8_t num_steps)
{
    uint8_t li = sequencer_core_add_layer(type, num_steps);
    if (li == 0xFF) return 0xFF;

    seq_layer_t *layer = &seq_state.layers[li];
    memset(layer, 0, sizeof(seq_layer_t));
    layer->type       = type;
    layer->num_steps  = (num_steps == SEQ_MAX_STEPS) ? SEQ_MAX_STEPS : SEQ_STEPS;
    layer->num_tracks = SEQ_TRACKS;
    layer->step_page  = 0;

    if (type == SEQ_LAYER_MELODIC) {
        layer->patch = sequencer_core_get_melodic_patch();
        /* Default: Cmaj7 voicing — C4 E4 G4 B4 */
        static const uint8_t mel_notes[SEQ_TRACKS] = {60, 64, 67, 71};
        for (int t = 0; t < SEQ_TRACKS; t++) {
            layer->track_base_note[t] = mel_notes[t];
            for (int s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = mel_notes[t];
            }
        }
    } else {
        static const uint8_t drum_notes[SEQ_TRACKS] = {42, 35, 38, 56};
        for (int t = 0; t < SEQ_TRACKS; t++) {
            layer->track_base_note[t] = drum_notes[t];
            for (int s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = drum_notes[t];
            }
        }
    }

    seq_state.num_layers = li + 1;
    ESP_LOGI(TAG, "UI layer %d added (type=%d steps=%d)",
             li, type, layer->num_steps);
    return li;
}

void sequencer_ui_cycle_active_layer(void)
{
    if (seq_state.num_layers <= 1) return;
    seq_state.active_layer_idx =
        (uint8_t)((seq_state.active_layer_idx + 1) % seq_state.num_layers);
    seq_state.selected_track = 0;
    seq_state.selected_step  = 0;
    seq_state.edit_mode      = true;
    ESP_LOGI(TAG, "Active layer -> %d (%s)",
             seq_state.active_layer_idx,
             seq_state.layers[seq_state.active_layer_idx].type == SEQ_LAYER_DRUM
             ? "drum" : "melodic");
}

/* Moves the step cursor by `delta` while in edit mode, or nudges BPM otherwise.
 * The cursor walks the current track's steps; running off either end wraps to
 * the adjacent track (and wraps track index too), so a long turn scans the
 * whole grid track-by-track. */
void sequencer_ui_handle_encoder(long delta)
{
    if (delta == 0) return;

    if (seq_state.edit_mode) {
        uint8_t li        = seq_state.active_layer_idx;
        uint8_t num_steps = seq_state.layers[li].num_steps;
        int new_step      = (int)seq_state.selected_step + (int)delta;

        if (new_step < 0) {
            /* Walked off the start: jump to the last step of the previous track. */
            new_step = (int)num_steps - 1;
            seq_state.selected_track =
                (uint8_t)((seq_state.selected_track + SEQ_TRACKS - 1) % SEQ_TRACKS);
        } else if (new_step >= (int)num_steps) {
            /* Walked off the end: jump to the first step of the next track. */
            new_step = 0;
            seq_state.selected_track =
                (uint8_t)((seq_state.selected_track + 1) % SEQ_TRACKS);
        }
        seq_state.selected_step = (uint8_t)new_step;

        /* 32-step layers display 16 steps per page; keep the cursor visible by
         * selecting the page (0 or 1) that contains the new step. */
        if (num_steps == SEQ_MAX_STEPS) {
            seq_state.layers[li].step_page = (uint8_t)(new_step / 16);
        }
    } else {
        sequencer_ui_set_bpm((uint16_t)((int)seq_state.bpm + (int)delta));
    }
}

/* Encoder push: in edit mode toggles the step under the cursor on/off (and
 * mirrors that to the core); otherwise it acts as a play/pause toggle. */
void sequencer_ui_handle_button(void)
{
    if (seq_state.edit_mode) {
        uint8_t li = seq_state.active_layer_idx;
        uint8_t t  = seq_state.selected_track;
        uint8_t s  = seq_state.selected_step;
        seq_state.layers[li].grid[t][s] = !seq_state.layers[li].grid[t][s];
        sequencer_core_set_step(li, t, s, seq_state.layers[li].grid[t][s]);
    } else {
        seq_state.playing = !seq_state.playing;
        sequencer_core_set_playing(seq_state.playing);
    }
}

void sequencer_ui_toggle_playing(void)
{
    seq_state.playing = !seq_state.playing;
    sequencer_core_set_playing(seq_state.playing);
    ESP_LOGI(TAG, "Playback %s", seq_state.playing ? "started" : "stopped");
}

void sequencer_ui_set_bpm(uint16_t bpm)
{
    bpm = SEQ_CLAMP_U16(bpm, 40, 300);
    seq_state.bpm = bpm;
    sequencer_core_set_bpm(bpm);
}

/* Transposes the selected track's note by `delta` semitones. We read/write the
 * *source* note (the user's raw choice) so repeated nudges accumulate cleanly;
 * the core may quantize it, so we read back the resolved note for display. */
void sequencer_ui_adjust_track_note(int delta)
{
    uint8_t li    = seq_state.active_layer_idx;
    uint8_t track = seq_state.selected_track;
    uint8_t note  = sequencer_core_get_track_source_note(li, track);
    uint8_t new_note = SEQ_CLAMP_U8(note + delta, 0, 127);
    sequencer_core_set_track_midi_note(li, track, new_note);
    /* Keep display in sync with resolved note after core clamp/quantize. */
    seq_state.layers[li].track_base_note[track] =
        sequencer_core_get_track_midi_note(li, track);
}

void sequencer_ui_set_drum_select_mode(bool held)
{
    seq_state.drum_select_mode = held;
}

void sequencer_ui_set_patch_select_mode(bool held)
{
    seq_state.patch_select_mode = held;
}

void sequencer_ui_cycle_melodic_patch(int delta)
{
    if (delta == 0) return;

    uint8_t li = seq_state.active_layer_idx;
    if (li >= seq_state.num_layers) return;
    if (seq_state.layers[li].type != SEQ_LAYER_MELODIC) return;

    uint16_t current = sequencer_core_get_melodic_patch();
    int idx = sequencer_patch_cycle_index_for(current);
    int n = SEQ_RUNTIME_PATCH_COUNT;
    int shift = delta % n;
    int next = (idx + shift + n) % n;

    sequencer_core_set_melodic_patch(s_melodic_patch_cycle[next]);
    sequencer_ui_sync_melodic_patch_cache();
    ESP_LOGI(TAG, "melodic patch cycle -> %u", (unsigned)sequencer_core_get_melodic_patch());
}


