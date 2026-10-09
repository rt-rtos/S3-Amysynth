# display — display layer

The project's **display layer**: the ESP-IDF↔U8g2 hardware glue plus every
on-screen renderer. It started as a drop-in I2C/U8g2 glue component and grew
into the home for all the OLED rendering as the UI expanded.

> Naming note: the renderer files use the `display_*` prefix. The reusable HAL
> file keeps the `priv_i2c_u8g2.{c,h}` filename and its `i2c_u8g2_*` symbols, so
> existing `CONFIG_I2C_U8G2_*` menuconfig options are unchanged.

## What it provides

**Display HAL** (the original, still-reusable part)

- ESP-IDF I2C master bus/device setup, U8g2 byte + gpio/delay callbacks
- Display init + power control; hands the app a `u8g2_t*` to draw with
- Configurable via menuconfig (`Kconfig`); default panel is SSD1306 128×64

**Renderers** (project-specific; the UI logic lives in `synth_core`, this layer
only draws the flat view structs it is handed)

- Every screen: sequencer grid, arp, drone (list + visualizer), menu overlay,
  chord progression, step-trig popup, and the bottom hint strip
- A reusable ADSR/curve **graph-popup** widget used by the envelope editor,
  plus the filter-response and LFO editor renderers
- Patch-name tables for the on-screen patch browser

## Files

| File | Role |
| --- | --- |
| `priv_i2c_u8g2.{c,h}` | I2C + U8g2 HAL: bus/device setup, init, power, `u8g2_t*` accessor (`i2c_u8g2_*` symbols) |
| `display_seq.{c,h}` | sequencer grid renderer + the shared UI state struct |
| `display_arp.{c,h}` | arp screen renderer + its flat view struct |
| `display_drone.{c,h}` | drone parameter-list renderer + the drone visualizer |
| `display_menu.{c,h}` | menu overlay renderer (scrollable label:value list, also reused by the FX and FM pages) |
| `display_prog.{c,h}` | chord-progression screen renderer |
| `display_stepedit.{c,h}` | per-step probability / ratchet / conditional-trig popup |
| `display_hint.{c,h}` | bottom hint-strip compositor (rows 57–63) |
| `display_lfo.{c,h}` | LFO editor overlay renderer |
| `display_dist.{c,h}` | DIST editor overlay renderer |
| `graph_popup.{c,h}` | reusable graph/curve editor widget (ADSR) |
| `filter_graph.{c,h}` | per-synth filter frequency-response renderer |
| `patch_names.{c,h}` | AMY patch number → name tables for the browser |
| `chord_types.h` | shared chord type/root name lookup tables |
| `Kconfig` | display menuconfig options |

## Design

The renderers are intentionally **dumb**: each takes a flat "view" struct
(plain values + cursor/edit flags) and draws it. The state those views describe,
and all the input handling, live in `synth_core` (sequencer core, arp, drone,
menu). This keeps the display layer decoupled from synth/sequencer logic — the
same reason the menu and arp/drone screens each have a small `*_view_t` instead
of reaching into engine state directly.

Two conventions matter for anyone adding a screen:

- **Fill-only renderers, single flush.** No renderer calls `u8g2_SendBuffer`.
  Every `*_draw_frame` only fills the buffer; the UI task composites the hint
  strip on top and issues the one `SendBuffer` per redraw. A renderer that
  flushes on its own reintroduces visible flicker on the hint strip.
- **The shared sequencer UI state struct** (`display_seq.h`) is the one
  exception to the flat-view rule: it is large enough that it doubles as the
  canonical UI state, included by both this layer and `synth_core`.

## Filter editor (`filter_graph`)

### UI overview

The filter editor is a full-screen overlay, one tab of the editor cycle (see
[CONTROLS.md](../../CONTROLS.md)).

The screen is split into two zones:

```
┌──────────────────────────────────────────┐  row 0
│  L1 T2        LPF          1.2kHz        │  top bar (rows 0-15)
├──────────────────────────────────────────┤  row 15
│                                          │
│  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓    │  frequency
│  ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓    │  response
│  ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓    │  plot
│  ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓    │  (rows 16-63)
│  ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓    │
└──────────────────────────────────────────┘  row 63
   20 Hz                              8 kHz
```

**Top bar** — three fields:
- **Left** (`font_6x10_tf`): target label — `"L<layer> T<track>"` for melodic
  tracks, `"ARP"` for the arpeggiator, `"DRONE"` for the drone synth.
- **Centre** (`font_5x7_tr`): filter type name (`NONE` / `LPF` / `BPF` / `HPF`
  / `LPF24`). Framed when cursor is on the type field; inverted when being edited.
