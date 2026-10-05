#include "custompatches/wt_builder.h"
#include "custompatches/wavetable_bank.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "amy.h"
#include "sequencer_core.h"    /* SEQ_PATCH_WAVETABLE_APP_SLOTS, for WT_BUILDER_PRESET */
#include "sdkconfig.h"
#if CONFIG_SYNTH_CUSTOM_WT_TIMING
#include "esp_timer.h"
#endif

static const char *TAG = "wt_builder";

/* pcm_load() call shape of the wavetable bank (wavetable_bank.c). */
#define WT_BUILDER_SAMPLE_RATE 44100u
#define WT_BUILDER_MIDINOTE    69.0f
#define WT_TABLE_SAMPLES       ((uint32_t)WT_FRAMES * WT_CYCLE)

static wt_params_t  s_params;      /* written by the Core-0 setters */
static uint8_t      s_dirty;       /* __atomic only */
static wt_params_t  s_build;       /* snapshot the running build uses */
static uint8_t      s_next_frame;  /* 0 = idle; WT_FRAMES = finish pending */
static float       *s_scratch;     /* WT_TABLE_SAMPLES floats, PSRAM */
static int16_t     *s_live;        /* the preset's sample RAM */
static bool         s_ready;
static uint32_t     s_generation;
static wt_preview_t s_preview;

static void mark_dirty(void)
{
    __atomic_store_n(&s_dirty, 1, __ATOMIC_RELEASE);
}

bool wt_builder_init(void)
{
    wt_params_default(&s_params);

    s_scratch = heap_caps_malloc(WT_TABLE_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM);
    if (s_scratch == NULL) {
        ESP_LOGW(TAG, "no PSRAM for the %lu KB scratch, patch %u falls back",
                 (unsigned long)(WT_TABLE_SAMPLES * sizeof(float) / 1024u),
                 (unsigned)SEQ_PATCH_WAVETABLE_CUSTOM);
        return false;
    }
    /* pcm_load() mutates pcm.c's memory-preset list, which render_wavetable()
     * walks inside the render body. Nothing plays the preset before this init
     * returns, so the first table is written outside the lock. */
    amy_grab_lock();
    s_live = pcm_load((uint16_t)WT_BUILDER_PRESET, WT_TABLE_SAMPLES,
                      WT_BUILDER_SAMPLE_RATE, 1, WT_BUILDER_MIDINOTE, 0, 0);
    amy_release_lock();
    if (s_live == NULL) {
        heap_caps_free(s_scratch);
        s_scratch = NULL;
        ESP_LOGW(TAG, "no sample RAM for the %lu KB table, patch %u falls back",
                 (unsigned long)(WT_TABLE_SAMPLES * sizeof(int16_t) / 1024u),
                 (unsigned)SEQ_PATCH_WAVETABLE_CUSTOM);
        return false;
    }

    s_build = s_params;
    wt_synth_build_frames(&s_build, 0, WT_FRAMES, s_scratch);
    wt_synth_finish(s_scratch, (int16_t *)s_scratch, &s_preview);
    memcpy(s_live, s_scratch, WT_TABLE_SAMPLES * sizeof(int16_t));
    s_generation = 1;
    s_ready = true;
    ESP_LOGI(TAG, "custom wavetable on preset %u, patch %u",
             (unsigned)WT_BUILDER_PRESET, (unsigned)SEQ_PATCH_WAVETABLE_CUSTOM);
    return true;
}

