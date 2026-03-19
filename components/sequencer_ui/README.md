# Sequencer UI Component

This component provides a minimal, non-blocking 16-step sequencer UI for the AMY synth project on an ESP32-S3 with a 128x64 OLED display.

## Features

- **4-Track x 16-Step Grid**: Displays 4 tracks (BD, SD, CH, OH) with 16 steps each.
- **Non-blocking UI**: Uses a dedicated FreeRTOS task (`sequencer_ui_task`) running at 20Hz to refresh the display without blocking audio processing.
- **Sequencer Timer**: A separate high-priority FreeRTOS task (`sequencer_timer_task`) handles BPM timing and schedules `amy_event`s for active steps.
- **Encoder Integration**: 
  - Rotating the encoder navigates the grid (in edit mode) or changes the BPM (in play mode).
  - Pressing the encoder button toggles the selected step (in edit mode) or toggles play/pause (in play mode).
- **Immediate Feedback**: Toggling a step immediately posts an `amy_event` to preview the sound.

## Architecture

- `sequencer_ui.c`: Contains the state machine, FreeRTOS tasks, and input handlers.
- `include/sequencer_ui.h`: Exposes the initialization and input handling functions.
- Relies on `priv_i2c_u8g2` for low-level display drawing helpers (`priv_u8g2_seq_draw_frame`).
- Relies on `amy` for audio synthesis and event scheduling.

## Usage

1. Initialize the display using `i2c_u8g2_init()`.
2. Call `sequencer_ui_init(u8g2)` with the initialized `u8g2_t` pointer.
3. In your encoder task, call `sequencer_ui_handle_encoder(delta)` when the encoder rotates.
4. In your button task/interrupt, call `sequencer_ui_handle_button()` when the encoder button is pressed.
