#include "synth_level.h"
#include "synth_slots.h"
#include "amy_helpers.h"

/* Owner of amy_event.synth_level for app synths: contract in synth_level.h. */

static float s_lfo_cap[SYNTH_SLOT_COUNT];
static float s_mix[SYNTH_SLOT_COUNT];

static void send_level(uint8_t synth)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth       = synth;
    e->synth_level = s_lfo_cap[synth] * s_mix[synth];
    amy_helpers_event_send(e);
}

void synth_level_init(void)
{
    for (uint16_t s = 0; s < SYNTH_SLOT_COUNT; s++) {
        s_lfo_cap[s] = 1.0f;
        s_mix[s]     = 1.0f;
    }
}

void synth_level_set_lfo_cap(uint8_t synth, float cap)
{
    if (synth >= SYNTH_SLOT_COUNT) return;
    s_lfo_cap[synth] = cap;
    send_level(synth);
}

void synth_level_set_mix(uint8_t synth, float gain)
{
    if (synth >= SYNTH_SLOT_COUNT) return;
    s_mix[synth] = gain;
    send_level(synth);
}

float synth_level_get(uint8_t synth)
{
    if (synth >= SYNTH_SLOT_COUNT) return 1.0f;
    return s_lfo_cap[synth] * s_mix[synth];
}

void synth_level_reassert(uint8_t synth)
{
    if (synth >= SYNTH_SLOT_COUNT) return;
    send_level(synth);
}
