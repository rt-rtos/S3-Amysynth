/* ── Melodic voice-block plumbing shared by the editor files ──
 * The envelope-routing (EG depth) pushers for cutoff, dist and wavetable scan,
 * the row's heads mask, and the voice-block source selector with its
 * peer fan-out, authored-group query and release. The per-group editors live in
 * seq_core_env.c, seq_core_filter.c, seq_core_dist.c and seq_core_lfo.c. */
#include "sequencer_core/seq_core_internal.h"
#include "lfo_targets.h"

/* The row's unison cluster oscs, or 0 when its voice carries the filter and
 * dist stages the flat way (broadcast / base osc). Every osc-addressed
 * per-voice push resolves the layout through here. */
uint8_t melodic_heads_mask(uint8_t layer_idx, uint8_t track)
{
    seq_voice_layout_t vl;
    if (!seq_track_voice_layout(layer_idx, track, &vl)) return 0u;
    return vl.heads_mask;
}

/* Write the row's eight envelope-routing depths into a pending event, one per
 * target coef vector and envelope slot. `own`: seq_filter_t.eg_depth says which
 * rows own the matrix (a drum row, or a row whose filter block is authored). The
 * dist push and this one never clobber each other - amy_event slots start at
 * the unset sentinel and voice_apply_dist_osc() authors CONST only. The cutoff
 * rails ride the filter and go out only while it is enabled. The DRIVE/MIX
 * rails go out only on an osc-addressed event: AMY reads dist fields on an
 * event that names no osc at bus scope (CONST only) and drops them before the
 * voice fan-out, so those rows get dist_push_eg_depths() instead. The SCAN
 * rails never ride this event and go out through scan_push_eg_depths(). Key
 * tracking (COEF_NOTE) rides the cutoff with the same ownership: an owning row
 * writes it, 0 included, so a patch's own tracking yields to the row. */
void filter_push_eg_depths(amy_event *e, bool own, const seq_filter_t *f)
{
    bool dist_ok = AMY_IS_SET(e->osc);
    if (f->enabled && (own || f->key_track != 0.0f))
        e->filter_freq_coefs[COEF_NOTE] = f->key_track;
    for (uint8_t eg = 0; eg < 2u; eg++) {
        uint8_t slot = (eg == 0u) ? COEF_EG0 : COEF_EG1;
        for (uint8_t t = 0; t < SEQ_EGT_COUNT; t++) {
            if (t == SEQ_EGT_CUTOFF && !f->enabled) continue;
            if ((t == SEQ_EGT_DRIVE || t == SEQ_EGT_MIX) && !dist_ok) continue;
            if (t == SEQ_EGT_SCAN) continue;
            float d = f->eg_depth[eg][t];
            if (!own && d == 0.0f) continue;
            switch (t) {
                case SEQ_EGT_PITCH:  e->freq_coefs[slot]        = d; break;
                case SEQ_EGT_CUTOFF: e->filter_freq_coefs[slot] = d; break;
                case SEQ_EGT_DRIVE:  e->dist_drive_coefs[slot]  = d; break;
                case SEQ_EGT_MIX:    e->dist_mix_coefs[slot]    = d; break;
                default: break;
            }
        }
    }
}

/* The DRIVE/MIX rails alone, to osc 0, where voice_apply_dist() puts the stage
 * on a row whose depth event names no osc. Ownership as filter_push_eg_depths(). */
void dist_push_eg_depths(uint8_t synth, bool own, const seq_filter_t *f)
{
    bool any = own;
    for (uint8_t eg = 0; eg < 2u; eg++)
        if (f->eg_depth[eg][SEQ_EGT_DRIVE] != 0.0f || f->eg_depth[eg][SEQ_EGT_MIX] != 0.0f)
            any = true;
    if (!any) return;
    amy_event *e = amy_helpers_event_begin();
    e->synth = synth;
    e->osc   = 0;
    for (uint8_t eg = 0; eg < 2u; eg++) {
        uint8_t slot = (eg == 0u) ? COEF_EG0 : COEF_EG1;
        float dd = f->eg_depth[eg][SEQ_EGT_DRIVE];
        float dm = f->eg_depth[eg][SEQ_EGT_MIX];
        if (own || dd != 0.0f) e->dist_drive_coefs[slot] = dd;
        if (own || dm != 0.0f) e->dist_mix_coefs[slot]   = dm;
    }
    amy_helpers_event_send(e);
}

/* The resting scan position. An envelope starts each note at 0, so a routed
 * SCAN rests on the frame its sweep leaves from: the first frame, or the last
 * when every routed depth is negative. Unrouted, it rests on the middle frame
 * (0.5), which the LFO's SCAN swings around. */
float seq_scan_rest(const seq_filter_t *f)
{
    float d0 = f->eg_depth[0][SEQ_EGT_SCAN];
    float d1 = f->eg_depth[1][SEQ_EGT_SCAN];
    if (d0 > 0.0f || d1 > 0.0f) return 0.0f;
    if (d0 < 0.0f || d1 < 0.0f) return 1.0f;
    return 0.5f;
}

/* Contract in seq_core_internal.h. The voice block resolves as seq_track_vp()
 * does, from the layer pointer. */
float seq_track_frame_duty(const seq_layer_t *layer, uint8_t track, uint8_t step)
{
    if (track >= SEQ_TRACKS || !sequencer_core_is_wavetable_patch(layer->track_patch[track]))
        return AMY_UNSET_FLOAT;
    if (step < SEQ_MAX_STEPS) {
        uint8_t lock = layer->step_frame[track][step];
        if (lock >= 1u && lock <= 64u) return (float)(lock - 1u) / 63.0f;
    }
    uint8_t v = layer->wt_frame[track];
    if (v >= 1u && v <= 64u) return (float)(v - 1u) / 63.0f;
    const voice_params_t *vp = (layer->vp_src[track] == SEQ_VP_SRC_LAYER)
                             ? &layer->vp_layer : &layer->vp[track];
    return seq_scan_rest(&vp->filter);
}

/* The SCAN rails alone, one event per osc in `mask` (the oscs that render the
 * wave), with the resting scan position (seq_scan_rest) as the synth-wide
 * baseline; each note-on then sets its own voice's frame
 * (seq_track_frame_duty). Sent on every call, so removing the routing
 * restores the middle. EG slots: ownership as filter_push_eg_depths(). */
void scan_push_eg_depths(uint8_t synth, uint8_t mask, bool own,
                         const seq_filter_t *f)
{
    float d0 = f->eg_depth[0][SEQ_EGT_SCAN];
    float d1 = f->eg_depth[1][SEQ_EGT_SCAN];
    if (!mask) return;
    float rest = seq_scan_rest(f);
    for (uint8_t o = 0; (uint8_t)(mask >> o) != 0u; o++) {
        if (!(mask & (uint8_t)(1u << o))) continue;
        amy_event *e = amy_helpers_event_begin();
        e->synth = synth;
        e->osc   = o;
        e->duty_coefs[COEF_CONST] = rest;
        if (own || d0 != 0.0f) e->duty_coefs[COEF_EG0] = d0;
        if (own || d1 != 0.0f) e->duty_coefs[COEF_EG1] = d1;
        amy_helpers_event_send(e);
    }
}

void sequencer_core_push_eg_scan(uint8_t synth, uint8_t mask,
                                 const seq_filter_t *f, bool own)
{
    if (!f) return;
    scan_push_eg_depths(synth, mask, own, f);
}

void sequencer_core_push_eg_depths(uint8_t synth, int osc,
                                   const seq_filter_t *f, bool own)
{
    if (!f) return;
    amy_event *e = amy_helpers_event_begin();
    e->synth = synth;
    if (osc >= 0) e->osc = (uint8_t)osc;
    filter_push_eg_depths(e, own, f);
    amy_helpers_event_send(e);
    if (osc < 0) dist_push_eg_depths(synth, own, f);
}