void wt_builder_set_field(wt_field_t field, uint8_t keyframe, uint8_t value)
{
    uint8_t k = keyframe > WT_KEYS - 1 ? WT_KEYS - 1 : keyframe;
    wt_params_t t = s_params;
    switch (field) {
        case WT_FIELD_SHAPE:  t.shape[k]  = value; wt_params_clamp(&t); s_params.shape[k]  = t.shape[k];  break;
        case WT_FIELD_WIDTH:  t.width[k]  = value; wt_params_clamp(&t); s_params.width[k]  = t.width[k];  break;
        case WT_FIELD_BRIGHT: t.bright[k] = value; wt_params_clamp(&t); s_params.bright[k] = t.bright[k]; break;
        case WT_FIELD_SYNC:   t.sync[k]   = value; wt_params_clamp(&t); s_params.sync[k]   = t.sync[k];   break;
        case WT_FIELD_PEAK:   t.peak[k]   = value; wt_params_clamp(&t); s_params.peak[k]   = t.peak[k];   break;
        case WT_FIELD_RANGE:  t.range     = value; wt_params_clamp(&t); s_params.range     = t.range;     break;
        default: return;
    }
    mark_dirty();
}

void wt_builder_set_params(const wt_params_t *p)
{
    wt_params_t t = *p;
    wt_params_clamp(&t);
    s_params = t;
    mark_dirty();
}

void wt_builder_get_params(wt_params_t *out)
{
    *out = s_params;
}

void wt_builder_copy_keyframe(uint8_t from)
{
    uint8_t a = from > WT_KEYS - 1 ? WT_KEYS - 1 : from;
    uint8_t b = (uint8_t)((a + 1) % WT_KEYS);
    s_params.shape[b]  = s_params.shape[a];
    s_params.width[b]  = s_params.width[a];
    s_params.bright[b] = s_params.bright[a];
    s_params.sync[b]   = s_params.sync[a];
    s_params.peak[b]   = s_params.peak[a];
    mark_dirty();
}

void wt_builder_reset_keyframe(uint8_t k)
{
    wt_params_t d;
    wt_params_default(&d);
    if (k > WT_KEYS - 1) k = WT_KEYS - 1;
    s_params.shape[k]  = d.shape[k];
    s_params.width[k]  = d.width[k];
    s_params.bright[k] = d.bright[k];
    s_params.sync[k]   = d.sync[k];
    s_params.peak[k]   = d.peak[k];
    mark_dirty();
}

void wt_builder_blend_mid(void)
{
    wt_params_blend_mid(&s_params);
    mark_dirty();
}

void wt_builder_service(void)
{
    if (!s_ready) return;

    if (s_next_frame == 0) {
        if (!__atomic_exchange_n(&s_dirty, 0, __ATOMIC_ACQ_REL)) return;
        s_build = s_params;
    }

#if CONFIG_SYNTH_CUSTOM_WT_TIMING
    int64_t t0 = esp_timer_get_time();
#endif
    if (s_next_frame < WT_FRAMES) {
        uint8_t first = s_next_frame;
        wt_synth_build_frames(&s_build, first, WT_FRAMES_PER_SLICE, s_scratch);
        s_next_frame = (uint8_t)(first + WT_FRAMES_PER_SLICE);
#if CONFIG_SYNTH_CUSTOM_WT_TIMING
        ESP_LOGI(TAG, "frames %u..%u: %lld us", (unsigned)first,
                 (unsigned)(s_next_frame - 1u), (long long)(esp_timer_get_time() - t0));
#endif
        return;
    }

    /* Finish in place in the scratch, then the one copy the render task may
     * see half of (the torn block in wt_builder.h). */
    wt_synth_finish(s_scratch, (int16_t *)s_scratch, &s_preview);
    memcpy(s_live, s_scratch, WT_TABLE_SAMPLES * sizeof(int16_t));
    s_generation++;
    s_next_frame = 0;
#if CONFIG_SYNTH_CUSTOM_WT_TIMING
    ESP_LOGI(TAG, "finish + copy: %lld us", (long long)(esp_timer_get_time() - t0));
#endif
}

uint16_t wt_builder_preset(void)
{
    return s_ready ? (uint16_t)WT_BUILDER_PRESET : (uint16_t)WT_BUILDER_PRESET_NONE;
}

uint32_t wt_builder_generation(void)
{
    return s_generation;
}

void wt_builder_preview(wt_preview_t *out)
{
    *out = s_preview;
}
