/* drum_cache.c - per-preset PSRAM windows over the gamma9001 flash partition.
 * The header carries the contract. */

#include "custompatches/drum_cache.h"

#include <stdatomic.h>
#include "esp_log.h"
#include "amy.h"
#include "sequencer_core.h"
#include "seq_core_config.h"

static const char *TAG = "drum_cache";

#ifdef GAMMA9001

/* Render blocks between a release and its unload; covers the queued osc
 * reset plus a margin. */
#define DRUM_CACHE_RELEASE_BLOCKS 4u

/* Table size; the bank's real count comes from AMY at init and is clamped
 * to this (the gamma9001 header that defines it cannot be included twice). */
#define DRUM_CACHE_MAX_PRESETS 136u

typedef struct {
    bool     loaded;
    uint32_t bytes;
    _Atomic uint8_t release_countdown;   /* >0: draining on the render task */
    bool     releasing;
} drum_window_t;

static const esp_partition_t *s_part;
static drum_window_t s_win[DRUM_CACHE_MAX_PRESETS];
static uint32_t s_bytes;
static uint16_t s_base;    /* first gamma preset number */
static uint16_t s_count;   /* presets in the bank, <= DRUM_CACHE_MAX_PRESETS */

static inline uint16_t preset_of(uint16_t i) { return (uint16_t)(s_base + i); }

void drum_cache_init(const esp_partition_t *part)
{
    s_part  = part;
    s_base  = amy_gamma9001_preset_base();
    s_count = amy_gamma9001_preset_count();
    if (s_count > DRUM_CACHE_MAX_PRESETS) s_count = DRUM_CACHE_MAX_PRESETS;
}

bool drum_cache_available(void)
{
    return s_part != NULL;
}

uint32_t drum_cache_bytes(void)
{
    return s_bytes;
}

/* Load one preset as a memory preset (PSRAM via ram_caps_sample) and fill it
 * from the partition. false on OOM or a read error; the preset stays silent.
 * The fill happens outside the lock: nothing references the preset yet, since
 * every caller syncs before it points an osc at a new number. */
static bool window_load(uint16_t i)
{
    amy_gamma9001_span_t sp;
    if (!amy_gamma9001_preset_span(preset_of(i), &sp) || sp.length == 0) return false;
    uint32_t bytes = sp.length * (uint32_t)sizeof(int16_t);
    /* pcm_load() mutates pcm.c's unlocked memory-preset list, which
     * render_pcm() walks inside the render body. */
    amy_grab_lock();
    int16_t *buf = pcm_load(preset_of(i), sp.length, sp.samplerate, 1,
                            sp.midinote, sp.loopstart, sp.loopend);
    amy_release_lock();
    if (buf == NULL) {
        ESP_LOGW(TAG, "preset %u: no PSRAM for %lu KB", (unsigned)preset_of(i),
                 (unsigned long)(bytes / 1024u));
        return false;
    }
    if (esp_partition_read(s_part, (size_t)sp.offset * sizeof(int16_t), buf, bytes) != ESP_OK) {
        ESP_LOGW(TAG, "preset %u: partition read failed", (unsigned)preset_of(i));
        amy_grab_lock();
        pcm_unload_preset(preset_of(i));
        amy_release_lock();
        return false;
    }
    s_win[i].loaded = true;
    s_win[i].bytes  = bytes;
    s_bytes += bytes;
    return true;
}

/* Start a release; the preset stays loaded (and readable) until the drain
 * ends and drum_cache_service unloads it. */
static void window_release(uint16_t i)
{
    s_win[i].releasing = true;
    atomic_store_explicit(&s_win[i].release_countdown, DRUM_CACHE_RELEASE_BLOCKS,
                          memory_order_release);
}

/* A release in flight is simply cancelled: the preset never went away. */
static void window_reclaim(uint16_t i)
{
    s_win[i].releasing = false;
    atomic_store_explicit(&s_win[i].release_countdown, 0, memory_order_relaxed);
}

void drum_cache_sync(void)
{
    if (s_part == NULL) return;
    bool wanted[DRUM_CACHE_MAX_PRESETS] = { false };
    if (sequencer_core_get_drum_engine() == SEQ_DRUM_PCM) {
        uint8_t n = sequencer_core_get_num_layers();
        for (uint8_t l = 0; l < n; l++) {
            if (sequencer_core_get_layer_type(l) != SEQ_LAYER_DRUM) continue;
            uint8_t rows = sequencer_core_get_layer_tracks(l);
            for (uint8_t t = 0; t < rows; t++) {
                uint16_t p = sequencer_core_get_drum_pcm_preset(l, t);
                if (p >= s_base && p < s_base + s_count) {
                    wanted[p - s_base] = true;
                }
            }
        }
    }
    uint32_t before = s_bytes;
    for (uint16_t i = 0; i < s_count; i++) {
        drum_window_t *w = &s_win[i];
        if (wanted[i]) {
            if (!w->loaded)         window_load(i);
            else if (w->releasing)  window_reclaim(i);
        } else if (w->loaded && !w->releasing) {
            window_release(i);
        }
    }
    if (s_bytes != before) {
        ESP_LOGI(TAG, "windows: %lu KB in PSRAM", (unsigned long)(s_bytes / 1024u));
    }
}

void drum_cache_service(void)
{
    for (uint16_t i = 0; i < s_count; i++) {
        drum_window_t *w = &s_win[i];
        if (!w->releasing || !w->loaded) continue;
        if (atomic_load_explicit(&w->release_countdown, memory_order_acquire) != 0) continue;
        amy_grab_lock();
        pcm_unload_preset(preset_of(i));
        amy_release_lock();
        s_bytes -= w->bytes;
        w->loaded = false;
        w->bytes = 0;
        w->releasing = false;
    }
}

void drum_cache_render_service(void)
{
    for (uint16_t i = 0; i < s_count; i++) {
        uint8_t c = atomic_load_explicit(&s_win[i].release_countdown, memory_order_relaxed);
        if (c > 0) {
            atomic_store_explicit(&s_win[i].release_countdown, (uint8_t)(c - 1),
                                  memory_order_release);
        }
    }
}

#else /* !GAMMA9001 */

void     drum_cache_init(const esp_partition_t *part) { (void)part; }
bool     drum_cache_available(void) { return false; }
void     drum_cache_sync(void) {}
void     drum_cache_service(void) {}
void     drum_cache_render_service(void) {}
uint32_t drum_cache_bytes(void) { return 0; }

#endif
