#include "custompatches/additive_voice.h"
#include "amy.h"           /* BYO_PARTIALS, PARTIAL, COEF_*, ENVELOPE_* */
#include "amy_helpers.h"   /* amy_helpers_event_begin/send */
#include "seq_clamp.h"
#include "voice_config.h"  /* SEQ_LFO_PITCH_BASE_HZ */

/* The live-editable "custom" additive voice; ownership convention in
 * additive_voice.h (mirrors s_fm_voice / amy_fx.h's s_fx). */
additive_voice_t s_additive_voice;

/* At or below this floor a ratio plays as 1. */
#define ADD_MIN_RATIO 0.01f

/* freq CONST for a partial at `ratio` above the note. The event field is Hz,
 * anchored so SEQ_LFO_PITCH_BASE_HZ is the note-neutral value (voice_config.h),
 * which puts the partial log2(ratio) octaves above the parent. */
static float add_ratio_hz(float ratio)
{
    if (!(ratio > ADD_MIN_RATIO)) return SEQ_LFO_PITCH_BASE_HZ;   /* also catches NaN */
    return SEQ_LFO_PITCH_BASE_HZ * ratio;
}

void additive_voice_default(additive_voice_t *v)
{
    if (!v) return;
    v->num_partials = 8;
    for (uint8_t i = 0; i < 8; i++) {
        v->ratio[i]    = (float)(i + 1);         /* harmonics 1..8            */
        v->level[i]    = 1.0f / (float)(i + 1);  /* 1, 1/2, 1/3 … saw-ish     */
        v->decay_ms[i] = 0.0f;                   /* all follow the parent ADSR */
    }
    for (uint8_t i = 8; i < ADD_MAX_PARTIALS; i++) {
        v->ratio[i] = 1.0f; v->level[i] = 0.0f; v->decay_ms[i] = 0.0f;
    }
}

void additive_voice_configure_track(uint8_t synth_id, uint16_t num_voices,
                                    const additive_voice_t *voice)
{
    if (!voice) return;
    uint8_t n = voice->num_partials;
    n = SEQ_CLAMP_U8(n, 1, ADD_MAX_PARTIALS);

    /* 1) Allocate the voice: 1 control osc + N partial oscs. */
    amy_event *e = amy_helpers_event_begin();
    e->synth          = synth_id;
    e->num_voices     = num_voices;
    e->oscs_per_voice = (uint8_t)(n + 1);
    amy_helpers_event_send(e);

    /* 2) osc 0: BYO_PARTIALS control osc. preset = N drives the child count
     *    spawned by partials_note_on(). Carries the shared amp envelope +
     *    velocity + overall pitch; produces no sound of its own. */
    e = amy_helpers_event_begin();
    e->synth                 = synth_id;
    e->osc                   = 0;
    e->wave                  = BYO_PARTIALS;
    e->preset                = n;
    e->freq_coefs[COEF_NOTE] = 1.0f;      /* whole voice tracks the note       */
    e->amp_coefs[COEF_CONST] = 1.0f;
    e->amp_coefs[COEF_VEL]   = 1.0f;      /* propagated to each partial as vel */
    e->amp_coefs[COEF_EG0]   = 1.0f;      /* row ADSR overwrites this later    */
    e->eg_type[0]            = ENVELOPE_NORMAL;
    e->eg0_times[0]  = 5;    e->eg0_values[0] = 1.0f;
    e->eg0_times[1]  = 400;  e->eg0_values[1] = 0.7f;
    e->eg0_times[2]  = 250;  e->eg0_values[2] = 0.0f;
    amy_helpers_event_send(e);

    /* 3) oscs 1..N: one PARTIAL sine per harmonic. partials_note_on() sets
     *    each child's status/pitch base but NOT its wave - PARTIAL selects the
     *    render_partial path and hold_and_modify's fade-in special case.
     *    Pitch offset is log2(ratio); amp is the spectrum level, scaled per
     *    block by the parent's envelope through COEF_VEL. */
    for (uint8_t i = 0; i < n; i++) {
        e = amy_helpers_event_begin();
        e->synth                  = synth_id;
        e->osc                    = (uint16_t)(i + 1);
        e->wave                   = PARTIAL;
        e->freq_coefs[COEF_NOTE]  = 1.0f;
        e->freq_coefs[COEF_CONST] = add_ratio_hz(voice->ratio[i]);
        e->amp_coefs[COEF_CONST]  = voice->level[i];
        e->amp_coefs[COEF_VEL]    = 1.0f;   /* receives parent amp each block */
        if (voice->decay_ms[i] > 0.0f) {
            /* Local ring-down (fast attack, decay to silence, no sustain) so
             * upper partials die before the parent envelope releases. */
            e->amp_coefs[COEF_EG0] = 1.0f;
            e->eg_type[0]          = ENVELOPE_NORMAL;
            e->eg0_times[0] = 3;   e->eg0_values[0] = 1.0f;
            e->eg0_times[1] = (uint32_t)voice->decay_ms[i];
            e->eg0_values[1] = 0.0f;
        }
        amy_helpers_event_send(e);
    }
}

void additive_voice_push_live(uint8_t synth_id, const additive_voice_t *voice)
{
    if (!voice) return;
    uint8_t n = voice->num_partials;
    n = SEQ_CLAMP_U8(n, 1, ADD_MAX_PARTIALS);

    for (uint8_t i = 0; i < n; i++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth                  = synth_id;
        e->osc                    = (uint16_t)(i + 1);
        e->freq_coefs[COEF_CONST] = add_ratio_hz(voice->ratio[i]);
        e->amp_coefs[COEF_CONST]  = voice->level[i];
        amy_helpers_event_send(e);
    }
}
