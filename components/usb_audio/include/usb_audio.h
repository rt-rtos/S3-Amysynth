#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
	bool    initialized;
	size_t  fill_samples;
	size_t  peak_fill_samples;
	uint32_t write_calls;
	uint32_t write_drop_events;
	/* underrun_events and zlp_events mirror dropout_stats (diagnostics
	 * component) and are valid in every build (underrun_events always reads
	 * 0); the other counters above are live only with
	 * CONFIG_USB_AUDIO_DIAGNOSTICS. */
	uint32_t underrun_events;
	uint32_t zlp_events;
	int16_t  peak_abs_sample;
} usb_audio_diag_snapshot_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the USB Audio Class device (UAC microphone, ESP to host).
 *
 * On failure returns the uac_device_init error with the ring buffer freed.
 */
esp_err_t usb_audio_init(void);

/**
 * @brief Write interleaved stereo 16-bit frames (L, R, L, R, ...)
 *
 * Single producer: the AMY render task. Returns ESP_OK without writing while
 * no host is consuming, ESP_ERR_NO_MEM with nothing written when the ring
 * lacks room for the whole block, and ESP_ERR_INVALID_STATE if uninitialized,
 * data is NULL or num_frames is 0.
 */
esp_err_t usb_audio_write_stereo(const int16_t *data, size_t num_frames);

/**
 * @brief True iff a USB host has pulled audio within the liveness timeout.
 *        Producer-side gate so render output is not buffered/dropped when no
 *        host is consuming. Advisory (relaxed atomics).
 */
bool usb_audio_consumer_active(void);

/**
 * @brief Lock-free advisory peek at the most recent audio committed to the ring.
 *
 * Peak and mean |sample| over the n_samples immediately behind the write
 * index; *write_idx receives a snapshot of that index (compare across calls to
 * detect "no new audio produced").
 *
 * Safe from any task/core without touching the SPSC contract: the acquire load
 * guarantees every sample behind the index is committed, and the producer
 * cannot rewrite that region until it wraps the whole ring (~170 ms), far
 * longer than one scan. Only compiled with CONFIG_OUTPUT_WATCHDOG.
 *
 * @return false if the driver is not initialized (outputs are zeroed).
 */
bool usb_audio_peek_levels(size_t n_samples, int32_t *peak_abs,
                           int32_t *mean_abs, size_t *write_idx);

/**
 * @brief Get a snapshot of USB audio diagnostic counters and buffer state.
 */
void usb_audio_diag_get_snapshot(usb_audio_diag_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif