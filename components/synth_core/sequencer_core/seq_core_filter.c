#include "sequencer_core/seq_core_internal.h"
#include "seq_clamp.h"

/* ── Per-row melodic filter (runtime-editable) ─────────────────────────── */

/* One filter event, either broadcast to the whole voice (osc < 0) or addressed
 * to one voice-relative osc - a unison voice carries the stage on each cluster
 * osc. */
static void melodic_filter_push_osc(uint8_t layer_idx, uint8_t track,
                                    const seq_filter_t *f, int osc)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    /* Ownership per filter_push_eg_depths(). A preview on a never-authored
     * row therefore pushes nonzero slots only, until the commit authors the
     * block and re-applies with the row owning its rails. */
    bool own = (layer->type == SEQ_LAYER_DRUM) ||
               seq_track_vp(layer_idx, track)->filter_authored;
    amy_event *e = amy_helpers_event_begin();
    e->synth       = layer->synth_id[track];
    if (osc >= 0) e->osc = (uint8_t)osc;
    if (f->enabled) {
        e->filter_type = f->filter_type;
        e->filter_freq_coefs[COEF_CONST] = sequencer_core_filter_const_hz(f);
        e->resonance = f->resonance;
    } else {
        e->filter_type = FILTER_NONE;
    }
    filter_push_eg_depths(e, own, f);
    /* KS string decay: the authored feedback, pushed directly. 0 = never
     * authored, so leave AMY's build-time 0.9 default in place. Pluck duty is
     * written on every KS push: its zero offset IS the 0.5 default. */
    if (layer->track_patch[track] == SEQ_PATCH_KS) {
        if (f->feedback > 0.0f) {
            e->feedback = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
        }
        e->duty_coefs[COEF_CONST] = 0.5f + f->ks_duty_ofs;
    }
    amy_helpers_event_send(e);
    if (osc < 0) dist_push_eg_depths(layer->synth_id[track], own, f);
}

/* Apply one filter config to a row's AMY synth. Shared by the stored-state
 * configure below and the live-preview push: the caller decides whether `f` is
 * the row's committed filter or an editor's scratch copy. */
void melodic_filter_apply(uint8_t layer_idx, uint8_t track,
                          const seq_filter_t *f)
{
    uint8_t heads = melodic_heads_mask(layer_idx, track);
    if (!heads) {
        melodic_filter_push_osc(layer_idx, track, f,
                                sequencer_core_patch_voice_osc(
                                    s_layers[layer_idx].track_patch[track]));
    } else {
        for (uint8_t o = 0; (uint8_t)(heads >> o) != 0u; o++)
            if (heads & (uint8_t)(1u << o))
                melodic_filter_push_osc(layer_idx, track, f, (int)o);
    }

    /* Guarantee valid EG1 breakpoints whenever any EG1 depth is live
     * (sequencer_core_push_envelope_eg1()), from the row's stored EG1
     * (authored shape or the seeded default). */
    if (seq_filter_eg1_live(f)) {
        melodic_eg1_push(layer_idx, track, seq_layer_env1(layer_idx, track));
    }

    if (sequencer_core_is_wavetable_patch(s_layers[layer_idx].track_patch[track])) {
        seq_voice_layout_t vl;
        uint8_t mask = seq_track_voice_layout(layer_idx, track, &vl) ? vl.pitch_mask
                                                                     : 0x01u;
        bool own = (s_layers[layer_idx].type == SEQ_LAYER_DRUM) ||
                   seq_track_vp(layer_idx, track)->filter_authored;
        scan_push_eg_depths(s_layers[layer_idx].synth_id[track], mask, own, f);
    }
}

void sequencer_configure_melodic_filter_track(uint8_t layer_idx, uint8_t track)
{
    melodic_filter_apply(layer_idx, track, &seq_track_vp(layer_idx, track)->filter);
}

/* A committed filter edit on a stored row. On a wavetable row the Auto frame
 * (seq_scan_rest of this filter) is baked into the row's stored AMY sequencer
 * entries at emit time, so the row re-emits as well. */
void melodic_filter_commit_track(uint8_t layer_idx, uint8_t track)
{
    sequencer_configure_melodic_filter_track(layer_idx, track);
    if (sequencer_core_is_wavetable_patch(s_layers[layer_idx].track_patch[track]))
        sequencer_emit_track(layer_idx, track);
}

bool sequencer_core_get_melodic_filter(uint8_t layer_idx, uint8_t track,
                                       seq_filter_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = seq_track_vp(layer_idx, track)->filter;
    return true;
}

