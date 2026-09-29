#include "usb_audio.h"
#include "usb_device_uac.h"
#include "tusb.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   // xTaskGetTickCount / pdMS_TO_TICKS
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_compiler.h"   // likely()/unlikely() branch hints
#include "diag_mem.h"
#include "dropout_stats.h"
#include <stdatomic.h>
#include <string.h>

#ifndef CONFIG_USB_AUDIO_DIAGNOSTICS
#define CONFIG_USB_AUDIO_DIAGNOSTICS 0
#endif

#ifndef CONFIG_OUTPUT_WATCHDOG
#define CONFIG_OUTPUT_WATCHDOG 0
#endif

static const char *TAG = "usb_audio";

// ~170 ms buffer @ 48 kHz stereo 16-bit → very safe for drum prototyping
#define RING_BUFFER_SIZE  (32768)   // must be power-of-2 for fast modulo
// 64 KB, allocated from PSRAM in usb_audio_init(): touched at USB-frame /
// render-block granularity, never in the per-sample DSP loop, so PSRAM latency
// is irrelevant while the internal-RAM relief is significant.
//
// LOCK-FREE SPSC, no mutex (a mutex here let the lower-priority consumer block
// the render task across PSRAM copies on a strict-1:1 5333 us budget). Exactly
// ONE producer (AMY render task, Core 1, usb_audio_write_stereo) and ONE
// consumer (usb_mic_task, Core 0, uac_input_cb):
//   - s_write_idx is WRITER-owned: only the producer stores it.
//   - s_read_idx is READER-owned: only the consumer stores it.
// Each side publishes its own index release-ordered AFTER its data copy and
// acquire-loads the other's BEFORE its copy, so an advanced index is never
// observed before the samples it points to are committed.
static int16_t *s_ring_buffer = NULL;
static _Atomic size_t s_write_idx = 0;   // writer-owned (producer)
static _Atomic size_t s_read_idx  = 0;   // reader-owned (consumer)

static bool s_initialized = false;

// Consumer-liveness: tick of the most recent host pull (uac_input_cb call).
// Written only by the consumer; read relaxed cross-core for advisory gating.
#define USB_AUDIO_CONSUMER_TIMEOUT_MS 200
static _Atomic uint32_t s_last_consumed_tick = 0;

bool usb_audio_consumer_active(void)
{
    if (!s_initialized) return false;
    uint32_t last = atomic_load_explicit(&s_last_consumed_tick, memory_order_relaxed);
    if (last == 0) return false;   // host never pulled yet
    return (xTaskGetTickCount() - last) < pdMS_TO_TICKS(USB_AUDIO_CONSUMER_TIMEOUT_MS);
}

#if CONFIG_USB_AUDIO_DIAGNOSTICS
// Producer-owned counters (written only by usb_audio_write_stereo).
static uint32_t s_write_calls = 0;
static uint32_t s_write_drop_events = 0;
static size_t s_peak_fill_samples = 0;
static int16_t s_peak_abs_sample = 0;
#endif

#if CONFIG_OUTPUT_WATCHDOG
// Third-party PEEK reader (output-level watchdog, UI task on Core 0). Outside
// the SPSC protocol: never stores an index, only acquire-loads s_write_idx so
// everything behind it is committed. The producer cannot rewrite [w - n, w)
// until the index wraps the whole ring (~170 ms), so a short scan at UI cadence
// cannot see torn data.
bool usb_audio_peek_levels(size_t n_samples, int32_t *peak_abs,
                           int32_t *mean_abs, size_t *write_idx)
{
    *peak_abs = 0;
    *mean_abs = 0;
    *write_idx = 0;
    if (!s_initialized || n_samples == 0) return false;
    // Cap the window so the |sample| accumulator cannot overflow int32 and the
    // scan stays deep inside the not-yet-rewritable region behind the index.
    if (n_samples > RING_BUFFER_SIZE / 4) n_samples = RING_BUFFER_SIZE / 4;

    size_t w = atomic_load_explicit(&s_write_idx, memory_order_acquire);
    size_t start = (w + RING_BUFFER_SIZE - n_samples) % RING_BUFFER_SIZE;
    int32_t peak = 0, acc = 0;
    for (size_t i = 0; i < n_samples; ++i) {
        int32_t v = s_ring_buffer[(start + i) & (RING_BUFFER_SIZE - 1)];
        if (v < 0) v = -v;
        acc += v;
        if (v > peak) peak = v;
    }
    *peak_abs = peak;
    *mean_abs = acc / (int32_t)n_samples;
    *write_idx = w;
    return true;
}
#endif

