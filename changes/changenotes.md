<img width="1135" height="121" alt="sadb0y" src="https://github.com/user-attachments/assets/a1bc0b42-7432-451b-908b-dc17cde8047c" />

## AMY i2s.c runtime note (USB/UAC mode)

This project currently runs AMY with `audio = AMY_AUDIO_IS_NONE` and uses a custom render task (`amy_update`) to feed USB UAC output.

### What changed in AMY

File changed: `components/amy/src/i2s.c`

In the ESP multithread path (`amy_render_audio`), we now re-bind `amy_update_handle` to the task that is actually calling `amy_update` at runtime.

Why this was needed:
- `amy_platform_init` originally captures `amy_update_handle` during `amy_start` (inside `app_main`).
- In this project, `amy_update` is called from a separate FreeRTOS task (`amy_usb_render_task`).
- Without re-binding, the AMY fill-buffer task notifies the wrong task handle, causing a deadlock in `amy_update`.
- Symptom was: sequencer ticks stuck at 0 and render blocks stuck at 0.

### Safety / impact when switching back to hardware I2S

This change is safe for I2S mode and should not require removal.

- In hardware I2S mode, AMY typically owns the audio pipeline and this re-bind is effectively a no-op unless task ownership changes.
- If you move back to AMY-managed I2S output, keep using normal AMY startup config and verify logs show stable audio task startup.
- If you later run `amy_update` from a different task again, this patch remains necessary and correct.

### Quick regression check

After any audio routing change (USB UAC vs I2S), confirm in monitor logs:
- render blocks increase over time
- sequencer tick count increases over time
- playhead advances on UI
