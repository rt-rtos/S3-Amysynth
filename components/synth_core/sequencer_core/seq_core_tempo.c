#include "sequencer_core/seq_core_internal.h"
#include "custompatches/clip_player.h"
#include "voice_config.h"                   /* SEQ_LFO_PITCH_BASE_HZ */
#include "custompatches/drone_std_core.h"   /* drone_std_core_refresh_lfo_freq */
#include "seq_clamp.h"
#include "amy_fx.h"                         /* amy_fx_on_tempo_change */
#include "sdkconfig.h"
#if CONFIG_SYNTH_WIRELESS
#include "live_play.h"                      /* live_play_refresh_lfo_freq */
#endif

/* ── State definitions — owns BPM and quantizer ─────────────────────── */
uint16_t s_bpm = SEQ_DEFAULT_BPM;
quantizer_state_t s_quantizer = {
    .root_note  = CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE,
    .scale_index = CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE,
    .enabled    = CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED,
};

/* ── BPM helpers ─────────────────────────────────────────────────────── */

uint16_t sequencer_clamp_bpm(uint16_t b)
{
    return SEQ_CLAMP_U16(b, SEQ_MIN_BPM, SEQ_MAX_BPM);
}

void sequencer_push_tempo(uint16_t b)
{
    amy_event *e = amy_helpers_event_begin();
    e->tempo = b;
    amy_helpers_event_send(e);
}

/* ── LFO helpers ─────────────────────────────────────────────────────── */

float lfo_rate_to_hz(note_div_t rate, uint16_t bpm)
{
    /* Sub-audible ceiling: at high BPM the fastest divisions cross into
     * audio-rate AM. Cap rather than hide rates from the pickers. */
    return fminf(note_div_hz(rate, (float)bpm), SEQ_LFO_NATIVE_MAX_HZ);
}

float lfo_next_rand(void)
{
    s_lfo_rng_state ^= s_lfo_rng_state << 13;
    s_lfo_rng_state ^= s_lfo_rng_state >> 17;
    s_lfo_rng_state ^= s_lfo_rng_state << 5;
    return (float)(s_lfo_rng_state >> 17) / 32767.0f * 2.0f - 1.0f;
}

void lfo_push_amp(uint8_t synth_id, uint16_t patch, float amp)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth = synth_id;
    int osc = sequencer_core_patch_amp_osc(patch);
    if (osc >= 0) e->osc = (uint16_t)osc;
    e->amp_coefs[COEF_CONST] = amp;
    amy_helpers_event_send(e);
}

void lfo_push_target_neutral(uint8_t synth_id, uint16_t patch,
                             lfo_target_t target)
{
    if (target == LFO_TARGET_AMP) {
        lfo_push_amp(synth_id, patch, 1.0f);
        return;
    }
    amy_event *e = amy_helpers_event_begin();
    e->synth = synth_id;
    switch (target) {
        /* Absolute Hz: SEQ_LFO_PITCH_BASE_HZ is the note-neutral reset
         * default. Osc 0 only, mirroring the service push. */
        case LFO_TARGET_PITCH:  e->osc = 0;
                                e->freq_coefs[COEF_CONST] = SEQ_LFO_PITCH_BASE_HZ; break;
        case LFO_TARGET_PAN:    e->pan_coefs[COEF_CONST]  = 0.5f;           break;
        /* FILTER and DIST have no context-free neutral - the resting state
         * is the caller's committed seq_filter_t / seq_dist_t, so every
         * restore path pushes those directly. */
        case LFO_TARGET_FILTER:
        case LFO_TARGET_DIST_DRIVE:
        case LFO_TARGET_DIST_MIX:
        default: break;
    }
    amy_helpers_event_send(e);
}

/* ── Public API — BPM ────────────────────────────────────────────────── */

void sequencer_core_set_bpm(uint16_t new_bpm)
{
    s_bpm = sequencer_clamp_bpm(new_bpm);
    sequencer_push_tempo(s_bpm);
    for (int li = 0; li < s_num_layers; li++) {
        for (int tr = 0; tr < SEQ_TRACKS; tr++) {
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
            /* Native-LFO rows must keep s_lfo_hz == 0: their carrier is retuned
             * by melodic_lfo_refresh_native_freq() below, and a nonzero value
             * would double-modulate. Per row, since a melodic layer can mix
             * wave and string patches. Only melodic rows can be native - a
             * drum row's stored patch is just its SYNTH-mode selection. */
            if (s_layers[li].type == SEQ_LAYER_MELODIC &&
                sequencer_core_lfo_native_layout(s_layers[li].track_patch[tr],
                                                 NULL, NULL)) continue;
#endif
            const voice_params_t *vp = seq_track_vp((uint8_t)li, (uint8_t)tr);
            if (vp->lfo_authored && vp->lfo.enabled)
                s_lfo_hz[li][tr] = seq_lfo_sw_hz(vp->lfo.rate, s_bpm);
        }
    }
    /* Sync every native LFO carrier to the new BPM (each a no-op when the
     * owner has none active). */
    arp_core_refresh_lfo_freq();
    drone_std_core_refresh_lfo_freq();
    melodic_lfo_refresh_native_freq();
#if CONFIG_SYNTH_WIRELESS
    live_play_refresh_lfo_freq();
#endif
    clip_player_on_tempo_change(s_bpm);
    amy_fx_on_tempo_change();
}

uint16_t sequencer_core_get_bpm(void) { return s_bpm; }

/* ── Public API — quantizer ──────────────────────────────────────────── */

/* An arp in GLOBAL quant mode snaps against s_quantizer at emit time, and so
 * does a CHORD-mode arp while no progression chord is applied, so any change
 * here must re-emit its schedule via the same coalesced dirty-mark the arp's
 * own setters use. No-op when the arp uses its own scale. */
static void quantizer_changed_refresh_arp(void)
{
    if (arp_get_quant_mode() != ARP_QUANT_OWN) arp_core_mark_dirty();
}

void sequencer_core_set_quantizer_enabled(bool enabled)
{
    if (s_quantizer.enabled == enabled) return;
    s_quantizer.enabled = enabled;
    sequencer_refresh_melodic_layers(false);
    quantizer_changed_refresh_arp();
    ESP_LOGI(TAG, "quantizer %s", enabled ? "enabled" : "disabled");
}

void sequencer_core_set_quantizer_root_note(uint8_t root_note)
{
    s_quantizer.root_note = root_note;
    sequencer_refresh_melodic_layers(false);
    quantizer_changed_refresh_arp();
    ESP_LOGI(TAG, "quantizer root -> %u", root_note);
}

void sequencer_core_set_quantizer_scale(uint8_t scale_index)
{
    s_quantizer.scale_index = (scale_index >= quantizer_scale_count()) ? 0 : scale_index;
    sequencer_refresh_melodic_layers(false);
    quantizer_changed_refresh_arp();
    ESP_LOGI(TAG, "quantizer scale -> %u", s_quantizer.scale_index);
}

bool sequencer_core_get_quantizer_enabled(void)
{
    return s_quantizer.enabled;
}

uint8_t sequencer_core_get_quantizer_root_note(void)
{
    return s_quantizer.root_note;
}

uint8_t sequencer_core_get_quantizer_scale(void)
{
    return s_quantizer.scale_index;
}
