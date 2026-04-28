# Multi-Layer Sequencer Architecture

> Implemented 2026-04-03. Build target: ESP32-S3-N16R8, ESP-IDF 6.0.

---

## Overview

The sequencer is split into three cooperating layers:

```
main.c  ──button events──▶  sequencer_ui.c  ──state changes──▶  sequencer_core.c  ──amy_add_event──▶  AMY engine
                            (FreeRTOS task)                      (stateless helpers)
                                   │
                             priv_u8g2_seq.c  ──u8g2──▶  SSD1306 OLED
```

| Layer | File | Responsibility |
|---|---|---|
| UI / input | `sequencer_ui.c` | Encoder + button dispatch, cursor navigation, layer cycling, OLED refresh (20 Hz task) |
| Audio core | `sequencer_core.c` | Owns all AMY scheduling; edits to grid, note, BPM immediately call `amy_add_event()` |
| Display | `priv_u8g2_seq.c` | Pure render function; reads `priv_u8g2_seq_state_t` and drives U8g2 |
| Types / state | `priv_u8g2_seq.h` | Shared data structures (`seq_layer_t`, `priv_u8g2_seq_state_t`) |

---

## Data Model

### `seq_layer_t`  (`priv_u8g2_seq.h`)

One instance per active sequencer layer. Holds everything about a single pattern.

```c
typedef struct {
    seq_layer_type_t type;                        // SEQ_LAYER_DRUM | SEQ_LAYER_MELODIC
    uint8_t  num_steps;                           // 16 or 32
    uint8_t  num_tracks;                          // always SEQ_TRACKS (4)
    bool     grid[SEQ_TRACKS][SEQ_MAX_STEPS];     // step on/off
    uint8_t  step_note[SEQ_TRACKS][SEQ_MAX_STEPS];// per-step MIDI pitch (forward-compatible)
    uint8_t  track_base_note[SEQ_TRACKS];         // current base pitch shown on OLED
    uint8_t  synth_id;                            // AMY synth slot
    uint16_t patch;                               // AMY patch number
    uint32_t synth_flags;                         // AMY synth flags
    uint8_t  num_voices;                          // polyphony count
    uint8_t  step_page;                           // display page 0|1 (32-step layers only)
} seq_layer_t;
```

### `priv_u8g2_seq_state_t` (`priv_u8g2_seq.h`)

Single global (`seq_state` in `sequencer_ui.c`) shared between the UI task and the display renderer.

```c
typedef struct {
    seq_layer_t layers[MAX_LAYERS];  // all layer data
    uint8_t     num_layers;          // how many are active
    uint8_t     active_layer_idx;    // which layer is currently displayed/edited
    uint16_t    bpm;
    uint8_t     current_pattern;
    uint8_t     current_step;        // playhead for active layer
    bool        playing;
    uint8_t     selected_track;
    uint8_t     selected_step;
    bool        edit_mode;
    bool        drum_select_mode;    // true while MY_BUTTON_2 held
} priv_u8g2_seq_state_t;
```

### Compile-time limits (`priv_u8g2_seq.h`)

| Define | Value | Meaning |
|---|---|---|
| `SEQ_TRACKS` | 4 | Tracks per layer |
| `SEQ_STEPS` | 16 | Default steps for a new layer |
| `SEQ_MAX_STEPS` | 32 | Maximum per layer |
| `MAX_LAYERS` | 4 | Maximum simultaneous layers |

---

## AMY Scheduling

Steps are scheduled as **repeating AMY sequencer events** using `SEQUENCE_TICK` / `SEQUENCE_PERIOD`. No FreeRTOS timer fires audio — all timing is owned by the AMY tick engine driven by `amy_update()` in `amy_usb_render_task`.

### Tag formula

`SEQUENCE_TAG` is `uint32_t` so the tag space is effectively unlimited. Tags are assigned by layer / track / step position:

