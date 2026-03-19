#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize USB Audio Class device (UAC Microphone – ESP → Host only)
 *
 * After this call the board appears as a USB audio input device on your PC/DAW.
 * Configure channels / sample rate in menuconfig → Component config → ESP-IoT-Solution → USB Device UAC
 */
esp_err_t usb_audio_init(void);

/**
 * @brief Write interleaved stereo 16-bit samples (L, R, L, R, ...)
 *
 * Call this from your sequencer mixing task (every 5–10 ms is perfect).
 * If the internal ring buffer is full, samples are dropped (safe underrun behaviour).
 */
esp_err_t usb_audio_write_stereo(const int16_t *data, size_t num_frames);

/**
 * @brief Write mono 16-bit samples (automatically duplicated to L+R)
 */
esp_err_t usb_audio_write_mono(const int16_t *data, size_t num_samples);

#ifdef __cplusplus
}
#endif