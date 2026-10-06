# Multi-Layer Sequencer Architecture

> Build target: ESP32-S3-N16R8, ESP-IDF 6.1.

---

## Overview

The sequencer is split into cooperating layers:

```
main/   ──button events──▶  synth_ui/  ──state changes──▶  sequencer_core/  ──amy_add_event──▶  AMY engine
                            (FreeRTOS task + screens)         (engine TUs)
                                   │
                             display_*.c  ──u8g2──▶  SSD1306 OLED
```

| Layer | Location | Responsibility |
|---|---|---|
| UI / input | `components/synth_core/synth_ui/` | Encoder + button dispatch, per-screen input handlers, cursor navigation, layer cycling, OLED refresh (20 Hz task) |
| Audio core | `components/synth_core/sequencer_core/` | Owns all AMY scheduling; edits to grid, note, BPM immediately call `amy_add_event()` |
| Display | `components/display/display_seq.c` (and siblings) | Pure render functions; read flat view structs and drive U8g2 |
| Types / state | `sequencer_core/seq_model.h` | Shared data structures (`seq_layer_t` and friends) |

The engine side is split by concern rather than living in one file:

| File | Role |
|---|---|
| `seq_model.h` | data model (`seq_layer_t`, per-step decoration fields, LFO/filter/env types) |
| `seq_core_config.h` | compile-time limits, Kconfig fallbacks, synth-slot and tag layout |
| `seq_core_state.c` | layer table and lifecycle |
| `seq_core_engine.c` | tag scheduling, transport, mute/solo gating |
| `seq_core_trig.c` | per-step probability / ratchet / conditional-trig evaluation |
| `seq_core_synth.c` | patch loading, drum Synth/PCM engine switch |
| `seq_core_editors.c` | envelope / filter / LFO / distortion commits from the editors |
| `seq_core_tempo.c` | BPM |
| `seq_core_progression.c` | chord progression |

---

## Data Model

### `seq_layer_t`  (`sequencer_core/seq_model.h`)

One instance per active sequencer layer. Holds everything about a single
pattern. Abridged to its current field groups:

```c
typedef struct {
    seq_layer_type_t type;                        // SEQ_LAYER_DRUM | SEQ_LAYER_MELODIC
    uint8_t  num_steps;                           // 16 or 32
    uint8_t  num_tracks;                          // live rows, SEQ_TRACKS_DEFAULT..SEQ_TRACKS (4 or 5)
    bool     grid[SEQ_TRACKS][SEQ_MAX_STEPS];     // step on/off
    uint8_t  step_note[SEQ_TRACKS][SEQ_MAX_STEPS];// per-step MIDI pitch
    int8_t   step_pitch_ofs[SEQ_TRACKS][SEQ_MAX_STEPS]; // +-24 st from step_note,
                                                  // 0 = neutral; applied at emit,
                                                  // survives pitch/bank rewrites
    uint8_t  track_base_note[SEQ_TRACKS];         // base pitch shown on OLED

    voice_params_t vp[SEQ_TRACKS];                // per-row EG0+EG1 envelopes, filter,
                                                  // LFO, distortion (each with a
                                                  // deferred-authority flag) + output trim
    uint8_t  repeat_rate[SEQ_TRACKS];             // fires every N bars (1/2/4/8)
    bool     mute[SEQ_TRACKS];                    // per-track mute
    bool     solo[SEQ_TRACKS];                    // per-track solo (overrides mute)
    bool     chord_mode;                          // progression re-voices this layer
    uint8_t  chord_root;                          // chromatic 0-11
    chord_type_t chord_type;
    uint8_t  swing_pct;                           // odd 16ths delayed by % of a step

    uint8_t  synth_id[SEQ_TRACKS];                // one AMY synth slot per row
    uint16_t patch;                               // shared melodic timbre
    uint16_t track_patch[SEQ_TRACKS];             // per-track timbre (drum layer)
    uint16_t track_pcm_preset[SEQ_TRACKS];        // per-track PCM preset (drum/PCM)
    uint32_t synth_flags;
    uint8_t  num_voices;
    uint8_t  fm_algo_override;                    // melodic FM patches: live algorithm
                                                  // shadowing the patch's baked one;
                                                  // SEQ_FM_ALGO_NONE (0xFF) = follow patch

    // per-step decorations (100/1/NONE = "plain" step, zero extra cost)
    uint8_t  step_prob[SEQ_TRACKS][SEQ_MAX_STEPS];      // 0..100 %
    uint8_t  step_ratchet[SEQ_TRACKS][SEQ_MAX_STEPS];   // 1..SEQ_MAX_RATCHET (4)
    uint8_t  step_every[SEQ_TRACKS][SEQ_MAX_STEPS];     // loop divisor, 1 = every pass
    uint8_t  step_prev[SEQ_TRACKS][SEQ_MAX_STEPS];      // 1 = only if prev attempt fired
    // further per-step fields: velocity offset, micro-timing nudge, ratchet
    // taper (edited in the Step Trig popup)
    ...
} seq_layer_t;
```