void sequencer_core_set_melodic_filter(uint8_t layer_idx, uint8_t track,
                                       const seq_filter_t *f)
{
    if (!f || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    voice_params_t *vp = seq_track_vp(layer_idx, track);

    seq_filter_t *dst = &vp->filter;
    dst->filter_type = (f->filter_type < SEQ_FILTER_COUNT) ? f->filter_type : FILTER_NONE;
    dst->cutoff_hz   = SEQ_CLAMP_F32(f->cutoff_hz,  65.0f, 16000.0f);
    dst->resonance   = SEQ_CLAMP_F32(f->resonance,  0.51f, 8.0f);
    dst->enabled     = f->enabled;
    dst->feedback    = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
    dst->ks_duty_ofs = SEQ_CLAMP_F32(f->ks_duty_ofs, -0.5f, 0.5f);
    dst->key_track   = SEQ_CLAMP_F32(f->key_track, 0.0f, 1.0f);
    for (uint8_t eg = 0; eg < 2u; eg++)
        for (uint8_t t = 0; t < SEQ_EGT_COUNT; t++)
            dst->eg_depth[eg][t] = SEQ_CLAMP_F32(f->eg_depth[eg][t],
                                                 -seq_eg_depth_max(t),
                                                 seq_eg_depth_max(t));

    vp->filter_authored = true;
    melodic_push_peers(layer_idx, track, melodic_filter_commit_track);
    ESP_LOGI(TAG, "filter L%u T%u [%s] -> type%u %.0fHz Q%.2f (authored)",
             layer_idx + 1u, track + 1u, melodic_vp_where(layer_idx, track),
             dst->filter_type, (double)dst->cutoff_hz, (double)dst->resonance);
}

float sequencer_core_filter_const_hz(const seq_filter_t *f)
{
    if (f->key_track == 0.0f) return f->cutoff_hz;
    int pivot = 60 + (int)(sequencer_core_get_quantizer_root_note() % 12u);
    return f->cutoff_hz * exp2f(f->key_track * (float)(69 - pivot) / 12.0f);
}

void sequencer_core_filter_seed_default(seq_filter_t *f)
{
    if (f->cutoff_hz > 0.0f) return;
    f->filter_type = SEQ_FILTER_LPF24;
    f->cutoff_hz   = 800.0f;
    f->resonance   = 1.0f;
    f->enabled     = false;
}

/* Generic filter push: shared by arp, drone (via synth_ui). */
void sequencer_core_push_filter(uint8_t synth, const seq_filter_t *f, bool is_ks)
{
    if (!f) return;
    amy_event *e = amy_helpers_event_begin();
    e->synth = synth;
    if (f->enabled) {
        e->filter_type = f->filter_type;
        e->filter_freq_coefs[COEF_CONST] = sequencer_core_filter_const_hz(f);
        e->resonance = f->resonance;
    } else {
        e->filter_type = FILTER_NONE;
    }
    /* KS string decay from the authored feedback; 0 = never authored, so keep
     * AMY's build-time 0.9 default. Pluck duty is written on every KS push:
     * its zero offset IS the 0.5 default. */
    if (is_ks) {
        if (f->feedback > 0.0f) {
            e->feedback = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
        }
        e->duty_coefs[COEF_CONST] = 0.5f + f->ks_duty_ofs;
    }
    amy_helpers_event_send(e);
}

void sequencer_core_preview_melodic_filter(uint8_t layer_idx, uint8_t track,
                                           const seq_filter_t *f)
{
    if (!f || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    /* Same clamps as the committing setter, so the audition matches what
     * confirm would store. */
    seq_filter_t tmp = *f;
    tmp.filter_type       = (f->filter_type < SEQ_FILTER_COUNT) ? f->filter_type : FILTER_NONE;
    tmp.cutoff_hz         = SEQ_CLAMP_F32(f->cutoff_hz,  65.0f, 16000.0f);
    tmp.resonance         = SEQ_CLAMP_F32(f->resonance,  0.51f, 8.0f);
    tmp.feedback          = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
    tmp.ks_duty_ofs       = SEQ_CLAMP_F32(f->ks_duty_ofs, -0.5f, 0.5f);
    tmp.key_track         = SEQ_CLAMP_F32(f->key_track, 0.0f, 1.0f);
    for (uint8_t eg = 0; eg < 2u; eg++)
        for (uint8_t t = 0; t < SEQ_EGT_COUNT; t++)
            tmp.eg_depth[eg][t] = SEQ_CLAMP_F32(f->eg_depth[eg][t],
                                                -seq_eg_depth_max(t),
                                                seq_eg_depth_max(t));
    /* Register the scratch so the software-LFO service sweeps around the
     * in-progress cutoff instead of stomping it from the store. */
    s_preview_gen++;
    preview_slot_touch(layer_idx, track);
    s_preview.filter       = tmp;
    s_preview.filter_valid = true;
    s_preview_gen++;
    melodic_filter_apply(layer_idx, track, &tmp);
}

bool sequencer_core_melodic_filter_authored(uint8_t layer_idx, uint8_t track)
{
    return sequencer_core_melodic_group_authored(layer_idx, track, SEQ_VP_GROUP_FILTER);
}
