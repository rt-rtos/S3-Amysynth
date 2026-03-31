# AMY Local Edits

Track local, project-specific changes made against the upstream AMY component here.

## 2026-03-21

- Added ESP-IDF 6.0 compatibility comments and updated FreeRTOS task entry points in `components/amy/src/amy_midi.c` and `components/amy/src/i2s.c` so they use the required `void *` task signature.
- This is an external dependency compatibility edit, not a first-party AMY refactor.
- Excluded `components/amy/src/usb.c` from the ESP-IDF component build because this project provides its own USB implementation and does not need the MicroPython/Arduino USB path.

## 2026-03-31

- **Bug fix:** `AMY_SAMPLE_RATE` on `ESP_PLATFORM` defaulted to `44100` (the `#else` branch in `amy.h` lines 57–65), but `CONFIG_UAC_SAMPLE_RATE=48000` in `sdkconfig` means the TinyUSB UAC descriptor advertises 48 kHz to the Windows host. This caused AMY to render at 44.1 kHz while the host consumed samples as if they were 48 kHz — audio played ~88 cents flat and ~8% slow.
  - Added `#elif defined ESP_PLATFORM` → `48000` between the `__EMSCRIPTEN__` and `#else` cases in `amy.h`.
  - **Motivation:** UAC sample rate and AMY render rate must match; the minimal fix is a single added clause in the platform SR block.
  - **Risk:** Low. 48 kHz on S3 is well within hardware capability. PCM samples are internally stored at 22050 Hz and resampled — no change needed there. No time-sensitive path is altered; block size (256) stays the same.
  - **Rollback:** Remove the `#elif defined ESP_PLATFORM` / `48000` clause.

- **Config fix (not an AMY patch):** `platform.multithread` and `platform.multicore` must be set to `0` in `main.c` when using `AMY_AUDIO_IS_NONE`. With the defaults (`1`/`1`), `amy_platform_init()` spawns FABT and captures `app_main`'s task handle as `amy_update_handle`, causing a permanent deadlock between FABT and our `amy_usb_render_task` — `render_blocks` and `seq_tick` stay at 0. No change to AMY source required; fixed in `main/main.c`. See `amy-issue-fabt-deadlock.md` for full analysis.

## 2026-03-22

- **Bug fix:** `AMY_RENDER_TASK_PRIORITY` and `AMY_FILL_BUFFER_TASK_PRIORITY` in `components/amy/src/amy.h` changed from `ESP_TASK_PRIO_MAX` to `ESP_TASK_PRIO_MAX - 1`.
  - `ESP_TASK_PRIO_MAX` equals `configMAX_PRIORITIES` (25). FreeRTOS asserts `uxPriority < configMAX_PRIORITIES`, so passing 25 is unconditionally invalid and causes an immediate boot crash.
  - Filed upstream: see `amy-issue-task-priority.md`.
  - **Rollback:** revert the `- 1` subtraction in both defines.