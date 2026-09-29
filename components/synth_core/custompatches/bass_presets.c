/* bass_presets.c - three two-oscillator bass presets (SEQ_PATCH_BASS_1..3),
 * dispatched from sequencer_configure_synth(). UI task, via amy_helpers.
 *
 * BASS_1/BASS_2 sweep the filter with EG1, separately from the EG0 amp
 * envelope: a plucky amp decay over a slower filter tail. The EG1
 * breakpoints go in the same event as the coef reading them
 * (seq_filter_eg1_live()). BASS_3 has no filter. */

#include "custompatches/bass_presets.h"
#include "sequencer_core.h"    /* SEQ_PATCH_BASS_* */
#include "amy.h"               /* PULSE, SAW_DOWN, SINE, FILTER_LPF24, COEF_*, ENVELOPE_* */
#include "amy_helpers.h"       /* amy_helpers_event_begin/send */
#include "voice_config.h"      /* voice_park_oscs (reserved LFO pair) */

void bass_preset_configure_track(uint8_t synth_id, uint16_t patch,
                                 uint16_t num_voices)
{
    /* 2 audible oscs per voice plus a reserved native-LFO carrier pair at
     * osc2/osc3 (sequencer_core_lfo_native_layout), parked until an LFO is
     * authored: notes reach every osc of the voice (voice_park_oscs). */
    amy_event *e = amy_helpers_event_begin();
    e->synth          = synth_id;
    e->num_voices     = num_voices;
    e->oscs_per_voice = 4;
    amy_helpers_event_send(e);
    voice_park_oscs(synth_id, 2, 4);

    if (patch == SEQ_PATCH_BASS_1) {
        /* ─── Preset 264: Classic Sub-Heavy Detune Bass ─────────────────
         * PULSE carrier + SAW_DOWN detuned ~8.6 cents for thickness,
         * LPF24 on the carrier swept by EG1 (120 Hz -> ~2000 Hz at peak),
         * settling slower than the EG0 amp decay. */

        /* osc 0: PULSE carrier with envelope-swept low-pass filter */
        e = amy_helpers_event_begin();
        e->synth                          = synth_id;
        e->osc                            = 0;
        e->wave                           = PULSE;
        e->freq_coefs[COEF_NOTE]          = 1.0f;
        e->amp_coefs[COEF_CONST]          = 1.0f;
        e->amp_coefs[COEF_VEL]            = 1.0f;
        e->amp_coefs[COEF_EG0]            = 1.0f;
        e->filter_type                    = FILTER_LPF24;
        e->resonance                      = 0.5f;
        e->filter_freq_coefs[COEF_CONST]  = 120.0f;   /* start low per spec */
        e->filter_freq_coefs[COEF_EG1]    = 4.06f;    /* EG1 sweeps 120→~2000 Hz at peak */
        e->chained_osc                    = 1;         /* voice-local osc 1 */
        e->eg_type[0]                     = ENVELOPE_NORMAL;
        e->eg0_times[0]  = 5;    e->eg0_values[0] = 1.0f;   /* atk  5ms */
        e->eg0_times[1]  = 250;  e->eg0_values[1] = 0.4f;   /* dec 250ms, sus 40% */
        e->eg0_times[2]  = 150;  e->eg0_values[2] = 0.0f;   /* rel 150ms */
        e->eg_type[1]                     = ENVELOPE_NORMAL;
        e->eg1_times[0]  = 5;    e->eg1_values[0] = 1.0f;   /* atk  5ms (snap open) */
        e->eg1_times[1]  = 450;  e->eg1_values[1] = 0.15f;  /* dec 450ms, settles at 15% */
        e->eg1_times[2]  = 200;  e->eg1_values[2] = 0.0f;   /* rel 200ms, closes fully */
        amy_helpers_event_send(e);

        /* osc 1: SAW_DOWN, detuned +0.5% (~8.6 cents), tracking the note */
        e = amy_helpers_event_begin();
        e->synth                  = synth_id;
        e->osc                    = 1;
        e->wave                   = SAW_DOWN;
        e->freq_coefs[COEF_NOTE]  = 1.0f;
        e->freq_coefs[COEF_CONST] = ZERO_LOGFREQ_IN_HZ * 1.005f;
        e->amp_coefs[COEF_CONST] = 0.8f;    /* slightly quieter than carrier */
        e->amp_coefs[COEF_VEL]   = 1.0f;
        e->amp_coefs[COEF_EG0]   = 1.0f;
        e->eg_type[0]            = ENVELOPE_NORMAL;
        e->eg0_times[0]  = 5;    e->eg0_values[0] = 1.0f;
        e->eg0_times[1]  = 250;  e->eg0_values[1] = 0.4f;
        e->eg0_times[2]  = 150;  e->eg0_values[2] = 0.0f;
        amy_helpers_event_send(e);

    } else if (patch == SEQ_PATCH_BASS_2) {
        /* ─── Preset 265: Acid Pluck Bass ───────────────────────────────
         * SINE fundamental + SAW_DOWN through a swept LPF24 on osc1, which
         * runs on the accumulated osc0+osc1 buffer. Amp (EG0) and filter (EG1)
         * are independent: 80 Hz at rest, ~1800 Hz at attack peak, settling
         * slower/lower than the amp decay; resonance accentuates the crossover
         * for the TB-303 squelch. */

        /* osc 0: SINE fundamental */
        e = amy_helpers_event_begin();
        e->synth                 = synth_id;
        e->osc                   = 0;
        e->wave                  = SINE;
        e->freq_coefs[COEF_NOTE] = 1.0f;
        e->amp_coefs[COEF_CONST] = 1.0f;
        e->amp_coefs[COEF_VEL]   = 1.0f;
        e->amp_coefs[COEF_EG0]   = 1.0f;
        e->chained_osc           = 1;         /* voice-local osc 1 */
        e->eg_type[0]            = ENVELOPE_NORMAL;
        e->eg0_times[0]  = 10;   e->eg0_values[0] = 1.0f;   /* atk 10ms */
        e->eg0_times[1]  = 180;  e->eg0_values[1] = 0.5f;   /* dec 180ms, sus 50% */
        e->eg0_times[2]  = 100;  e->eg0_values[2] = 0.0f;   /* rel 100ms */
        amy_helpers_event_send(e);

        /* osc 1: SAW_DOWN with envelope-swept LPF24 (acid squelch); the filter
         * sees the accumulated osc0+osc1 signal, so both oscs pass through. */
        e = amy_helpers_event_begin();
        e->synth                          = synth_id;
        e->osc                            = 1;
        e->wave                           = SAW_DOWN;
        e->freq_coefs[COEF_NOTE]          = 1.0f;
        e->amp_coefs[COEF_CONST]          = 0.7f;
        e->amp_coefs[COEF_VEL]            = 1.0f;
        e->amp_coefs[COEF_EG0]            = 1.0f;
        e->filter_type                    = FILTER_LPF24;
        e->resonance                      = 1.5f;             /* squelch resonance */
        e->filter_freq_coefs[COEF_CONST]  = 80.0f;            /* rest cutoff: 80 Hz */
        e->filter_freq_coefs[COEF_EG1]    = 4.5f;             /* EG1 sweeps 80→~1800 Hz at peak */
        e->eg_type[0]                     = ENVELOPE_NORMAL;
        e->eg0_times[0]  = 5;    e->eg0_values[0] = 1.0f;   /* atk  5ms (snappy) */
        e->eg0_times[1]  = 120;  e->eg0_values[1] = 0.2f;   /* dec 120ms, sus 20% */
        e->eg0_times[2]  = 100;  e->eg0_values[2] = 0.0f;   /* rel 100ms */
        e->eg_type[1]                     = ENVELOPE_NORMAL;
        e->eg1_times[0]  = 5;    e->eg1_values[0] = 1.0f;   /* atk  5ms (snappy open) */
        e->eg1_times[1]  = 350;  e->eg1_values[1] = 0.1f;   /* dec 350ms, settles at 10% */
        e->eg1_times[2]  = 180;  e->eg1_values[2] = 0.0f;   /* rel 180ms, closes fully */
        amy_helpers_event_send(e);

    } else if (patch == SEQ_PATCH_BASS_3) {
        /* ─── Preset 266: Bright Synth Bass ─────────────────────────────
         * PULSE carrier cuts through the mix; SAW_DOWN sub-octave
         * reinforces the low end. Percussive envelope: fast attack, moderate
         * decay to a sustained body. */

        /* osc 0: PULSE carrier */
        e = amy_helpers_event_begin();
        e->synth                 = synth_id;
        e->osc                   = 0;
        e->wave                  = PULSE;
        e->freq_coefs[COEF_NOTE] = 1.0f;
        e->amp_coefs[COEF_CONST] = 1.0f;
        e->amp_coefs[COEF_VEL]   = 1.0f;
        e->amp_coefs[COEF_EG0]   = 1.0f;
        e->chained_osc           = 1;         /* voice-local osc 1 */
        e->eg_type[0]            = ENVELOPE_NORMAL;
        e->eg0_times[0]  = 5;    e->eg0_values[0] = 1.0f;   /* atk  5ms */
        e->eg0_times[1]  = 200;  e->eg0_values[1] = 0.7f;   /* dec 200ms, sus 70% */
        e->eg0_times[2]  = 150;  e->eg0_values[2] = 0.0f;   /* rel 150ms */
        amy_helpers_event_send(e);

        /* osc 1: SAW_DOWN one octave down. Faster decay than the carrier so
         * the sub fades to a click-free sustain. */
        e = amy_helpers_event_begin();
        e->synth                 = synth_id;
        e->osc                   = 1;
        e->wave                  = SAW_DOWN;
        e->freq_coefs[COEF_NOTE]  = 1.0f;
        e->freq_coefs[COEF_CONST] = ZERO_LOGFREQ_IN_HZ * 0.5f;   /* one octave below carrier */
        e->amp_coefs[COEF_CONST]  = 0.7f;
        e->amp_coefs[COEF_VEL]   = 1.0f;
        e->amp_coefs[COEF_EG0]   = 1.0f;
        e->eg_type[0]            = ENVELOPE_NORMAL;
        e->eg0_times[0]  = 2;    e->eg0_values[0] = 1.0f;   /* atk  2ms */
        e->eg0_times[1]  = 100;  e->eg0_values[1] = 0.5f;   /* dec 100ms, sus 50% */
        e->eg0_times[2]  = 120;  e->eg0_values[2] = 0.0f;   /* rel 120ms */
        amy_helpers_event_send(e);
    }
}
