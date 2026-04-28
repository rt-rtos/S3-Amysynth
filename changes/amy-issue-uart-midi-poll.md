# Bug: `uart_read_bytes()` errors when AMY runs with `AMY_MIDI_IS_NONE` but still polls MIDI on ESP-IDF

## Environment

| Field | Value |
|---|---|
| Framework | ESP-IDF 6.0 |
| Target | ESP32-S3 |
| AMY version | latest `main` (as of 2026-04-02) |
| Audio mode | `AMY_AUDIO_IS_NONE` with custom USB audio render loop |

## Symptom

The serial monitor shows repeated UART errors during startup and runtime:

```text
E (3105) uart: uart_read_bytes(1727): uart driver error
E (3129) uart: uart_read_bytes(1727): uart driver error
E (3140) uart: uart_read_bytes(1727): uart driver error
```

The error appears even though this project is not using UART MIDI. The noise can be mistaken for a log-level problem, but it is a real driver error.

## Root Cause

`amy_default_config()` sets:

```c
c.midi = AMY_MIDI_IS_NONE;
```

for ESP32 builds, but it also sets:

```c
c.midi_uart = 1;
```

In the ESP-IDF path, `amy_update_tasks()` in `components/amy/src/i2s.c` called `esp_poll_midi()` whenever `platform.multithread == 0`, regardless of whether UART MIDI was actually enabled.

That reaches `esp_poll_midi()` in `components/amy/src/amy_midi.c`, which does:

```c
int length = uart_read_bytes(uart_num, data, MAX_MIDI_BYTES_TO_PARSE, 1/portTICK_PERIOD_MS);
```

with `uart_num` resolved from `c.midi_uart`. Because UART MIDI was never initialised, the ESP-IDF UART driver reports `uart driver error`.

## Fix

Guard the poll in `components/amy/src/i2s.c` so it only runs when UART MIDI is enabled:

```c
if (amy_global.config.midi & AMY_MIDI_IS_UART) {
    esp_poll_midi();
}
```

This is the minimal fix because it preserves the existing MIDI path for builds that intentionally enable UART MIDI, while avoiding a bogus driver call in USB-audio-only mode.

## Why This Is Safe

- It does not change the AMY audio path.
- It does not affect builds that explicitly enable UART MIDI.
- It only removes an invalid poll when the UART MIDI feature is off.

## Rollback

Remove the `AMY_MIDI_IS_UART` guard and restore the unconditional call to `esp_poll_midi()`.

## Notes

The error was easy to misread because other startup logs were also printing at debug level. The underlying UART failure is independent of log verbosity.