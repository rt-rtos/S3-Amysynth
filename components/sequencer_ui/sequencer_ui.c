#include "sequencer_ui.h"
#include "priv_u8g2_seq.h"
#include "sequencer_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "sequencer_ui";

sequencer_ui_state_t seq_state = {
    .grid = {{0}},
    .bpm = 120,
    .current_pattern = 1,
    .current_step = 0,
    .playing = true,
    .selected_track = 0,
    .selected_step = 0,
    .edit_mode = true,
};

static u8g2_t *s_u8g2 = NULL;

// Copy UI grid → core grid (called once at init and after every edit)
static void sync_grid_to_core(void) {
    for (int t = 0; t < SEQ_TRACKS; t++) {
        for (int s = 0; s < SEQ_STEPS; s++) {
            if (seq_state.grid[t][s]) {
                sequencer_core_set_step(t, s, true);
            }
        }
    }
}

static void sequencer_ui_task(void *pvParameters) {
    (void)pvParameters;
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t delay = pdMS_TO_TICKS(50); // 20 Hz UI refresh

    for (;;) {
        seq_state.current_step = sequencer_core_get_current_step();
        if (s_u8g2) {
            priv_u8g2_seq_draw_frame(s_u8g2, (priv_u8g2_seq_state_t*)&seq_state);   // unchanged — still uses seq_state.grid
        }
        vTaskDelayUntil(&last_wake_time, delay);
    }
}

void sequencer_ui_init(u8g2_t *u8g2) {
    s_u8g2 = u8g2;
    seq_state.playing = true;

    // Default pattern (same as before)
    seq_state.grid[0][0] = seq_state.grid[0][4] = seq_state.grid[0][8] = seq_state.grid[0][12] = true;
    seq_state.grid[1][4] = seq_state.grid[1][12] = true;

    // Start the real-time core
    sequencer_core_init();

    // Sync initial pattern to core
    sync_grid_to_core();

    // No transport button yet: force loop-play at boot.
    sequencer_core_set_playing(true);

    // Start UI task
    xTaskCreate(sequencer_ui_task, "seq_ui", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Sequencer UI + Core initialized");
}

void sequencer_ui_handle_encoder(long delta) {
    if (delta == 0) return;

    if (seq_state.edit_mode) {
        // Move selection (wrap around tracks & steps)
        int new_step = (int)seq_state.selected_step + delta;
        if (new_step < 0) {
            new_step = 15;
            seq_state.selected_track = (seq_state.selected_track + 3) % 4;
        } else if (new_step > 15) {
            new_step = 0;
            seq_state.selected_track = (seq_state.selected_track + 1) % 4;
        }
        seq_state.selected_step = new_step;
    } else {
        // BPM change
        sequencer_ui_set_bpm(seq_state.bpm + delta);
    }
}

void sequencer_ui_handle_button(void) {
    if (seq_state.edit_mode) {
        // Toggle step in UI grid
        uint8_t t = seq_state.selected_track;
        uint8_t s = seq_state.selected_step;
        seq_state.grid[t][s] = !seq_state.grid[t][s];

        // Immediately update core (this is the only place we sync)
        if (seq_state.grid[t][s]) {
            sequencer_core_set_step(t, s, true);   // turn ON
        } else {
            sequencer_core_set_step(t, s, false);  // turn OFF
        }
    } else {
        // Toggle play/pause
        seq_state.playing = !seq_state.playing;
        sequencer_core_set_playing(seq_state.playing);
    }
}

void sequencer_ui_toggle_playing(void) {
    seq_state.playing = !seq_state.playing;
    sequencer_core_set_playing(seq_state.playing);
    ESP_LOGI(TAG, "Playback %s", seq_state.playing ? "started" : "stopped");
}

void sequencer_ui_set_bpm(uint16_t bpm) {
    if (bpm < 40) bpm = 40;
    if (bpm > 300) bpm = 300;
    seq_state.bpm = bpm;
    sequencer_core_set_bpm(bpm);
}
