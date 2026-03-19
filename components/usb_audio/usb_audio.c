#include "usb_audio.h"
#include "usb_device_uac.h"
#include "tusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "usb_audio";

// ~170 ms buffer @ 48 kHz stereo 16-bit → very safe for drum prototyping
#define RING_BUFFER_SIZE  (16384)   // must be power-of-2 for fast modulo
static int16_t s_ring_buffer[RING_BUFFER_SIZE];
static volatile size_t s_write_idx = 0;
static volatile size_t s_read_idx  = 0;
static SemaphoreHandle_t s_mutex = NULL;

static bool s_initialized = false;

static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *cb_ctx)
{
    (void)cb_ctx;

    if (!s_initialized) {
        memset(buf, 0, len);
        *bytes_read = len;
        return ESP_OK;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t available_samples = (s_write_idx - s_read_idx + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
    size_t samples_to_copy = (len / 2 < available_samples) ? len / 2 : available_samples;

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

    size_t free_slots = (s_read_idx - s_write_idx - 1 + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
    size_t samples_to_write = (num_frames * 2 <= free_slots) ? num_frames * 2 : free_slots;

    for (size_t i = 0; i < samples_to_write; i++) {
        s_ring_buffer[s_write_idx] = data[i];
        s_write_idx = (s_write_idx + 1) % RING_BUFFER_SIZE;
    }

    xSemaphoreGive(s_mutex);

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

// from main int16_t mix[960];   // 10 ms @ 48 kHz mono
// ... fill mix with your drum samples ... usb_audio_write_mono(mix, 960);