#include "usb_audio.h"
#include "usb_device_uac.h"
#include "tusb.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

#ifndef CONFIG_USB_AUDIO_DIAGNOSTICS
#define CONFIG_USB_AUDIO_DIAGNOSTICS 0
#endif

static const char *TAG = "usb_audio";

// ~170 ms buffer @ 48 kHz stereo 16-bit → very safe for drum prototyping
#define RING_BUFFER_SIZE  (32768)   // must be power-of-2 for fast modulo
static int16_t s_ring_buffer[RING_BUFFER_SIZE];
static volatile size_t s_write_idx = 0;
static volatile size_t s_read_idx  = 0;
static SemaphoreHandle_t s_mutex = NULL;

static bool s_initialized = false;

#if CONFIG_USB_AUDIO_DIAGNOSTICS
static uint32_t s_write_calls = 0;
static uint32_t s_write_drop_events = 0;
static uint32_t s_underrun_events = 0;
static size_t s_peak_fill_samples = 0;
static int16_t s_peak_abs_sample = 0;
#endif

static inline size_t usb_audio_fill_samples_unsafe(void)
{
    return (s_write_idx - s_read_idx + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
}

static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *cb_ctx)
{
    (void)cb_ctx;

    if (!s_initialized) {
        memset(buf, 0, len);
        *bytes_read = len;
        return ESP_OK;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t available_samples = usb_audio_fill_samples_unsafe();
    size_t samples_to_copy = (len / 2 < available_samples) ? len / 2 : available_samples;

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    if (samples_to_copy < (len / 2)) {
        s_underrun_events++;
    }
#endif

    // Copy with wrap-around
    size_t first = RING_BUFFER_SIZE - s_read_idx;
    if (first > samples_to_copy) first = samples_to_copy;

    memcpy(buf, &s_ring_buffer[s_read_idx], first * sizeof(int16_t));
    if (samples_to_copy > first) {
        memcpy(buf + first * sizeof(int16_t),
               s_ring_buffer,
               (samples_to_copy - first) * sizeof(int16_t));
    }

    s_read_idx = (s_read_idx + samples_to_copy) % RING_BUFFER_SIZE;
    *bytes_read = samples_to_copy * 2;   // bytes

    xSemaphoreGive(s_mutex);

    // Zero-pad on underrun (prevents pops)
    if (*bytes_read < len) {
        memset(buf + *bytes_read, 0, len - *bytes_read);
        *bytes_read = len;
    }

    return ESP_OK;
}

static void uac_set_mute_cb(uint32_t mute, void *ctx)
{
    ESP_LOGI(TAG, "Mute → %lu", mute);
}

static void uac_set_volume_cb(uint32_t volume, void *ctx)
{
    ESP_LOGI(TAG, "Volume → %lu%%", volume);
}

esp_err_t usb_audio_init(void)
{
    if (s_initialized) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;

    uac_device_config_t cfg = {
        .skip_tinyusb_init = false,
        .output_cb   = NULL,                    // we only need mic direction
        .input_cb    = uac_input_cb,
        .set_mute_cb = uac_set_mute_cb,
        .set_volume_cb = uac_set_volume_cb,
        .cb_ctx      = NULL
    };

    esp_err_t ret = uac_device_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uac_device_init failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_mutex);
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "USB Audio ready – 48 kHz 16-bit stereo mic");
    ESP_LOGI(TAG, "Plug into PC and select the new audio input device");
    return ESP_OK;
}

esp_err_t usb_audio_write_stereo(const int16_t *data, size_t num_frames)
{
    if (!s_initialized || !data || num_frames == 0) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    s_write_calls++;
#endif

    size_t free_slots = (s_read_idx - s_write_idx - 1 + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
    size_t samples_to_write = (num_frames * 2 <= free_slots) ? num_frames * 2 : free_slots;

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    int16_t local_peak = 0;
#endif

    for (size_t i = 0; i < samples_to_write; i++) {
#if CONFIG_USB_AUDIO_DIAGNOSTICS
        int16_t sample = data[i];
        int16_t abs_sample = (sample < 0) ? (int16_t)(-sample) : sample;
        if (abs_sample > local_peak) {
            local_peak = abs_sample;
        }
#endif
        s_ring_buffer[s_write_idx] = data[i];
        s_write_idx = (s_write_idx + 1) % RING_BUFFER_SIZE;
    }

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    if (local_peak > s_peak_abs_sample) {
        s_peak_abs_sample = local_peak;
    }
    size_t fill_samples = usb_audio_fill_samples_unsafe();
    if (fill_samples > s_peak_fill_samples) {
        s_peak_fill_samples = fill_samples;
    }
#endif

    xSemaphoreGive(s_mutex);

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    if (samples_to_write != num_frames * 2) {
        s_write_drop_events++;
    }
#endif

    return (samples_to_write == num_frames * 2) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t usb_audio_write_mono(const int16_t *data, size_t num_samples)
{
    for (size_t i = 0; i < num_samples; i++) {
        int16_t frame[2] = {data[i], data[i]};
        usb_audio_write_stereo(frame, 1);
    }
    return ESP_OK;
}

void usb_audio_diag_get_snapshot(usb_audio_diag_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->initialized = s_initialized;

    if (!s_initialized || s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snapshot->fill_samples = usb_audio_fill_samples_unsafe();
#if CONFIG_USB_AUDIO_DIAGNOSTICS
    snapshot->peak_fill_samples = s_peak_fill_samples;
    snapshot->write_calls = s_write_calls;
    snapshot->write_drop_events = s_write_drop_events;
    snapshot->underrun_events = s_underrun_events;
    snapshot->peak_abs_sample = s_peak_abs_sample;
#endif
    xSemaphoreGive(s_mutex);
}

void usb_audio_diag_reset(void)
{
#if CONFIG_USB_AUDIO_DIAGNOSTICS
    if (!s_initialized || s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_write_calls = 0;
    s_write_drop_events = 0;
    s_underrun_events = 0;
    s_peak_fill_samples = 0;
    s_peak_abs_sample = 0;
    xSemaphoreGive(s_mutex);
#else
    (void)0;
#endif
}

// from main int16_t mix[960];   // 10 ms @ 48 kHz mono
// ... fill mix with your drum samples ... usb_audio_write_mono(mix, 960);