```
ON  tag = layer × (SEQ_TRACKS × SEQ_MAX_STEPS × 2)
          + track × SEQ_MAX_STEPS
          + step

OFF tag = ON tag + (SEQ_TRACKS × SEQ_MAX_STEPS)

Preview = MAX_LAYERS × (SEQ_TRACKS × SEQ_MAX_STEPS × 2)
          + layer × SEQ_TRACKS + track
```

With `MAX_LAYERS=4`, `SEQ_TRACKS=4`, `SEQ_MAX_STEPS=32`:
- Tags 0–1023 — step ON/OFF events for all 4 layers
- Tags 1024–1039 — one-shot preview events (one per layer per track)

### Period derivation

Each layer's bar period is `num_steps × SEQ_TICKS_PER_STEP` (where `SEQ_TICKS_PER_STEP = AMY_SEQUENCER_PPQ / 4 = 12`).

| `num_steps` | Bar period (ticks) | Typical use |
|---|---|---|
| 16 | 192 | Standard 1-bar pattern |
| 32 | 384 | Extended 2-bar pattern |

When a 16-step layer and a 32-step layer run simultaneously, the 16-step layer's period (192) divides evenly into the 32-step period (384), so the shorter pattern **repeats exactly twice** per longer cycle — it never goes silent.

### Gate widths

| Layer type | Gate (ticks) | Duration |
|---|---|---|
| Drum | `SEQ_TICKS_PER_STEP / 3` = 4 | Short, percussive |
| Melodic | `SEQ_TICKS_PER_STEP × 2 / 3` = 8 | Legato-ish, 2/3 of step |

---

## AMY Synth Slot Assignment

| Layer | Synth ID | Patch | Voices | Synth flags |
|---|---|---|---|---|
| Layer 0 (Drum) | 10 | 1025 (`w7f0` GM drums) | 6 | `MIDI_DRUMS \| IGNORE_NOTE_OFFS` |
| Layer 1 (Melodic) | 12 | 128 (DX7 "E Piano 1") | 4 | 0 (note-offs enabled) |
| Layer 2 (Melodic) | 13 | 128 | 4 | 0 |
| Layer N (Melodic) | `10 + N + 1` (capped at 62) | 128 | 4 | 0 |

The drum synth is always slot 10 (first `add_layer` call). Melodic layers are assigned consecutive slots. The cap of 62 keeps the slot below AMY's `max_synths` (64).

Default melodic base notes: **C4 / E4 / G4 / B4** (Cmaj7 voicing). Change via hold MY_BUTTON_2 + encoder.

---

## Public API

### `sequencer_core.h`

```c
/* Lifecycle */
void sequencer_core_init(void);
void sequencer_core_set_playing(bool playing);
void sequencer_core_set_bpm(uint16_t bpm);
uint8_t sequencer_core_get_current_step(uint8_t layer_idx);

/* Layer management */
uint8_t          sequencer_core_add_layer(seq_layer_type_t type, uint8_t num_steps);
uint8_t          sequencer_core_get_num_layers(void);
seq_layer_type_t sequencer_core_get_layer_type(uint8_t layer_idx);

/* Per-layer step / note control */
void    sequencer_core_set_step(uint8_t layer_idx, uint8_t track,
                                uint8_t step, bool state);
void    sequencer_core_set_track_midi_note(uint8_t layer_idx, uint8_t track,
                                           uint8_t midi_note);
uint8_t sequencer_core_get_track_midi_note(uint8_t layer_idx, uint8_t track);
```

### `sequencer_ui.h`

```c
/* Init (call after amy_start) */
void    sequencer_ui_init(u8g2_t *u8g2);

/* Layer management */
uint8_t sequencer_ui_add_layer(seq_layer_type_t type, uint8_t num_steps);
void    sequencer_ui_cycle_active_layer(void);

/* Input dispatch */
void sequencer_ui_handle_encoder(long delta);
void sequencer_ui_handle_button(void);
void sequencer_ui_toggle_playing(void);
void sequencer_ui_set_bpm(uint16_t bpm);
void sequencer_ui_adjust_track_note(int delta);
void sequencer_ui_set_drum_select_mode(bool held);

/* Global state — bpm is read directly by encoder_task */
extern sequencer_ui_state_t seq_state;
```

---

## Button Mapping (as of this implementation)

| Button | Event | Action |
|---|---|---|
| MY_BUTTON_0 (GPIO17) | `BUTTON_SINGLE_CLICK` | Cycle active layer (L0 ↔ L1 ↔ …) |
| MY_BUTTON_0 (GPIO17) | `BUTTON_LONG_PRESS_START` | Toggle play / stop |
| MY_BUTTON_ENC (GPIO16) | `BUTTON_PRESS_DOWN` | Toggle focused step / toggle play (non-edit mode) |
| MY_BUTTON_1 (GPIO18) | held + encoder | Adjust BPM |
| MY_BUTTON_2 (GPIO8) | held + encoder | Scroll base note for selected track (drum or melodic) |

Note: MY_BUTTON_0's role changed with this implementation. Previously it was play/stop on press-down. It is now short-press = cycle layer, long-press = play/stop.

---

## OLED Display Layout

```
[0,0]──────────────────────────────[127,0]
BPM 120   L0 DRM        ▶         y=8
──────────────────────────────────  y=10
CHH  □■□□ □■□□ □■□□ □■□□           y=20
ABD  □□□□ □■□□ □□□□ □■□□           y=30
Snr  □□□□ □□□□ □□□□ □□□□           y=40
CBl  □□□□ □□□□ □□□□ □□□□           y=50
```

- Header: `BPM NNN` | `LN TYP` (layer index + DRM/MEL) | ▶/▮▮ | `P1`/`P2` (32-step only)
- Track labels: 3-char GM drum name (drum layer) or note name e.g. "C4", "C#4" (melodic layer)
- Label inverts (white-on-black) while MY_BUTTON_2 is held for the selected track
- Step cells: 5×5 px filled = active, outline = inactive
- Beat separators: vertical lines every 4 steps
- Playhead: XOR column over current step (only shown if step is on this page)
- Cursor: rounded rectangle around selected cell (edit mode only)

For 32-step layers the 16-cell window shown is `page × 16 .. (page+1) × 16 − 1`. Scrolling the cursor past step 15 flips to page 1; past step 31 wraps to step 0 page 0.

---

## FreeRTOS Task Summary

| Task | Priority | Core | Stack | Rate |
|---|---|---|---|---|
| `amy_render` | 7 | Core 1 | 8 KB | Deadline-driven (~5.33 ms/block at 48 kHz) |
| `seq_ui` | 5 | any | 4 KB | 20 Hz (`vTaskDelayUntil`, 50 ms) |
| `encoder_task` | 5 | any | 8 KB | 50 Hz poll |
| `button_task` | 5 | any | 8 KB | Blocks on queue |
| `encoder_init_task` | 5 | any | 2 KB | One-shot (1 s delay, self-deletes) |

The `seq_ui` task owns one call path: read playhead from `sequencer_core_get_current_step()` → copy into `seq_state` → call `priv_u8g2_seq_draw_frame()`. No AMY state is read or written here.

---

## Initialization Sequence

```
app_main
  ├── i2c_u8g2_init()
  ├── amy_start()              ← multicore=0, multithread=0, AMY_AUDIO_IS_NONE
  ├── usb_audio_init()
  ├── sequencer_ui_init()
  │     ├── sequencer_core_init()
  │     ├── sequencer_ui_add_layer(SEQ_LAYER_DRUM, 16)
  │     │     └── sequencer_core_add_layer() → configures AMY synth slot 10
  │     ├── default pattern written to layers[0].grid
  │     ├── sync_layer_to_core(0)
  │     ├── sequencer_core_set_playing(true)
  │     └── xTaskCreate(seq_ui_task)
  ├── sequencer_ui_add_layer(SEQ_LAYER_MELODIC, 16)
  │     └── sequencer_core_add_layer() → configures AMY synth slot 12
  ├── xTaskCreatePinnedToCore(amy_render_task, core 1)
  ├── my_buttons_init() + register_cb()
  └── encoder_init_task (deferred 1 s)
```

