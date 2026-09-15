#include "custompatches/wavetable_bank.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "amy.h"
#include "sequencer_core.h"

static const char *TAG = "wt_bank";

#if CONFIG_AMY_WAVETABLE

/* Memory-preset numbers for the bank: above AMY's ROM map (24 entries incl.
 * the vendored wavetables) and below the gamma9001 drum base (256), a band
 * nothing else loads into. */
#define WT_BANK_PRESET_BASE 32u
#define WT_BANK_SAMPLE_RATE 44100u
#define WT_BANK_MIDINOTE    69.0f
#define WT_CYCLE            256u   /* WAVETABLE_SAMPLES_PER_CYCLE, oscillators.c */

/* First pass: the table definitions. */
#define WT_MANIFEST_INCLUDES
#include "wavetables/manifest.inc"
#undef WT_MANIFEST_INCLUDES

typedef struct {
    const int16_t *data;
    const char    *label;
    uint16_t       frames;
} wt_table_t;

/* Second pass: the enumeration. */
#define WT_ENTRY(sym, lbl, fr) { wt_##sym, lbl, fr },
static const wt_table_t s_tables[] = {
#include "wavetables/manifest.inc"
};
#undef WT_ENTRY
#define WT_TABLE_COUNT ((uint8_t)(sizeof(s_tables) / sizeof(s_tables[0])))

static bool s_loaded[SEQ_PATCH_WAVETABLE_APP_SLOTS];
static char s_names[SEQ_PATCH_WAVETABLE_APP_SLOTS][24];

uint8_t wavetable_bank_count(void)
{
    return WT_TABLE_COUNT < SEQ_PATCH_WAVETABLE_APP_SLOTS
         ? WT_TABLE_COUNT : SEQ_PATCH_WAVETABLE_APP_SLOTS;
}

void wavetable_bank_init(void)
{
    uint8_t n = wavetable_bank_count();
    uint32_t bytes_total = 0;
    for (uint8_t i = 0; i < n; i++) {
        const wt_table_t *t = &s_tables[i];
        uint32_t length = (uint32_t)t->frames * WT_CYCLE;
        snprintf(s_names[i], sizeof(s_names[i]), "Wavetable: %s", t->label);
        /* pcm_load() mutates pcm.c's unlocked memory-preset list, which
         * render_wavetable() walks inside the render body. The copy happens
         * outside the lock: nothing references the preset until a patch is
         * applied, and that cannot happen before this init returns. */
        amy_grab_lock();
        int16_t *buf = pcm_load((uint16_t)(WT_BANK_PRESET_BASE + i), length,
                                WT_BANK_SAMPLE_RATE, 1, WT_BANK_MIDINOTE, 0, 0);
        amy_release_lock();
        if (buf == NULL) {
            ESP_LOGW(TAG, "%s: no sample RAM for %lu KB, patch %u falls back",
                     t->label, (unsigned long)(length * sizeof(int16_t) / 1024u),
                     (unsigned)(SEQ_PATCH_WAVETABLE_APP_BASE + i));
            continue;
        }
        memcpy(buf, t->data, length * sizeof(int16_t));
        s_loaded[i] = true;
        bytes_total += length * sizeof(int16_t);
    }
    ESP_LOGI(TAG, "%u wavetable(s) loaded, %lu KB, patches %u..%u",
             (unsigned)n, (unsigned long)(bytes_total / 1024u),
             (unsigned)SEQ_PATCH_WAVETABLE_APP_BASE,
             (unsigned)(SEQ_PATCH_WAVETABLE_APP_BASE + n - 1));
}

uint16_t wavetable_bank_preset_for_patch(uint16_t patch)
{
    if (patch >= SEQ_PATCH_WAVETABLE_BASE && patch <= SEQ_PATCH_WAVETABLE_MAX) {
        return (uint16_t)(pcm_wavetable_base + (patch - SEQ_PATCH_WAVETABLE_BASE));
    }
    if (patch >= SEQ_PATCH_WAVETABLE_APP_BASE && patch <= SEQ_PATCH_WAVETABLE_APP_MAX) {
        uint16_t i = (uint16_t)(patch - SEQ_PATCH_WAVETABLE_APP_BASE);
        if (i < wavetable_bank_count() && s_loaded[i]) {
            return (uint16_t)(WT_BANK_PRESET_BASE + i);
        }
    }
    return pcm_wavetable_base;
}

const char *wavetable_bank_patch_name(uint16_t patch)
{
    if (patch < SEQ_PATCH_WAVETABLE_APP_BASE || patch > SEQ_PATCH_WAVETABLE_APP_MAX) return NULL;
    uint16_t i = (uint16_t)(patch - SEQ_PATCH_WAVETABLE_APP_BASE);
    if (i >= wavetable_bank_count()) return NULL;
    return s_names[i];
}

#else  /* !CONFIG_AMY_WAVETABLE */

uint8_t wavetable_bank_count(void) { return 0; }
void wavetable_bank_init(void) { ESP_LOGI(TAG, "AMY_WAVETABLE off, bank idle"); }
uint16_t wavetable_bank_preset_for_patch(uint16_t patch) { (void)patch; return 0; }
const char *wavetable_bank_patch_name(uint16_t patch) { (void)patch; return NULL; }

#endif
