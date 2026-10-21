#include "synth_level.h"
#include "synth_slots.h"
#include "amy_helpers.h"

/* Owner of amy_event.synth_level for app synths: contract in synth_level.h. */

static float s_lfo_cap[SYNTH_SLOT_COUNT][2];   /* per mod slot */
static float s_mix[SYNTH_SLOT_COUNT];
static float s_sw_lfo[SYNTH_SLOT_COUNT];        /* software-stepper AMP dip */
static float s_transport = 1.0f;               /* every synth but LIVE_SYNTH */

static float level_of(uint8_t synth)
{
    float l = s_lfo_cap[synth][0] * s_lfo_cap[synth][1] * s_mix[synth]
            * s_sw_lfo[synth];
    return (synth == LIVE_SYNTH) ? l : l * s_transport;
}

static void send_level(uint8_t synth)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth       = synth;
    e->synth_level = level_of(synth);
    amy_helpers_event_send(e);
}

void synth_level_init(void)
{
    for (uint16_t s = 0; s < SYNTH_SLOT_COUNT; s++) {
        s_lfo_cap[s][0] = 1.0f;
        s_lfo_cap[s][1] = 1.0f;
        s_mix[s]     = 1.0f;
        s_sw_lfo[s]  = 1.0f;
    }
    s_transport = 1.0f;
}

void synth_level_set_lfo_cap(uint8_t synth, uint8_t slot, float cap)
{
    if (synth >= SYNTH_SLOT_COUNT || slot > 1u) return;
    s_lfo_cap[synth][slot] = cap;
    send_level(synth);
}

void synth_level_set_mix(uint8_t synth, float gain)
{
    if (synth >= SYNTH_SLOT_COUNT) return;
    s_mix[synth] = gain;
    send_level(synth);
}

void synth_level_set_sw_lfo(uint8_t synth, float g)
{
    if (synth >= SYNTH_SLOT_COUNT) return;
    s_sw_lfo[synth] = g;
    send_level(synth);
}

void synth_level_set_transport(float g)
{
    s_transport = g;
}

float synth_level_get(uint8_t synth)
{
    if (synth >= SYNTH_SLOT_COUNT) return 1.0f;
    return level_of(synth);
}

void synth_level_reassert(uint8_t synth)
{
    if (synth >= SYNTH_SLOT_COUNT) return;
    send_level(synth);
}