---

## Future Development Considerations

### Per-step pitch / velocity editing

`step_note[SEQ_TRACKS][SEQ_MAX_STEPS]` is already stored per step, initialised uniformly to `track_base_note`. The core's `sequencer_emit_step()` already reads `step_note[track][step]` for every event — **no core changes required**. What is missing:

1. A UI mode (e.g. long-press encoder on a step → enter pitch-edit mode)
2. `sequencer_ui_set_step_note(uint8_t layer, uint8_t track, uint8_t step, uint8_t note)` — calls `sequencer_core_set_step(...)` to reschedule the event

Velocity would require adding a `step_velocity[SEQ_TRACKS][SEQ_MAX_STEPS]` field to `seq_layer_t` and plumbing it through `sequencer_emit_step()`.

### 32-step patterns

`sequencer_ui_add_layer(type, 32)` already works. The core uses `num_steps × SEQ_TICKS_PER_STEP` as the bar period, so any `num_steps` value that is a multiple of 16 (16, 32) works without code changes. A UI function to toggle an existing layer between 16 and 32 steps would need to reschedule all its events (`sequencer_resync_layer(idx)`) after updating `layer->num_steps`.

### Changing the melodic patch

Replace `SEQ_MEL_PATCH` (default 128 = DX7 "E Piano 1") with any value:
- 0–127: Juno-106 analog emulation
- 128–255: Yamaha DX7 FM
- 256: Piano
- 1024–1055: User patches

Call `sequencer_configure_synth(layer_idx)` (currently static) after changing `s_layers[layer_idx].patch`. You would need to expose it or add a `sequencer_core_set_patch(uint8_t layer_idx, uint16_t patch)` API function.

### More than 4 layers

Increase `MAX_LAYERS` in `priv_u8g2_seq.h`. The tag formula scales automatically. Memory impact: each `seq_layer_t` is approximately `4×32 + 4×32 + sizeof(misc)` ≈ 300 bytes; 8 layers ≈ 2.4 KB. The OLED layout fits 4 tracks regardless of layer count — only one layer is displayed at a time, so display code is unaffected.

AMY synth slot pressure: each melodic layer consumes one AMY synth slot (slots 12, 13, 14 … for layers 1, 2, 3). AMY defaults to `max_synths=64`, so up to ~50 melodic layers are mechanically possible.

### Layer deletion / reordering

Not currently implemented. `s_num_layers` only ever increments. A delete operation would need to: clear all scheduled tags for the layer (`sequencer_clear_layer_tags(idx)`), compact the `s_layers[]` array, and renumber the surviving layers' tags (which requires re-emitting all steps). Simplest alternative: support muting a layer instead of deleting it.

### Separate play/stop per layer

Currently `sequencer_core_set_playing(bool)` stops all layers. Per-layer mute would be simpler to implement: add a `bool muted` field to `seq_layer_t`; `sequencer_emit_step()` already checks `s_playing && layer->grid[track][step]` — add `&& !layer->muted` to that condition and call `sequencer_resync_layer(idx)` when mute toggles.

### Saving patterns (NVS)

No persistence is implemented. `seq_layer_t` is a flat struct with no pointers, so it is directly serialisable to NVS with `nvs_set_blob`. Key design decision: use a fixed blob key per slot index (e.g. `"layer_0"`, `"layer_1"`) and save `seq_state.num_layers` separately.

### AMY `write_samples_fn` / future upstream UAC support

The current audio path is `AMY_AUDIO_IS_NONE` with `amy_usb_render_task` manually calling `amy_update()` → `usb_audio_write_stereo()`. If AMY upstream adds a proper ESP UAC path, migration would involve setting `amy_cfg.audio = AMY_AUDIO_IS_USB_GADGET` and pointing `amy_cfg.write_samples_fn` to a thin wrapper, eliminating `amy_usb_render_task`. The sequencer core is unaffected by this change.