/* ── Shared-block fan-out ────────────────────────────────────────────────────
 * Voice-block source selector: sequencer_core.h. Committing setters push to
 * every row that reads the written block (the peer set) through here instead of
 * to `track` alone. */
uint8_t sequencer_core_melodic_vp_peers(uint8_t layer_idx, uint8_t track,
                                        uint8_t peers[SEQ_TRACKS])
{
    if (!peers || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->vp_src[track] != SEQ_VP_SRC_LAYER) {
        peers[0] = track;
        return 1;
    }
    uint8_t n = 0;
    for (uint8_t t = 0; t < layer->num_tracks; ++t)
        if (layer->vp_src[t] == SEQ_VP_SRC_LAYER) peers[n++] = t;
    return n;
}

void melodic_push_peers(uint8_t layer_idx, uint8_t track,
                        void (*push)(uint8_t, uint8_t))
{
    uint8_t peers[SEQ_TRACKS];
    uint8_t n = sequencer_core_melodic_vp_peers(layer_idx, track, peers);
    for (uint8_t i = 0; i < n; ++i) push(layer_idx, peers[i]);
}

/* Log tag: which block a row's commit landed in. */
const char *melodic_vp_where(uint8_t layer_idx, uint8_t track)
{
    return (s_layers[layer_idx].vp_src[track] == SEQ_VP_SRC_LAYER) ? "layer"
                                                                    : "track";
}

void sequencer_core_preview_melodic_clear(void)
{
    s_preview_gen++;
    s_preview = (melodic_preview_t){0};
    s_preview_gen++;
}

bool sequencer_core_melodic_group_authored(uint8_t layer_idx, uint8_t track,
                                           seq_vp_group_t group)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    const voice_params_t *vp = seq_track_vp(layer_idx, track);
    switch (group) {
        case SEQ_VP_GROUP_ENV:    return vp->env_authored;
        case SEQ_VP_GROUP_ENV1:   return vp->env1_authored;
        case SEQ_VP_GROUP_FILTER: return vp->filter_authored;
        case SEQ_VP_GROUP_LFO:    return vp->lfo_authored;
        case SEQ_VP_GROUP_LFO2:   return vp->lfo2_authored;
        case SEQ_VP_GROUP_DIST:   return vp->dist_authored;
        default:                  return false;
    }
}

/* ── Voice-block source selector ──────────────────────────────────────────
 * Contract in sequencer_core.h. The audible state of a row must be a function
 * of stored data alone (project load, layer switch and patch change all
 * re-push from the store), so the selector is the row's persisted vp_src and
 * every push path resolves through seq_track_vp(). */

seq_vp_src_t sequencer_core_get_melodic_vp_source(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return SEQ_VP_SRC_TRACK;
    return (seq_vp_src_t)s_layers[layer_idx].vp_src[track];
}

/* Make one row's synth sound like its currently selected block. Direct pushes
 * when every group is authored; otherwise a layer reload, the only way to get
 * the patch's own values back for the unauthored groups (it re-pushes every
 * authored group of the whole layer through the resolver). The LFO runtime
 * (software pacing, shared LFO synth or native carrier) is re-armed first
 * either way: the reload path only covers live mapped / native tracks, and
 * its neutral restores must land before the block's filter/dist pushes, not
 * after. force_reload: the caller unwired a Juno/DX7 patch's own LFO from
 * slot 0, so a block whose LFO2 is collapsed also takes the reload, the only
 * way that patch LFO comes back. */