// Advisory fill level for diagnostics. Loads both indices relaxed; the result
// is a momentary estimate, never used for correctness decisions.
static inline size_t usb_audio_fill_samples_relaxed(void)
{
    size_t w = atomic_load_explicit(&s_write_idx, memory_order_relaxed);
    size_t r = atomic_load_explicit(&s_read_idx, memory_order_relaxed);
    return (w - r + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
}

static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *cb_ctx)
{
    (void)cb_ctx;

    if (!s_initialized) {
        *bytes_read = 0;
        return ESP_OK;
    }

    // Flush-on-resume: the consumer owns s_read_idx, so it may advance it to
    // s_write_idx to discard audio buffered during a host gap - playback starts
    // fresh instead of 200 ms late.
    uint32_t now = xTaskGetTickCount();
    uint32_t last = atomic_load_explicit(&s_last_consumed_tick, memory_order_relaxed);
    if (last != 0 && (now - last) > pdMS_TO_TICKS(USB_AUDIO_CONSUMER_TIMEOUT_MS)) {
        size_t w = atomic_load_explicit(&s_write_idx, memory_order_acquire);
        atomic_store_explicit(&s_read_idx, w, memory_order_release);
    }
    atomic_store_explicit(&s_last_consumed_tick, now, memory_order_relaxed);

    // CONSUMER side. s_read_idx is ours to advance; acquire s_write_idx so all
    // of [read, write) is guaranteed committed.
    size_t read_idx = atomic_load_explicit(&s_read_idx, memory_order_relaxed);
    size_t write_idx = atomic_load_explicit(&s_write_idx, memory_order_acquire);

    size_t available_samples = (write_idx - read_idx + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
    size_t samples_to_copy = (len / 2 < available_samples) ? len / 2 : available_samples;

    // Copy with wrap-around
    size_t first = RING_BUFFER_SIZE - read_idx;
    if (first > samples_to_copy) first = samples_to_copy;

    memcpy(buf, &s_ring_buffer[read_idx], first * sizeof(int16_t));
    if (samples_to_copy > first) {
        memcpy(buf + first * sizeof(int16_t),
               s_ring_buffer,
               (samples_to_copy - first) * sizeof(int16_t));
    }

    // Release-store: the producer sees the slots freed only after the copy.
    atomic_store_explicit(&s_read_idx,
                          (read_idx + samples_to_copy) % RING_BUFFER_SIZE,
                          memory_order_release);
    // Short returns are the contract, not a fault: the SOF-context pull
    // sizes requests by EP FIFO room, and handing back only real audio is
    // what lets the flow controller's packet sizing track the render rate
    // (async source). Zero-padding here would pin the EP FIFO full and
    // insert silence at the correction rate.
    *bytes_read = samples_to_copy * 2;   // bytes

    return ESP_OK;
}

// Strong override of TinyUSB's weak callback (the vendored usb_device_uac
// component leaves it unimplemented): the audio class driver calls this from
// the TinyUSB task (Core 0) after loading each isochronous IN frame,
// n_bytes_copied = bytes it could pull from its EP-IN software FIFO. Zero
// means a zero-length packet goes on the wire. Under the async-source pull
// that happens only when the device genuinely had no audio across a full
// EP FIFO's worth of frames - true starvation (render stalled or stream
// gap). Counting only; this path serves
// all USB traffic, so it must stay a bare increment. Must return true: the
// driver TU_VERIFYs the result and would abort the transfer chain on false.
bool tud_audio_tx_done_post_load_cb(uint8_t rhport, uint16_t n_bytes_copied,
                                    uint8_t func_id, uint8_t ep_in,
                                    uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)func_id;
    (void)ep_in;
    (void)cur_alt_setting;
    if (unlikely(n_bytes_copied == 0)) {
        dropout_count_wire_zlp();
    }
    return true;
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

    s_ring_buffer = heap_caps_malloc(RING_BUFFER_SIZE * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring_buffer == NULL) {
        ESP_LOGE(TAG, "ring buffer alloc failed (%u bytes PSRAM)",
                 (unsigned)(RING_BUFFER_SIZE * sizeof(int16_t)));
        return ESP_ERR_NO_MEM;
    }
    // Zero the ring: a level peek or the host's first read must never see
    // uninitialized PSRAM as (loud) audio.
    memset(s_ring_buffer, 0, RING_BUFFER_SIZE * sizeof(int16_t));

    // A SPIRAM cap request can be served from internal RAM under pressure, and
    // this buffer is large enough that the fallback would matter. Record where
    // it actually landed.
    diag_mem_track("usb-ring", s_ring_buffer,
                   RING_BUFFER_SIZE * sizeof(int16_t));

    atomic_store_explicit(&s_write_idx, 0, memory_order_relaxed);
    atomic_store_explicit(&s_read_idx, 0, memory_order_relaxed);

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
        heap_caps_free(s_ring_buffer);
        s_ring_buffer = NULL;
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

    // No host consuming → do not churn the ring. Keep it empty so the host gets
    // fresh audio on open. Not a drop (nobody is listening); report success.
    if (!usb_audio_consumer_active()) {
        return ESP_OK;
    }

    const size_t samples = num_frames * 2;   // interleaved L,R

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    s_write_calls++;   // producer-owned counter
#endif

    // PRODUCER side. s_write_idx is ours to advance; acquire s_read_idx so the
    // free-slot count reflects everything the consumer already drained.
    size_t write_idx = atomic_load_explicit(&s_write_idx, memory_order_relaxed);
    size_t read_idx  = atomic_load_explicit(&s_read_idx, memory_order_acquire);

    size_t free_slots = (read_idx - write_idx - 1 + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;

    // ALL-OR-NOTHING: a partial write would splice a half-block against the
    // next full block and mangle the waveform. A whole-block gap is absorbed
    // by the ~170 ms buffer / host resync instead.
    if (unlikely(samples > free_slots)) {
#if CONFIG_USB_AUDIO_DIAGNOSTICS
        s_write_drop_events++;
#endif
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    int16_t local_peak = 0;
#endif

    // Copy with wrap-around. The producer is the sole writer of these slots, so
    // this is safe before publishing the new write index.
    size_t first = RING_BUFFER_SIZE - write_idx;
    if (first > samples) first = samples;
    memcpy(&s_ring_buffer[write_idx], data, first * sizeof(int16_t));
    if (samples > first) {
        memcpy(s_ring_buffer, data + first, (samples - first) * sizeof(int16_t));
    }

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    for (size_t i = 0; i < samples; i++) {
        int16_t sample = data[i];
        int16_t abs_sample = (sample < 0) ? (int16_t)(-sample) : sample;
        if (abs_sample > local_peak) {
            local_peak = abs_sample;
        }
    }
    if (local_peak > s_peak_abs_sample) {
        s_peak_abs_sample = local_peak;
    }
#endif

    // Release-store: the consumer observes it only after the sample stores are
    // committed to PSRAM.
    atomic_store_explicit(&s_write_idx,
                          (write_idx + samples) % RING_BUFFER_SIZE,
                          memory_order_release);

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    size_t fill_samples = usb_audio_fill_samples_relaxed();
    if (fill_samples > s_peak_fill_samples) {
        s_peak_fill_samples = fill_samples;
    }
#endif

    return ESP_OK;
}

void usb_audio_diag_get_snapshot(usb_audio_diag_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->initialized = s_initialized;

    if (!s_initialized) {
        return;
    }

    // Advisory only. Each diag counter has a single owner task, so a plain read
    // can lose/gain one update but never corrupt state.
    snapshot->fill_samples = usb_audio_fill_samples_relaxed();

    // Underruns and wire ZLPs live in dropout_stats (always counted there);
    // mirrored into the snapshot so the serial diag line stays one-stop.
    dropout_stats_t drops;
    dropout_stats_get(&drops);
    snapshot->underrun_events = drops.ring_underrun;
    snapshot->zlp_events = drops.wire_zlp;
#if CONFIG_USB_AUDIO_DIAGNOSTICS
    snapshot->peak_fill_samples = s_peak_fill_samples;
    snapshot->write_calls = s_write_calls;
    snapshot->write_drop_events = s_write_drop_events;
    snapshot->peak_abs_sample = s_peak_abs_sample;
#endif
}
