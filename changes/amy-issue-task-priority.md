# Bug: `AMY_RENDER_TASK_PRIORITY` crashes on ESP-IDF 6.0 — `ESP_TASK_PRIO_MAX` equals `configMAX_PRIORITIES`, which is an invalid FreeRTOS priority

## Environment

| Field | Value |
|---|---|
| Framework | ESP-IDF 6.0 (not Arduino) |
| Target | ESP32-S3 |
| AMY version | latest `main` (as of 2026-03-22) |

## Problem

AMY crashes immediately at boot with:

```
assert failed: prvInitialiseNewTask tasks.c:1111 (uxPriority < ( 25 ))

Backtrace:
--- 0x4200fe9e: amy_platform_init at components/amy/src/i2s.c:293
--- 0x4200fc31: amy_start at components/amy/src/api.c:361
```

The crash happens inside `amy_platform_init` → `xTaskCreatePinnedToCore` in `src/i2s.c:293`, before any audio initialisation occurs. The device boot-loops unconditionally.

## Root Cause

In `src/amy.h`, the non-Arduino ESP-IDF path defines:

```c
// src/amy.h (non-Arduino path)
#define AMY_RENDER_TASK_PRIORITY      (ESP_TASK_PRIO_MAX)
#define AMY_FILL_BUFFER_TASK_PRIORITY (ESP_TASK_PRIO_MAX)
```

`ESP_TASK_PRIO_MAX` is defined by ESP-IDF as:

```c
// esp_system/include/esp_task.h
#define ESP_TASK_PRIO_MAX (configMAX_PRIORITIES)   // = 25
```

FreeRTOS's `prvInitialiseNewTask` asserts **strict less-than**:

```c
// FreeRTOS-Kernel/tasks.c:1111
configASSERT( uxPriority < configMAX_PRIORITIES );
```

Passing `configMAX_PRIORITIES` (25) is therefore **always invalid** on any standard ESP-IDF build and crashes unconditionally. This is not a configuration issue — it is a hardcoded invalid value.

### The SDK documents the correct pattern

The `esp_task.h` header itself carries this comment immediately above the definition:

> *otherwise use `ESP_TASK_PRIO_MAX - X` style*

Every other ESP-IDF internal task follows this convention, e.g.:

```c
#define ESP_TASK_BT_CONTROLLER_PRIO   (ESP_TASK_PRIO_MAX - 2)
#define ESP_TASK_TIMER_PRIO           (ESP_TASK_PRIO_MAX - 3)
```

### AMY's own MIDI task already does this correctly

```c
// src/amy_midi.h
#define MIDI_TASK_PRIORITY (ESP_TASK_PRIO_MAX - 2)  // ✓ correct
```

This inconsistency between `amy.h` and `amy_midi.h` suggests the render/fill-buffer priorities were set without this constraint in mind.

## Proposed Fix

```c
// src/amy.h — non-Arduino ESP-IDF path
#define AMY_RENDER_TASK_PRIORITY      (ESP_TASK_PRIO_MAX - 1)
#define AMY_FILL_BUFFER_TASK_PRIORITY (ESP_TASK_PRIO_MAX - 1)
```

Priority 24 is the highest valid user-space priority on ESP-IDF. It still preempts all normal application tasks with no practical difference in real-time audio behaviour.

## Steps to Reproduce

1. Use AMY as a pure ESP-IDF component (not via Arduino)
2. Call `amy_start()` from `app_main`
3. Observe immediate boot crash — no audio output ever begins

## When Was This Introduced?

This is a **regression introduced in AMY commit `0aee666`** (2025-05-15), titled *"moving task names to amy.h for tulip"*.

Before that commit, `AMY_RENDER_TASK_PRIORITY` and `AMY_FILL_BUFFER_TASK_PRIORITY` did not exist in `amy.h` — priorities were defined elsewhere. That commit added them as `ESP_TASK_PRIO_MAX` without accounting for the strict less-than constraint.

The likely reason it wasn't caught: the change was made targeting the **Tulip CC board**, which runs a customised ESP-IDF build where `configMAX_PRIORITIES` is set to a higher value, making priority 25 valid in that environment. On a standard stock ESP-IDF project the default is `configMAX_PRIORITIES = 25`, so any value ≥ 25 is unconditionally invalid.

The ESP-IDF SDK itself has not changed — `ESP_TASK_PRIO_MAX = configMAX_PRIORITIES` has been defined that way for years. This is purely an AMY-side regression.

## Additional Notes

The Arduino path already hard-codes priority 20 as an apparent workaround:

```c
#ifdef ARDUINO
#define AMY_RENDER_TASK_PRIORITY      (20)
#define AMY_FILL_BUFFER_TASK_PRIORITY (20)
```

This suggests the issue has been encountered before on Arduino but the native IDF path was never corrected to match.
