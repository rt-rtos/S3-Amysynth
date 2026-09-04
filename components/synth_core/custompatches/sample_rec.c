#include "custompatches/sample_rec.h"
#include "sequencer_core.h"
#include "amy.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "sample_rec";

/* Single runtime PCM slot. pcm.c's memory-preset lookup keys on preset_number
 * directly, so any number clear of the baked-in ROM range is safe. */
#define SAMPLE_REC_PRESET_NUMBER  100u
#define SAMPLE_REC_SECONDS        1.5f
#define SAMPLE_REC_LENGTH         ((uint32_t)(SAMPLE_REC_SECONDS * (float)AMY_SAMPLE_RATE))
#define SAMPLE_REC_MIDINOTE       60u   /* neutral reference pitch (C4) */

static _Atomic(sample_rec_state_t) s_state = SAMPLE_REC_IDLE;
static _Atomic bool     s_cancel_requested = false;
static _Atomic uint32_t s_write_index = 0;
static int16_t *s_buf = NULL;
static uint8_t  s_target_layer = 0;
static uint8_t  s_target_track = 0;

void sample_rec_init(void)
{
    atomic_store_explicit(&s_state, SAMPLE_REC_IDLE, memory_order_relaxed);
    atomic_store_explicit(&s_cancel_requested, false, memory_order_relaxed);
    atomic_store_explicit(&s_write_index, 0, memory_order_relaxed);
    s_buf = NULL;
    s_target_layer = 0;
    s_target_track = 0;
}

bool sample_rec_arm(uint8_t layer_idx, uint8_t track)
{
    if (atomic_load_explicit(&s_state, memory_order_acquire) != SAMPLE_REC_IDLE) {
        return false;
    }
    if (track >= SEQ_TRACKS) {
        return false;
    }

    /* pcm_load() mutates pcm.c's unlocked memory-preset list, which
     * render_pcm() walks inside the render body. Take the same lock
     * add_delta_to_queue() uses. */
    amy_grab_lock();
    int16_t *buf = pcm_load(SAMPLE_REC_PRESET_NUMBER, SAMPLE_REC_LENGTH,
                            AMY_SAMPLE_RATE, 1, SAMPLE_REC_MIDINOTE, 0, 0);
    if (buf != NULL) {
        memset(buf, 0, SAMPLE_REC_LENGTH * sizeof(int16_t));
    }
    amy_release_lock();

    if (buf == NULL) {
        ESP_LOGW(TAG, "arm failed: no RAM for %lu-sample slot",
                 (unsigned long)SAMPLE_REC_LENGTH);
        return false;
    }

    s_buf          = buf;
    s_target_layer = layer_idx;
    s_target_track = track;
    atomic_store_explicit(&s_write_index, 0, memory_order_relaxed);
    atomic_store_explicit(&s_cancel_requested, false, memory_order_relaxed);
    atomic_store_explicit(&s_state, SAMPLE_REC_ARMED, memory_order_release);
    ESP_LOGI(TAG, "armed: L%u T%u, %lu samples", layer_idx + 1u, track + 1u,
             (unsigned long)SAMPLE_REC_LENGTH);
    return true;
}

bool sample_rec_start(void)
{
    sample_rec_state_t expected = SAMPLE_REC_ARMED;
    atomic_store_explicit(&s_write_index, 0, memory_order_relaxed);
    if (!atomic_compare_exchange_strong_explicit(&s_state, &expected, SAMPLE_REC_RECORDING,
                                                 memory_order_release, memory_order_acquire)) {
        return false;
    }
    ESP_LOGI(TAG, "recording started");
    return true;
}

bool sample_rec_assign(void)
{
    if (atomic_load_explicit(&s_state, memory_order_acquire) != SAMPLE_REC_READY) {
        return false;
    }
    /* Store the override BEFORE switching engines: set_drum_engine() below
     * reconfigures every drum track from scratch and reads it while doing so. */
    sequencer_core_set_drum_pcm_preset(s_target_layer, s_target_track,
                                       SAMPLE_REC_PRESET_NUMBER);
    sequencer_core_set_drum_engine(SEQ_DRUM_PCM);
    atomic_store_explicit(&s_state, SAMPLE_REC_IDLE, memory_order_release);
    ESP_LOGI(TAG, "assigned: L%u T%u", s_target_layer + 1u, s_target_track + 1u);
    return true;
}

void sample_rec_cancel(void)
{
    if (atomic_load_explicit(&s_state, memory_order_acquire) == SAMPLE_REC_IDLE) {
        return;
    }
    /* Deferred: pcm_unload_preset() + state reset run on the render task
     * (sample_rec_render_tick), never here. Freeing the buffer while the
     * render task may be mid-write would be a cross-core use-after-free;
     * tearing down at a block boundary keeps it single-writer. */
    atomic_store_explicit(&s_cancel_requested, true, memory_order_release);
}

sample_rec_state_t sample_rec_get_state(void)
{
    return atomic_load_explicit(&s_state, memory_order_relaxed);
}

uint8_t sample_rec_get_progress_pct(void)
{
    if (atomic_load_explicit(&s_state, memory_order_relaxed) != SAMPLE_REC_RECORDING) {
        return 0;
    }
    uint32_t idx = atomic_load_explicit(&s_write_index, memory_order_relaxed);
    return (uint8_t)((idx * 100u) / SAMPLE_REC_LENGTH);
}

void sample_rec_render_tick(const int16_t *interleaved_stereo, uint16_t frames)
{
    sample_rec_state_t st = atomic_load_explicit(&s_state, memory_order_acquire);
    if (st == SAMPLE_REC_IDLE) {
        return;
    }

    if (atomic_load_explicit(&s_cancel_requested, memory_order_acquire)) {
        amy_grab_lock();
        pcm_unload_preset(SAMPLE_REC_PRESET_NUMBER);
        amy_release_lock();
        s_buf = NULL;
        atomic_store_explicit(&s_write_index, 0, memory_order_relaxed);
        atomic_store_explicit(&s_cancel_requested, false, memory_order_relaxed);
        atomic_store_explicit(&s_state, SAMPLE_REC_IDLE, memory_order_release);
        ESP_LOGI(TAG, "cancelled");
        return;
    }

    if (st != SAMPLE_REC_RECORDING) {
        return;   /* ARMED or READY: nothing to capture this block */
    }

    uint32_t idx = atomic_load_explicit(&s_write_index, memory_order_relaxed);
    uint32_t remaining = SAMPLE_REC_LENGTH - idx;
    uint32_t n = ((uint32_t)frames < remaining) ? (uint32_t)frames : remaining;

    int16_t *dst = s_buf + idx;
    for (uint32_t i = 0; i < n; i++) {
        int32_t l = interleaved_stereo[i * 2];
        int32_t r = interleaved_stereo[i * 2 + 1];
        dst[i] = (int16_t)((l + r) / 2);
    }

    idx += n;
    atomic_store_explicit(&s_write_index, idx, memory_order_release);

    if (idx >= SAMPLE_REC_LENGTH) {
        atomic_store_explicit(&s_state, SAMPLE_REC_READY, memory_order_release);
        ESP_LOGI(TAG, "capture complete (%lu samples)", (unsigned long)SAMPLE_REC_LENGTH);
    }
}
