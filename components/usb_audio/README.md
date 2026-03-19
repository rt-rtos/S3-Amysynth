# USB Audio Component

This component provides a USB Audio Class 2.0 (UAC2) Microphone interface for the ESP32-S3. It allows the ESP32-S3 to appear as a standard USB audio input device on a host PC or DAW, streaming audio directly over USB without requiring an external DAC or audio interface.

## Features

- **UAC2 Microphone Device**: Appears as a standard USB audio input device on the host.
- **Stereo 16-bit Audio**: Streams interleaved stereo 16-bit audio samples.
- **Ring Buffer**: Uses an internal ring buffer (`RING_BUFFER_SIZE` = 16384) to safely handle audio data and prevent underruns.
- **AMY Integration**: Designed to work seamlessly with the AMY synthesizer engine, routing its output directly to USB.

## Architecture

- `usb_audio.c`: Contains the core logic for initializing the USB audio device, handling input callbacks, and managing the ring buffer.
- `include/usb_audio.h`: Exposes the public API for initializing the device and writing audio data.
- Relies on the `espressif/usb_device_uac` component for the underlying USB Audio Class implementation.
- Relies on `espressif__tinyusb` for the TinyUSB stack.

## Usage with AMY

To use this component with the AMY synthesizer:

1. **Configure AMY**: Set `amy_cfg.audio = AMY_AUDIO_IS_NONE` to disable AMY's built-in audio output (e.g., I2S).
2. **Initialize USB Audio**: Call `usb_audio_init()` after initializing AMY.
3. **Create an Audio Task**: Create a FreeRTOS task that continuously pulls audio blocks from AMY and writes them to the USB audio ring buffer.

```c
#include "amy.h"
#include "usb_audio.h"

static void amy_usb_audio_task(void *arg)
{
    (void)arg;

    while (1) {
        // Get the next block of stereo 16-bit samples from AMY
        int16_t *block = amy_simple_fill_buffer();

        if (block != NULL) {
            // Push the block into the USB ring buffer
            usb_audio_write_stereo(block, AMY_BLOCK_SIZE);
        }

        // Yield to keep real-time friendly
        vTaskDelay(1);
    }
}

void app_main(void)
{
    // ... other initialization ...

    // Configure and start AMY
    amy_config_t amy_cfg = amy_default_config();
    amy_cfg.audio = AMY_AUDIO_IS_NONE;
    amy_start(amy_cfg);

    // Initialize USB Audio
    ESP_ERROR_CHECK(usb_audio_init());

    // Launch the audio rendering task
    xTaskCreate(amy_usb_audio_task, "amy_usb_audio", 4096, NULL, 5, NULL);

    // ... rest of your application ...
}
```

## Configuration

The sample rate and number of channels can be configured via `menuconfig` under `Component config` -> `ESP-IoT-Solution` -> `USB Device UAC`. Ensure that the sample rate matches the rate used by AMY (default is 44.1 kHz, but can be configured to 48 kHz).
