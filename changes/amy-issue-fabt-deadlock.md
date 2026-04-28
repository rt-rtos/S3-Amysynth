# Bug: Render task and audio permanently frozen when using `AMY_AUDIO_IS_NONE` with default `platform.multithread=1`

## Environment

| Field | Value |
|---|---|
| Framework | ESP-IDF 6.0 |
| Target | ESP32-S3 |
| AMY version | latest `main` (as of 2026-03-31) |
| Audio mode | `AMY_AUDIO_IS_NONE` (project drives audio manually via TinyUSB UAC) |

## Symptom

After a clean boot, the monitor shows `render_blocks=0` and `seq_tick=0` indefinitely.
No audio is produced, the sequencer playhead never advances, and all `amy_sysclock()`-derived timing is frozen:

```
I (1616) main: Main loop idle... seq_tick=0 tick_hook_calls=0 render_blocks=0 render_sysclock_ms=0
I (6626) main: Main loop idle... seq_tick=0 tick_hook_calls=0 render_blocks=0 render_sysclock_ms=0
I (11626) main: Main loop idle... seq_tick=0 tick_hook_calls=0 render_blocks=0 render_sysclock_ms=0
```

No assertion, no panic — the firmware boots successfully but produces no audio and no sequencer motion. Play/pause and BPM changes appear to work (callbacks fire, logs emit) but nothing plays.

## Root Cause

### The call graph

`amy_default_config()` returns a config with:
```c
c.platform.multicore   = 1;
c.platform.multithread = 1;
```

When `amy_start(cfg)` is called from `app_main`, it does:
```
amy_start()
  → global_init()
  → oscs_init()     // also calls sequencer_init → _sequencer_start (hardware timer OK)
  → amy_platform_init()   ← problem is here
```

Inside `amy_platform_init()` on ESP (`i2s.c`):
```c
void amy_platform_init() {
    amy_update_handle = xTaskGetCurrentTaskHandle();   // ← captures app_main's handle
    if (AMY_HAS_I2S) { esp32_setup_i2s(); }
    if (amy_global.config.platform.multicore) {
        xTaskCreatePinnedToCore(&esp_render_task, ...);
    }
    if (amy_global.config.platform.multithread) {
        xTaskCreatePinnedToCore(&esp_fill_audio_buffer_task, ...);  // ← spawns FABT
        if (!AMY_HAS_I2S) {
            xTaskNotifyGive(amy_fill_buffer_handle);   // ← kicks FABT once
        }
    }
}
```

`AMY_HAS_I2S` is false (audio mode is NONE), so FABT receives **one** initial notification and runs one block on startup. After rendering that block, at the bottom of `esp_fill_audio_buffer_task`:

```c
// at the end of every FABT iteration (non-I2S path):
xTaskNotifyGive(amy_update_handle);   // ← notifies app_main, NOT our render task
```

### Our render task path

The project's `amy_usb_render_task` calls `amy_update()`:

```c
// api.c
int16_t *amy_update() {
    amy_update_tasks();
    int16_t *block = amy_render_audio();   // ← blocks here
    ...
}
```

With `multithread=1`, `amy_render_audio()` on ESP does:
```c
int16_t *amy_render_audio() {
    if (amy_global.config.platform.multithread) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // ← waits for notification on THIS task's handle
        buf = last_audio_buffer;
        if (!AMY_HAS_I2S) {
            xTaskNotifyGive(amy_fill_buffer_handle);  // re-arm FABT for next block
        }
    }
    ...
}
```

### The deadlock

| Handle | Who holds it | Who notifies it |
|---|---|---|
| `amy_update_handle` | `app_main` (captured at `amy_platform_init` time) | FABT (every block) |
| `amy_usb_render_task` handle | our render task (waiting in `ulTaskNotifyTake`) | nobody |
| `amy_fill_buffer_handle` | FABT | `amy_render_audio()` ... which never returns |

- `amy_usb_render_task` blocks in `ulTaskNotifyTake` forever — nobody ever notifies it.
- FABT renders one block (the startup kick), notifies `app_main` (which ignores it), then waits for our render task to re-arm it via `xTaskNotifyGive(amy_fill_buffer_handle)` — which never happens.
- `total_blocks` stops at 1 (the single startup block). `amy_sysclock()` → 0 ms. Sequencer timer fires but `sequencer_check_and_fill()` always sees `next_amy_tick_us` in the past and stalls.

### Why it looked like step 0 only

The single startup FABT render advances `total_blocks` to 1 and `amy_sysclock()` to ~5 ms. The drum synth's first scheduled event (step 0, tick 1) may fire and produce sound once. After that, everything is frozen. From the user's perspective: sequencer appears stuck on the first step, plays audio on step 0 only (or on the first press of play), then no further sequencer motion.

### Why it regressed with the pot removal

Before, `amy_update_handle` may have been initialised earlier in the boot (if the pot reader task happened to be the one touching AMY), creating a timing lucky-path. After removing the pot reader task, `app_main` calls `amy_start()` directly, so `amy_update_handle` is permanently `app_main`'s handle. The deadlock became unconditional.

## Fix

Disable AMY's internal background tasks before calling `amy_start()`. In `main/main.c`:

```c
amy_config_t amy_cfg = amy_default_config();
amy_cfg.audio = AMY_AUDIO_IS_NONE;
amy_cfg.platform.multicore   = 0;   // ← add this
amy_cfg.platform.multithread = 0;   // ← add this
amy_cfg.amy_external_sequencer_hook = main_sequencer_tick_hook;
amy_start(amy_cfg);
```

With both flags cleared:
- `amy_platform_init()` creates neither FABT nor the secondary render task.
- `amy_render_audio()` takes the synchronous `else` branch: renders all oscillators in-task.
- `amy_update()` returns a valid block every call. `total_blocks` increments. `amy_sysclock()` advances.
- The sequencer hardware timer (500 µs) drives `sequencer_check_and_fill()` correctly.
- `amy_usb_render_task` pushes each block to `usb_audio_write_stereo()` as designed.

## Performance Impact

Without multicore AMY rendering, all oscillator rendering runs on whichever core `amy_usb_render_task` is pinned to (core 1). At 160 MHz and 48 kHz / 256-sample blocks, this leaves ~5.3 ms per block for synthesis. The project uses 4 sequencer tracks with a percussion synth — well within single-core budget. If the osc count is raised significantly in future, re-enabling `multicore=1` (but still keeping `multithread=0`) can split rendering across both cores without reintroducing the deadlock, as long as `amy_update_handle` is set to the render task's handle before calling `amy_start()`.

## Upstream Note

The `AMY_AUDIO_IS_NONE` + custom render loop pattern is valid and documented, but the interaction with the default `multithread=1` is a footgun: `amy_update_handle` is always the task that calls `amy_platform_init()`, and on ESP-IDF that is typically `app_main`, not the user's render task. A minimal upstream fix would be: when `AMY_AUDIO_IS_NONE && platform.multithread`, skip spawning FABT entirely, since there is no I2S sink to pace the render loop.
