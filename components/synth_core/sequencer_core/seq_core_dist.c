#include "sequencer_core/seq_core_internal.h"
#include "voice_config.h"

/* Push one dist block where the row's layout carries the stage, then, on an
 * owning row, its routing depths (filter_push_eg_depths). */
void melodic_dist_apply(uint8_t layer_idx, uint8_t track,
                        const seq_dist_t *d)
{
    uint8_t synth = s_layers[layer_idx].synth_id[track];
    uint8_t heads = melodic_heads_mask(layer_idx, track);
    bool drum = (s_layers[layer_idx].type == SEQ_LAYER_DRUM);
    bool own = drum || seq_track_vp(layer_idx, track)->filter_authored;
    if (!heads) {
        voice_apply_dist(synth, d);
        if (own) sequencer_core_push_eg_depths(synth,
                                               sequencer_core_patch_voice_osc(
                                                   s_layers[layer_idx].track_patch[track]),
                                               &seq_track_vp(layer_idx, track)->filter,
                                               true);
        return;
    }
    for (uint8_t o = 0; (uint8_t)(heads >> o) != 0u; o++)
        if (heads & (uint8_t)(1u << o)) {
            voice_apply_dist_osc(synth, o, d);
            if (own) sequencer_core_push_eg_depths(synth, (int)o,
                                                   &seq_track_vp(layer_idx, track)->filter,
                                                   true);
        }
}

/* ── Per-row melodic distortion ──────────────────────────────────────────
 * Parallel to the filter, minus the service loop: distortion has no software
 * fallback path, so a push goes straight to the row's synth and there is
 * nothing for lfo_service to arbitrate. Preview and commit therefore differ
 * only in whether the store is written. */

bool sequencer_core_get_melodic_dist(uint8_t layer_idx, uint8_t track,
                                     seq_dist_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = seq_track_vp(layer_idx, track)->dist;
    return true;
}

void sequencer_core_set_melodic_dist(uint8_t layer_idx, uint8_t track,
                                     const seq_dist_t *d)
{
    if (!d || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    voice_params_t *vp = seq_track_vp(layer_idx, track);

    vp->dist = *d;
    voice_dist_clamp(&vp->dist);
    vp->dist_authored = true;
    melodic_push_peers(layer_idx, track, sequencer_configure_melodic_dist_track);
    ESP_LOGI(TAG, "dist L%u T%u [%s] -> type%u drv%u bit%u rte%u mix%u (authored)",
             layer_idx + 1u, track + 1u, melodic_vp_where(layer_idx, track),
             vp->dist.type, vp->dist.drive, vp->dist.bits, vp->dist.rate,
             vp->dist.mix);
}

void sequencer_core_preview_melodic_dist(uint8_t layer_idx, uint8_t track,
                                         const seq_dist_t *d)
{
    if (!d || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    melodic_dist_apply(layer_idx, track, d);
}

void sequencer_core_reapply_melodic_dist(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    melodic_dist_apply(layer_idx, track, &seq_track_vp(layer_idx, track)->dist);
}

/* Re-assert a row's stored distortion after anything that rebuilds its voice
 * (patch load, layer reload). Without this the stage is silently dropped, the
 * same failure the filter/LFO re-pushes exist for. */
void sequencer_configure_melodic_dist_track(uint8_t layer_idx, uint8_t track)
{
    sequencer_core_reapply_melodic_dist(layer_idx, track);
}