static void melodic_repush_track(uint8_t layer_idx, uint8_t track,
                                 bool force_reload)
{
    const voice_params_t *vp = seq_track_vp(layer_idx, track);
    melodic_reapply_lfo_track(layer_idx, track);
    melodic_reapply_lfo2_track(layer_idx, track);
    bool all_authored = vp->env_authored && vp->env1_authored &&
                        vp->filter_authored && vp->dist_authored;
    if (!all_authored || (force_reload && !vp->lfo2_authored)) {
        sequencer_reconfigure_layer_paused(layer_idx);
        return;
    }
    sequencer_configure_melodic_envelope_track(layer_idx, track);
    sequencer_configure_melodic_envelope1_track(layer_idx, track);
    melodic_filter_commit_track(layer_idx, track);
    sequencer_configure_melodic_dist_track(layer_idx, track);
}

bool sequencer_core_set_melodic_vp_source(uint8_t layer_idx, uint8_t track,
                                          seq_vp_src_t src)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return false;
    if (layer->vp_src[track] == (uint8_t)src) return true;

    /* Park the departing block's LFO while it is still the resolved one: the
     * stepper only restores the rails of the lfo it is handed, so a target
     * the new block does not modulate would otherwise stay at its last swept
     * value. */
    seq_lfo_t off = melodic_effective_lfo_idx(layer_idx, track, 0u);
    off.enabled = false;
    melodic_lfo_apply_runtime(layer_idx, track, &off);
    /* LFO2 twin: only unwire an open one (the repush below rewires or
     * reloads); a collapsed one never sent anything to slot 0. On a Juno/DX7
     * patch that unwire also clears the patch's own LFO, so the repush must
     * reload unless the new block's LFO2 is open. */
    bool slot0_cleared = melodic_lfo2_open(layer_idx, track) &&
                         lfo_targets_patch_owns_slot0(layer->track_patch[track]);
    if (melodic_lfo2_open(layer_idx, track))
        melodic_unwire_lfo2_track(layer_idx, track);

    layer->vp_src[track] = (uint8_t)src;
    melodic_repush_track(layer_idx, track, slot0_cleared);
    ESP_LOGI(TAG, "L%u T%u voice source -> %s", layer_idx + 1u, track + 1u,
             melodic_vp_where(layer_idx, track));
    return true;
}

void sequencer_core_release_melodic_group(uint8_t layer_idx, uint8_t track,
                                          seq_vp_group_t group)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    if (s_layers[layer_idx].type != SEQ_LAYER_MELODIC) return;
    voice_params_t *vp = seq_track_vp(layer_idx, track);
    switch (group) {
        case SEQ_VP_GROUP_ENV:    vp->env_authored    = false; break;
        case SEQ_VP_GROUP_ENV1:   vp->env1_authored   = false; break;
        case SEQ_VP_GROUP_FILTER: vp->filter_authored = false; break;
        case SEQ_VP_GROUP_LFO:
            vp->lfo_authored = false;
            /* Every row reading this block loses its modulator now, not on
             * the next tempo change: disarm the stepper / unwire the shared
             * LFO synth or native carrier and restore the rails before the
             * reload re-imposes the patch. */
            melodic_push_peers(layer_idx, track, melodic_reapply_lfo_track);
            break;
        case SEQ_VP_GROUP_LFO2:
            /* Collapse: slot 0 is handed back. Unwire it on every reader
             * now; the reload then re-applies the patch (a Juno/DX7 patch
             * LFO returns) and sends nothing for the collapsed LFO2. */
            melodic_push_peers(layer_idx, track, melodic_unwire_lfo2_track);
            vp->lfo2_authored = false;
            break;
        case SEQ_VP_GROUP_DIST:   vp->dist_authored   = false; break;
        default: return;
    }
    /* The patch's own values for the released group live only in the patch
     * string: a reload is the one way to hear them again. It re-pushes every
     * still-authored group of every row through the resolver. */
    sequencer_reconfigure_layer_paused(layer_idx);
    ESP_LOGI(TAG, "L%u T%u [%s] group %d released to patch", layer_idx + 1u,
             track + 1u, melodic_vp_where(layer_idx, track), (int)group);
}