Two structural points worth noting:

- **`synth_id` is per row.** Both drum and melodic layers give every track its
  own AMY synth slot, which is what keeps same-pitch notes on different rows
  from collapsing into one voice and lets each drum track carry its own patch.
- **`voice_params_t` is the shared voice-config block.** The same struct is
  embedded by the arp (`s_arp.vp`) and the drone (`s_d.vp`), so the editors and
  the deferred-authority rules behave identically across all three engines
  (rules: [ENGINE-SEMANTICS.md](components/synth_core/ENGINE-SEMANTICS.md#voice-blocks-and-deferred-authority)).

### UI state

The UI-facing state (`seq_state`, owned by `synth_ui/synth_ui_state.c`) wraps
the layer array with cursor / edit-mode / active-layer bookkeeping and is read
by the pure renderers in `components/display/`. See `display_seq.h`.

### Compile-time limits (`seq_model.h`)

| Define | Value | Meaning |
|---|---|---|
| `SEQ_TRACKS` | 5 | Tracks per layer (array width) |
| `SEQ_TRACKS_DEFAULT` | 4 | Tracks a new layer starts with |
| `SEQ_STEPS` | 16 | Default steps for a new layer |
| `SEQ_MAX_STEPS` | 32 | Maximum per layer |
| `MAX_LAYERS` | 4 | Maximum simultaneous layers |

```mermaid
classDiagram
    class seq_state {
        seq_layer_t layers[MAX_LAYERS]
        uint8_t num_layers
        uint8_t active_layer_idx
        uint8_t selected_track
        uint8_t selected_step
        bool playing
        bool edit_mode
    }
    class seq_layer_t {
        seq_layer_type_t type
        uint8_t num_steps
        uint8_t num_tracks
        bool grid[5][32]
        uint8_t step_note[5][32]
        int8_t step_pitch_ofs[5][32]
        voice_params_t vp[5]
        uint8_t repeat_rate[5]
        bool mute[5]
        bool solo[5]
        uint8_t synth_id[5]
        uint16_t track_patch[5]
        uint8_t step_prob[5][32]
        uint8_t step_ratchet[5][32]
        uint8_t step_every[5][32]
        uint8_t step_prev[5][32]
        uint8_t fm_algo_override
    }
    class voice_params_t {
        seq_env_t env
        seq_env_t env1
        seq_filter_t filter
        seq_lfo_t lfo
        seq_dist_t dist
        bool env_authored
        bool env1_authored
        bool filter_authored
        bool lfo_authored
        bool dist_authored
        float amp_trim
    }
    seq_state "1" *-- "0..4" seq_layer_t : layers
    seq_layer_t "1" *-- "5" voice_params_t : vp per row
```

---

## AMY Scheduling

Plain steps are scheduled as **repeating AMY sequencer events** using
`SEQUENCE_TICK` / `SEQUENCE_PERIOD`. No FreeRTOS timer fires audio - all timing
is owned by the AMY tick engine driven by `amy_update()` in
`amy_usb_render_task`.

**Decorated steps take a different path.** A step whose probability is below
100 %, whose ratchet count exceeds 1, or which carries a conditional trigger
does not get a periodic tag at all: `sequencer_core_service_tick()`
(`seq_core_trig.c`) evaluates it once per loop iteration and schedules one-shot
events instead, so the probability roll and FILL/PREV conditions are decided
fresh on every pass.

```mermaid
flowchart TD
    Step["step enabled in a layer"] --> Q{"decorated?<br/>prob &lt; 100% / ratchets &gt; 1 / cond-trig"}
    Q -->|no| Tag["repeating AMY sequencer event<br/>SEQUENCE_TICK / SEQUENCE_PERIOD<br/>fires autonomously every bar"]
    Q -->|yes| Svc["sequencer_core_service_tick()<br/>per 48-PPQ tick, on the render task"]
    Svc --> Roll["probability roll + FILL/PREV check,<br/>fresh every loop pass"]
    Roll --> Shot["one-shot events (+ ratchet sub-hits),<br/>scheduled off the render task<br/>via the amy_ingest pump"]
    Tag --> Fire["AMY tick engine fires -> note deltas"]
    Shot --> Fire
```

The tick engine itself runs once per rendered audio block on the core-1 render
task, slaved to the sample clock - see
[RUNTIME-ARCHITECTURE.md](RUNTIME-ARCHITECTURE.md) for that clock chain.

### Tag layout

`SEQUENCE_TAG` is `uint32_t`. Tags are assigned by layer / track / step
position:

```
ON  tag = layer × (SEQ_TRACKS × SEQ_MAX_STEPS × 2)
          + track × SEQ_MAX_STEPS
          + step

OFF tag = ON tag + (SEQ_TRACKS × SEQ_MAX_STEPS)

Preview = MAX_LAYERS × (SEQ_TRACKS × SEQ_MAX_STEPS × 2)
          + layer × SEQ_TRACKS + track
```

With `MAX_LAYERS=4`, `SEQ_TRACKS=5`, `SEQ_MAX_STEPS=32` the full map
(`seq_core_config.h`) is:

| Range | Owner |
|---|---|
| 0-1279 | step ON/OFF events, all 4 layers |
| 1280-1319 | one-shot preview events (one per layer per track) |
| 1320-1383 | arpeggiator (`SEQ_ARP_TAG_BASE` .. `SEQ_ARP_TAG_MAX`) |
| 1384-1543 | ratchet one-shots for decorated steps (`SEQ_RATCHET_TAG_BASE` ..) |
| 1544-2183 | chord one-shots (`SEQ_CHORD_TAG_BASE` .. `SEQ_CHORD_TAG_MAX`) |
| 2184-2343 | chord preview one-shots (`SEQ_CHORD_PREVIEW_TAG_BASE` ..) |
| 2344-2347 | bounce clip start tags (`SEQ_CLIP_TAG_BASE` .. `SEQ_CLIP_TAG_MAX`) |

The chord ranges scale with `SEQ_CHORD_MAX_NOTES` (5), so `main.c` does not
hardcode the ceiling: it sets `amy_cfg.max_sequencer_tags =
SEQ_CLIP_TAG_MAX + 2` (= 2349 as shipped), clearing the highest used
tag with the margin the layout comment requires (AMY's `sequencer_add_wire()`
rejects `tag >= max_sequences` - see ARP-ARCHITECTURE.md). Widening a chord
moves the top tag; deriving it is what keeps the two in step.

### Period derivation

Each layer's bar period is `num_steps × SEQ_TICKS_PER_STEP` (where
`SEQ_TICKS_PER_STEP = AMY_SEQUENCER_PPQ / 4 = 12`).

| `num_steps` | Bar period (ticks) | Typical use |
|---|---|---|
| 16 | 192 | Standard 1-bar pattern |
| 32 | 384 | Extended 2-bar pattern |

When a 16-step layer and a 32-step layer run simultaneously, the 16-step
layer's period (192) divides evenly into the 32-step period (384), so the
shorter pattern **repeats exactly twice** per longer cycle - it never goes
silent.

A per-track **repeat rate** (`repeat_rate[track]`, values 1/2/4/8) stretches
this further: the track's steps fire only every Nth bar, evaluated in the trig
path.

### Gate widths

Both layer types gate from the runtime per-layer `gate_pct` (the Layer page's
Gate row, saved with the project):

| `gate_pct` | Note-hold |
|---|---|
| 10..100 | % of one step |
| 101..`SEQ_GATE_PCT_MAX` (800) | % of one step, held across steps; the Layer page offers 2, 3, 4, 6 and 8 steps |
| `SEQ_GATE_HOLD` | until the row's next trig |

At any value the note-off lands at least one tick before the next active step
on the same row fires (`seq_step_hold_ticks()`). The distance is measured
between fire ticks, so swing and nudge count, and it wraps around the row's
loop. It is taken from the grid: a step whose probability or condition keeps
it silent in a given loop still ends the note before it. A row plays one
pitch and note-offs match by note number, so a note-off landing after the
next note-on would release that next note.

Only the default differs between layer types, and up to 100 % melodic steps
get an off-beat shortening drums don't:

| Layer type | Default | Off-beat steps |
|---|---|---|
| Drum | `SEQ_DRUM_GATE_DEFAULT_PCT` 50 % = 6 ticks | plain gate |
| Melodic | `SEQ_MELODIC_GATE_DEFAULT_PCT` 92 % (67 % with `SEQ_MELODIC_EXPRESSIVE_DEFAULTS` off) | 2 ticks shorter |

SYNTH-mode drums honor note-offs, so the drum gate controls choke vs. ring;
PCM drum rows schedule no note-off and ring to the end of the sample. The
near-legato melodic default lets notes connect instead of stabbing.

---

## AMY Synth Slot Assignment

The map lives in one header, `components/synth_core/include/synth_slots.h`:
static consumers pack the bottom of the slot space, and the melodic layers are
an open-ended arena on top.

| Consumer | Synth slots | Patch | Voices |
|---|---|---|---|
| (sentinel) | **0** - reserved, "no such row" in row lookups | - | - |
| Arp | **1** | `CONFIG_SEQ_ARP_DEFAULT_PATCH` | 4 |
| Drone | **2 / 3** (carrier / sub) | build-your-own or AMY preset | 5 / 1 |
| Drone (free-running mode) | **4 / 5** (carrier / sub) | build-your-own or AMY preset | chord size / 1 |
| Drum layer (layer 0) | **6-10** (one per track) | per-track from the curated drum list (defaults 58/245/221/220) or per-track PCM presets in PCM mode | 1 |
| Live play | **11** | live-play patch | 4 |
| Clip players | **12-13** (`CLIP_SYNTH_BASE`) | bounce clips (runtime PCM) | 1 |
| Melodic layers | **14..`SEQ_MAX_SYNTH`**, contiguous blocks of 5 from base 14 | `CONFIG_SEQ_MELODIC_PATCH`, shared across the layer's rows | 1 per row |

`main.c` derives `amy_cfg.max_synths` from `SYNTH_SLOT_COUNT` (66). The
melodic ceiling is `SYNTH_SLOT_COUNT - 1`: nothing static sits above the
melodic range, so growing melodic capacity is a single-constant change (the
real limit is AMY's 250-osc pool).

The drum layer is a **per-track patch layer**: each of its 4 or 5 tracks owns a
dedicated synth slot in the fixed block 6-10 and loads its own patch (row 5 is
the AUX row). In
**Synth** mode that patch comes from a curated DX7/Juno drum list
(`seq_core_synth.c`); in **PCM** mode each track plays an AMY PCM preset
(808-style kick/snare/hat/clap by default). Note-offs are honored (flags = 0)
so each patch's own release shapes the tail; the drum gate (above) controls
choke vs. ring.

Default melodic base notes: **C4 / E4 / G4 / B4** (Cmaj7 voicing). Default
drum pitches: **39 / 45 / 53 / 82** (role-tuned per track). Both editable via
hold MY_BUTTON_2 + encoder. Drum **patch** selection is the patch-hold gesture
(MY_BUTTON_1 + encoder), cycling the selected drum track through the curated
list.

---

## Public API

`sequencer_core.h` is the engine's public surface. Representative slices (the
header is the source of truth - it has grown well beyond this list):

```c
/* Lifecycle / transport */
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

/* Per-track performance state */
void sequencer_core_set_track_mute(uint8_t layer_idx, uint8_t track, bool mute);
void sequencer_core_set_track_solo(uint8_t layer_idx, uint8_t track, bool solo);
void sequencer_core_set_track_repeat_rate(uint8_t layer_idx, uint8_t track,
                                          seq_repeat_rate_t rate);

/* Per-step decorations */
void sequencer_core_set_step_prob(uint8_t layer_idx, uint8_t track,
                                  uint8_t step, uint8_t prob);
/* ...ratchet, cond_type, cond_param accessors follow the same shape */
```

Mute/solo gating happens in one place - `sequencer_track_audible()`
(`seq_core_engine.c`): if any track in the layer is soloed, only soloed tracks
sound; otherwise un-muted tracks sound. Both the plain emit path and the
decorated-step trig path consult it.

---

## Button Mapping

Per-button behavior: [CONTROLS.md](CONTROLS.md). GPIOs: `s_button_gpios[]` in
`components/my_buttons/my_buttons.c`. Buttons are dispatched by
`input_dispatch_button()` in `main/input_dispatch.c`.

Dispatch model: `iot_button` delivers events on the system `esp_timer` task,
where the callback only enqueues them (depth-16 queue in `main.c`; a full queue
drops rather than reorders); `button_handler_task` runs the routing cascade, so
no UI logic executes in timer-callback context. Precedence, first match wins:

```mermaid
flowchart TD
    EV["button event (button_task)"] --> P0{"SHOULDER or SHIFT?"}
    P0 -->|yes| A0["per-view step/polarity action,<br/>or arm the SHIFT modifier"]
    P0 -->|no| P1{"SHIFT chord (1/2/3)?"}
    P1 -->|yes| A1["editor open/close, Step Trig,<br/>voice-block flip, release to patch - press latched"]
    P1 -->|no| P2{"editor- or screen-specific<br/>override for this button?"}
    P2 -->|yes| A2["per-editor / per-screen action<br/>(isolation guards keep play/pause live)"]
    P2 -->|no| P3["route by synth_ui_active_view()"]
    P3 --> A3["overlay or mode screen handler,<br/>else sequencer default"]
```

The same `synth_ui_active_view()` resolver routes the encoder, so button and
encoder input always agree with the draw code about which screen is active.

---

## OLED Display Layout

```
[0,0]──────────────────────────────[127,0]
BPM 108   L0 DRM        ▶         y=8
──────────────────────────────────  y=10
CHH  □■□□ □■□□ □■□□ □■□□           y=20
ABD  □□□□ □■□□ □□□□ □■□□           y=30
Snr  □□□□ □□□□ □□□□ □□□□           y=40
CBl  □□□□ □□□□ □□□□ □□□□           y=50
──────────────────────────────────  y=57
[hint strip: current button roles]  y=57..63
```

- Header: `BPM NNN` | `LN TYP` (layer index + DRM/MEL) | ▶/▮▮ 
- Track labels: per-track patch number (drum layer) or note name e.g. "C4", "C#4" (melodic layer)
- Label inverts (white-on-black) while MY_BUTTON_2 is held for the selected track
- Step cells: 5×5 px filled = active, outline = inactive
- Beat separators: vertical lines every 4 steps
- Playhead: XOR column over current step (only shown if step is on this page)
- Cursor: rounded rectangle around selected cell (edit mode only)
- Hint strip: bottom 7 rows, composited by the UI task after the screen fill

For 32-step layers the 16-cell window shown is `page × 16 .. (page+1) × 16 − 1`.
Scrolling the cursor past step 15 flips to page 1; past step 31 wraps to step 0
page 0.

All renderers are **fill-only**; `synth_ui_task` issues the single
`u8g2_SendBuffer` per redraw after compositing the hint strip, and only when a
view signature changed (the blocking ~20 ms I2C transfer is the scarce
resource).

---

## FreeRTOS Task Summary

Configured at creation (stack = words passed to `xTaskCreatePinnedToCore`):

| Task | Priority | Core | Stack | Rate |
|---|---:|---:|---:|---|
| `amy_render` | 22 | 1 | 8192 | Deadline-driven (one 256-sample block per GPTimer wake, 5333 µs) |
| `seq_ui` | 5 | 0 | 8192 | 20 Hz (`vTaskDelayUntil`, 50 ms); sized for AMY patch-string parses during deferred layer adds |
| `button_task` | 5 | 0 | 8192 | Blocks on the button event queue |
| `amy_ingest` | 5 | 0 | 8192 | AMY event pump (`amy_helpers.c`): drains queued `amy_event`s off the render path, urgent decorated-step trig jobs (`seq_trig_pump.c`) first |
| `encoder_task` | 5 | 0 | 8192 | 50 Hz poll |
| `status_led` | 2 | 0 | 3072 | 10 Hz tick (blink animation); one core-load sample per second |
| TinyUSB / UAC tasks | (component) | 0 | - | USB service, pinned to core 0 via sdkconfig |
| `esp_timer` | 22 | 0 | - | System timer callbacks (button events originate here, then queue out); the sequencer tick does not run here - it advances per block on the core-1 render task |

The `seq_ui` task owns one call path per frame: service the arp / drone /
progression / LFO engines, resolve the active view, build its flat view
struct, draw, composite the hint strip, and flush once. No AMY state is
touched outside the queued event API.

---

## Initialization Sequence

```
app_main
  ├── i2c_u8g2_init()
  ├── amy_start()              ← multicore=0, multithread=0, AMY_AUDIO_IS_NONE,
  │                              max_synths=SYNTH_SLOT_COUNT (66),
  │                              max_sequencer_tags=SEQ_CLIP_TAG_MAX+2 (2349)
  ├── usb_audio_init()
  ├── synth_ui_init(u8g2)
  │     ├── amy_helpers_init()          (shared event scratch + mutex)
  │     ├── sequencer_core_init()
  │     ├── arp_core_init() / drone_core_init() / sample_rec_init()
  │     ├── add drum layer        → synth slots 6..10, seeded groove
  │     ├── sequencer_core_set_playing(true)
  │     ├── add melodic layer     → synth slots 14..18
  │     └── xTaskCreate(seq_ui task)
  ├── xTaskCreatePinnedToCore(amy_usb_render_task, core 1)
  ├── button queue + button_handler_task
  ├── my_buttons_init() + register_cb()
  └── encoder_init_task (deferred 1 s)
```

The same order expressed as a sequence diagram:

```mermaid
sequenceDiagram
    participant app_main
    participant OLED as i2c_u8g2_init
    participant AMY as amy_start
    participant USB as usb_audio_init
    participant UI as synth_ui_init
    participant Core as sequencer_core
    participant SeqTask as seq_ui task
    participant Render as amy_usb_render_task (Core 1)
    participant Btn as button path
    participant Enc as encoder_init_task

    app_main->>OLED: i2c_u8g2_init()
    app_main->>AMY: amy_start() [multicore=0, multithread=0, AMY_AUDIO_IS_NONE]
    app_main->>USB: usb_audio_init()
    app_main->>UI: synth_ui_init()
    UI->>Core: sequencer_core_init() + arp/drone/sampler init
    UI->>Core: add drum layer (slots 6..10, seeded groove)
    UI->>Core: sequencer_core_set_playing(true)
    UI->>Core: add melodic layer (slots 14..18)
    UI->>SeqTask: create seq_ui task
    app_main->>Render: xTaskCreatePinnedToCore(amy_usb_render_task, core 1)
    app_main->>Btn: button queue + button_handler_task + my_buttons_init()
    app_main->>Enc: encoder_init_task (deferred 1 s)
```

---

## Future Development Considerations

### More than 4 layers

Increase `MAX_LAYERS` in `seq_model.h`. The tag formula scales automatically
(raise `amy_cfg.max_sequencer_tags` to keep the arp and ratchet windows above
the step window). Memory impact per layer is dominated by the per-step
decoration arrays; the OLED shows one layer at a time, so display code is
unaffected. Melodic slot pressure: each layer consumes a block of 5 slots
between 14 and 65, so the practical ceiling is ~10 melodic layers before the
slot map, not memory, is the limit.

### Whole-layer mute / play-stop per layer

Per-track mute/solo (scoped within a layer) is implemented and editable from
the menu's Layer page. A whole-layer mute, or per-layer transport independent of the
global `sequencer_core_set_playing(bool)`, remains open.

### Saving patterns

Projects (patterns, voice parameters, arp, drones, progression, FX, tempo)
are saved to LittleFS slots as a TLV snapshot; see the project-store section
of [README.md](README.md#project-storage).

### AMY `write_samples_fn` / future upstream UAC support

The current audio path is `AMY_AUDIO_IS_NONE` with `amy_usb_render_task`
manually calling `amy_update()` → `usb_audio_write_stereo()`. If AMY upstream
adds a proper ESP UAC path, migration would involve setting
`amy_cfg.audio = AMY_AUDIO_IS_USB_GADGET` and pointing
`amy_cfg.write_samples_fn` to a thin wrapper, eliminating
`amy_usb_render_task`. The sequencer core is unaffected by this change.