- **Right** (`font_5x7_tr`): current parameter value — `NNNHz` / `N.NkHz` when
  the cursor is on cutoff, `Q:N.N` when on resonance. Framed while editing.

**Plot** — X axis is log-frequency (20 Hz left → 8 kHz right). Y axis is
amplitude (top = louder). The filled area under the curve is drawn with vertical
lines to the baseline. An XOR cursor column marks the cutoff frequency.

### Cursor and controls

Cursor 0 is cutoff (one semitone per detent), 1 resonance (2 % of range per
detent); the rows after them (KS feedback and duty, filter type, enable) and
the key bindings are in [CONTROLS.md](../../CONTROLS.md).

Stutter drone target: the filter type is always LPF24 (fixed by the drone
architecture), so only cutoff and resonance are on the cursor. Committing moves the filter sweep midpoint to the chosen
cutoff while preserving the sweep width; resonance maps directly to the drone
resonance parameter.

### `filter_graph_t` — data struct

```c
typedef struct {
    uint8_t filter_type;     /* FGRAPH_FILTER_{NONE,LPF,BPF,HPF,LPF24} */
    float   cutoff_norm;     /* 0..1, log-mapped: 0 = 20 Hz, 1 = 8000 Hz */
    float   resonance_norm;  /* 0..1 linear: 0 = 0.51 (min Q), 1 = 8.0 */
    uint8_t cursor;          /* 0=cutoff, 1=resonance, 2=type */
    bool    editing;         /* true while encoder is adjusting this field */
    bool    enabled;         /* false → draw flat line + "OFF" overlay */
    char    label[16];       /* top-bar left field */
} filter_graph_t;
```

`synth_core` owns the `filter_graph_t` scratch copy and populates it from the
live synth state on editor open. The display component has no AMY or
`synth_core` dependency; all Hz ↔ norm conversions happen on the UI side.

### Response model

The curve is computed from the standard second-order biquad magnitude formula,
evaluated at `FG_NPTS = 24` log-spaced frequency samples:

| Type | Formula | Notes |
|------|---------|-------|
| LPF (-12 dB/oct) | `1 / √((1-r²)² + (r/Q)²)` | r = f/fc |
| HPF (-12 dB/oct) | `r² / √((1-r²)² + (r/Q)²)` | |
| BPF | `(r/Q) / √((1-r²)² + (r/Q)²)` | |
| LPF24 (-24 dB/oct) | LPF magnitude squared | two cascaded biquads |

Passband is normalised to 75 % of plot height (`FG_PASSBAND_NORM = 0.75`),
leaving 25 % headroom for the resonance spike. Q values above ~1.33 push the
spike to the top of the plot; the exact Q is always shown numerically in the top
bar regardless.

The response is purely for display. It does not derive from AMY's actual biquad
coefficients — it is a standard textbook approximation with the same topology.
This keeps the renderer free of any runtime AMY dependency.

### Constants

| Symbol | Value | Meaning |
|--------|-------|---------|
| `FGRAPH_CUTOFF_HZ_MIN` | 20 Hz | Left edge of X axis / minimum cutoff |
| `FGRAPH_CUTOFF_HZ_MAX` | 16000 Hz | Right edge of X axis / maximum cutoff |
| `FGRAPH_RES_MIN` | 0.51 | Minimum Q (AMY biquad hard floor) |
| `FGRAPH_RES_MAX` | 8.0 | Maximum Q (project cap) |
| `FG_PASSBAND_NORM` | 0.75 | Passband fraction of plot height |
| `FG_NPTS` | 24 | Frequency sample count for the curve |

## HAL quick usage

```c
#include "priv_i2c_u8g2.h"

i2c_u8g2_handle_t display;
i2c_u8g2_config_t cfg = i2c_u8g2_config_default();
ESP_ERROR_CHECK(i2c_u8g2_init(&display, &cfg));

u8g2_t *u8g2 = i2c_u8g2_get_u8g2(&display);
u8g2_ClearBuffer(u8g2);
u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
u8g2_DrawStr(u8g2, 0, 12, "Hello");
u8g2_SendBuffer(u8g2);
```

## Notes

- Default panel setup is `u8g2_Setup_ssd1306_i2c_128x64_noname_f`; change
  `cfg.setup_fn` for a different controller.
- The OLED is full-buffer and `SendBuffer` is blocking I2C (~20 ms), so the UI
  task on core 0 renders on change only (signature-compared) and flushes once
  per frame, not per renderer.
- For multi-task access, guard U8g2 calls with your own mutex — the project does
  all drawing from the single `synth_ui` task.
