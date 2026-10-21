#include "sequencer_core/seq_core_internal.h"
#include "voice_config.h"
#include "seq_clamp.h"

/* Push one EG1 breakpoint set to every osc that can carry a filter-env rail
 * (sequencer_core_push_envelope_eg1() says why each needs it).
 * voice_mask is where the per-voice stages live: the cluster oscs on a unison
 * row, osc 0 on a plain wave row. */
void melodic_eg1_push(uint8_t layer_idx, uint8_t track,
                      const seq_env_t *env)
{
    uint8_t synth = s_layers[layer_idx].synth_id[track];
    seq_voice_layout_t vl;
    if (!seq_track_voice_layout(layer_idx, track, &vl)) {
        sequencer_core_push_envelope_eg1(synth, 0, env);
        return;
    }
    for (uint8_t o = 0; (uint8_t)(vl.voice_mask >> o) != 0u; o++)
        if (vl.voice_mask & (uint8_t)(1u << o))
            sequencer_core_push_envelope_eg1(synth, o, env);
}

/* ── Per-row melodic envelope (runtime-editable) ─────────────────────────── */

/* Push the given row's stored envelope to that row's OWN AMY synth. */
void sequencer_configure_melodic_envelope_track(uint8_t layer_idx, uint8_t track)
{
#if CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED
    const seq_layer_t *layer = &s_layers[layer_idx];
    /* No KS/NOISE special-casing: the row's envelope applies verbatim to every
     * wave. A forced onset floor or zeroed KS sustain makes those patches decay
     * to silence regardless of what the user authored. */
    seq_env_t env = *seq_layer_env(layer_idx, track);
    float sustain = (float)env.sustain_pct / 100.0f;

    amy_event *e = amy_helpers_event_begin();
    e->synth = layer->synth_id[track];
    int osc = sequencer_core_patch_voice_osc(layer->track_patch[track]);
    if (osc >= 0) e->osc = (uint16_t)osc;
    e->bp_is_set[0] = 1;
    e->eg_type[0] = env.eg_type;
    e->eg0_times[0] = SEQ_CLAMP_U32(env.attack_ms,
                                    VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    e->eg0_values[0] = 1.0f;
    e->eg0_times[1] = env.decay_ms;
    e->eg0_values[1] = sustain;
    e->eg0_times[2] = SEQ_CLAMP_U32(env.release_ms,
                                    VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    e->eg0_values[2] = 0.0f;
    amy_helpers_event_send(e);
#else
    (void)layer_idx; (void)track;
#endif
}

bool sequencer_core_get_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                         seq_env_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = *seq_layer_env(layer_idx, track);
    return true;
}

void sequencer_core_set_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                         const seq_env_t *env)
{
    if (!env || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    voice_params_t *vp = seq_track_vp(layer_idx, track);

    seq_env_t *dst = &vp->env;
    dst->attack_ms   = SEQ_CLAMP_U32(env->attack_ms,
                                     VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    dst->decay_ms    = SEQ_CLAMP_U32(env->decay_ms,   0, VOICE_ENV_TIME_MAX_MS);
    dst->sustain_pct = SEQ_CLAMP_U8(env->sustain_pct, 0, VOICE_ENV_SUSTAIN_MAX_PCT);
    dst->release_ms  = SEQ_CLAMP_U32(env->release_ms,
                                     VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    dst->eg_type     = env->eg_type;

    /* Committing establishes the block's authority over the patch's own
     * envelope: later patch changes re-impose it. The push reaches every row
     * reading this block (just this row for an own-block row). */
    vp->env_authored = true;
    melodic_push_peers(layer_idx, track, sequencer_configure_melodic_envelope_track);
    ESP_LOGI(TAG, "env L%u T%u [%s] -> A%u D%u S%u%% R%u (authored)",
             layer_idx + 1u, track + 1u, melodic_vp_where(layer_idx, track),
             (unsigned)dst->attack_ms, (unsigned)dst->decay_ms,
             (unsigned)dst->sustain_pct, (unsigned)dst->release_ms);
#if CONFIG_SEQ_ENV_DEBUG_DUMP
    ESP_LOGW(TAG, "ENVDUMP sent to synth %u: eg_type=%u bp0=[%ums,1.0] "
                  "bp1=[%ums,%.3f] bp2=[%ums,0.0]",
             (unsigned)s_layers[layer_idx].synth_id[track], (unsigned)dst->eg_type,
             (unsigned)dst->attack_ms,
             (unsigned)dst->decay_ms, (double)dst->sustain_pct / 100.0,
             (unsigned)dst->release_ms);
#endif
}

/* ── Per-row second envelope (runtime-editable EG1) ──────────────────────── */

/* Push the row's stored EG1 to every osc of its synth that can carry a
 * filter-env rail (melodic_eg1_push resolves which). No KS/NOISE special case:
 * EG1 has no role for those waves. */
void sequencer_configure_melodic_envelope1_track(uint8_t layer_idx, uint8_t track)
{
    melodic_eg1_push(layer_idx, track, seq_layer_env1(layer_idx, track));
}

bool sequencer_core_get_melodic_envelope2(uint8_t layer_idx, uint8_t track,
                                          seq_env_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = *seq_layer_env1(layer_idx, track);
    return true;
}

void sequencer_core_set_melodic_envelope2(uint8_t layer_idx, uint8_t track,
                                          const seq_env_t *env)
{
    if (!env || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    voice_params_t *vp = seq_track_vp(layer_idx, track);

    seq_env_t *dst = &vp->env1;
    dst->attack_ms   = SEQ_CLAMP_U32(env->attack_ms,
                                     VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    dst->decay_ms    = SEQ_CLAMP_U32(env->decay_ms,   0, VOICE_ENV_TIME_MAX_MS);
    dst->sustain_pct = SEQ_CLAMP_U8(env->sustain_pct, 0, VOICE_ENV_SUSTAIN_MAX_PCT);
    dst->release_ms  = SEQ_CLAMP_U32(env->release_ms,
                                     VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    dst->eg_type     = env->eg_type;

    vp->env1_authored = true;
    melodic_push_peers(layer_idx, track, sequencer_configure_melodic_envelope1_track);
    ESP_LOGI(TAG, "env1 L%u T%u [%s] -> A%u D%u S%u%% R%u (authored)",
             layer_idx + 1u, track + 1u, melodic_vp_where(layer_idx, track),
             (unsigned)dst->attack_ms, (unsigned)dst->decay_ms,
             (unsigned)dst->sustain_pct, (unsigned)dst->release_ms);
}

void sequencer_core_preview_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                             const seq_env_t *env)
{
    if (!env || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    const seq_layer_t *layer = &s_layers[layer_idx];
    sequencer_core_push_envelope_osc(layer->synth_id[track],
                                     sequencer_core_patch_voice_osc(layer->track_patch[track]),
                                     env);
}

void sequencer_core_preview_melodic_envelope2(uint8_t layer_idx, uint8_t track,
                                              const seq_env_t *env)
{
    if (!env || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    sequencer_core_push_envelope_eg1(s_layers[layer_idx].synth_id[track], 0, env);
}

bool sequencer_core_melodic_env_authored(uint8_t layer_idx, uint8_t track,
                                         uint8_t eg_index)
{
    return sequencer_core_melodic_group_authored(
        layer_idx, track, (eg_index == 1) ? SEQ_VP_GROUP_ENV1 : SEQ_VP_GROUP_ENV);
}

/* ── Per-track amplitude trim (Layer menu Level row) ─────────────────────────
 * A per-track multiplier on note velocity at emit time. Default 1.0, which
 * add_layer must set explicitly since memset zeroes the struct.
 *
 * The setter re-emits the track's steps: steps are scheduled ahead of time with
 * a period and are not re-emitted per tick, so a store-only change would stay
 * silent until some unrelated re-emit. */

float sequencer_core_get_melodic_amp_scale(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 1.0f;
    return s_layers[layer_idx].vp[track].amp_trim;
}

void sequencer_core_set_melodic_amp_scale(uint8_t layer_idx, uint8_t track,
                                          float v)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    v = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
    s_layers[layer_idx].vp[track].amp_trim = v;
    /* Re-emit all steps so the new amplitude takes effect immediately. */
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t s = 0; s < layer->num_steps; s++)
        sequencer_emit_step(layer_idx, track, s);
}
