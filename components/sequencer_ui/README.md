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


## Scale Table

| Index | Scale                   | Notes relative to root (e.g., C) | Character                                                        |
| ----- | ----------------------- | -------------------------------- | ---------------------------------------------------------------- |
| 0     | Chromatic               | C C# D D# E F F# G G# A A# B     | All 12 notes available; no scale restriction.                    |
| 1     | Major (Ionian)          | C D E F G A B                    | Bright, happy, conventional Western major scale.                 |
| 2     | Natural Minor (Aeolian) | C D D# F G G# A#                 | Darker, sadder, common minor scale.                              |
| 3     | Dorian                  | C D D# F G A A#                  | Minor feel with a brighter 6th; common in jazz and funk.         |
| 4     | Phrygian                | C C# D# F G G# A#                | Exotic, Spanish/Middle Eastern flavor due to the flat 2nd.       |
| 5     | Lydian                  | C D E F# G A B                   | Dreamy, floating sound because of the raised 4th.                |
| 6     | Mixolydian              | C D E F G A A#                   | Major-like but bluesier due to the flat 7th.                     |
| 7     | Minor Pentatonic        | C D# F G A#                      | Very common in blues, rock, and solos; hard to make wrong notes. |
| 8     | Major Pentatonic        | C D E G A                        | Open, melodic, folk/country sound; also very forgiving.          |
