# Rotary Encoder Component

This helper configures an ESP32S3 pulse counter (PCNT) unit and exposes a reusable API for handling quadrature rotary encoders.

## Features
- Dual-channel PCNT setup (edge + level actions) for X4 decoding.
- Optional glitch filter, user-configurable high/low limits, and watch-points.
- Watch-point notification queue with configurable depth.
- Thin wrapper that manages unit/channel creation, callbacks, and cleanup.

## Primary API
| Function | Description |
| --- | --- |
| `rotary_encoder_default_config(pin_a, pin_b)` | Build a `rotary_encoder_config_t` with sane defaults for the provided phase pins. |
| `rotary_encoder_new_with_config(config, &handle)` | Create/start an encoder using the supplied config. Watch points, limits, filters, and queue size can all be tuned here. |
| `rotary_encoder_new(pin_a, pin_b, &handle)` | Convenience helper that simply calls `rotary_encoder_default_config()` and creates the encoder. |
| `rotary_encoder_get_count(handle)` | Reads the signed counter value. |
| `rotary_encoder_reset(handle)` | Clears the PCNT unit count. |
| `rotary_encoder_get_event_queue(handle)` | Returns the queue that receives watch-point notifications (each enqueue carries the reached watch-point value). |
| `rotary_encoder_delete(handle)` | Stops the PCNT unit, deletes the queue, and frees the handle.

Adds the component directory to your CMake graph (handled automatically when placed under `components/`). Callers only need to include `rotary_encoder.h` and link against ESP-IDF's standard drivers component.
