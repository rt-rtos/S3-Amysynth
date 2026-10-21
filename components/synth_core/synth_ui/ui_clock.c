#include "synth_ui/synth_ui_internal.h"
#include "amy.h"               /* AMY_SEQUENCER_PPQ behind SEQ_TICKS_PER_STEP */
#include "seq_core_config.h"   /* SEQ_TICKS_PER_BAR, SEQ_TICKS_PER_STEP */
#include <stdio.h>

static const uint8_t s_phrase_bars[UI_CLOCK_PHRASE_COUNT] = { 0, 2, 4, 8, 16, 32 };

static uint8_t s_phrase_idx;

uint8_t ui_clock_defaults(void) { return 0; }

uint8_t ui_clock_phrase_bars(uint8_t idx)
{
    return s_phrase_bars[(idx < UI_CLOCK_PHRASE_COUNT) ? idx : UI_CLOCK_PHRASE_COUNT - 1];
}

uint8_t ui_clock_get_phrase_idx(void) { return s_phrase_idx; }

void ui_clock_set_phrase_idx(uint8_t idx)
{
    s_phrase_idx = (idx < UI_CLOCK_PHRASE_COUNT) ? idx : (uint8_t)(UI_CLOCK_PHRASE_COUNT - 1);
}

void ui_clock_text(char *out, size_t n, int32_t e, uint8_t phrase_idx)
{
    const int32_t bar_t  = (int32_t)SEQ_TICKS_PER_BAR;
    const int32_t step_t = (int32_t)SEQ_TICKS_PER_STEP;
    if (e < 0) {
        if (e < -bar_t) e = -bar_t;
        snprintf(out, n, "0.%u", (unsigned)((e + bar_t) / step_t + 1));
        return;
    }
    unsigned bar = (unsigned)(e / bar_t);
    unsigned s   = (unsigned)((e % bar_t) / step_t + 1);
    unsigned p   = ui_clock_phrase_bars(phrase_idx);
    if (p == 0)
        snprintf(out, n, "%u.%u", bar + 1, s);
    else
        snprintf(out, n, "%u:%u.%u", bar / p + 1, bar % p + 1, s);
}
