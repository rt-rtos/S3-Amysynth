#include "sequencer_core/seq_core_internal.h"
#include "voice_config.h"
#include "seq_clamp.h"

/* ── State definitions — owns LFO phase accumulators ────────────────── */
/* Software LFO state (phase accumulator, per-layer/track) */
float    s_lfo_phase[MAX_LAYERS][SEQ_TRACKS]; /* 0..1 normalized */
float    s_lfo_hz[MAX_LAYERS][SEQ_TRACKS];    /* Hz from rate+BPM; 0 = native or inactive */
float    s_lfo_rnd[MAX_LAYERS][SEQ_TRACKS];   /* S&H held value   */
uint32_t s_lfo_rng_state = 0xDEADBEEFu;

/* ── AMY native LFO helpers (wave patches only) ──────────────────────── */

#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO

/* True when an authored track should use the AMY native LFO. Caller must
 * already know the patch is native-layout (sequencer_core_lfo_native_layout);
 * every enabled LFO on such a patch is native. */
static bool is_native_lfo_track(const seq_lfo_t *lfo)
{
    return lfo->enabled;
}

/* Apply an EXPLICIT lfo struct (stored or an editor's live scratch) to one
 * track's native topology, activating or deactivating it (carrier dormant,
 * COEF_MOD cleared) so the caller always reaches a consistent AMY state; the
 * wrapper below keeps the stored-state callers unchanged. */
static void melodic_native_lfo_apply(const seq_layer_t *layer, uint8_t track,
                                     const seq_lfo_t *lfo)
{
    seq_voice_layout_t vl;
    uint8_t layer_idx = (uint8_t)(layer - s_layers);
    if (!seq_track_voice_layout(layer_idx, track, &vl))
        return;
    voice_apply_native_lfo_topo(layer->synth_id[track],
                                is_native_lfo_track(lfo) ? lfo : NULL, s_bpm,
                                vl.carrier, vl.pitch_mask, vl.voice_mask);
}

static void melodic_configure_native_lfo_track(uint8_t layer_idx, uint8_t track)
{
    melodic_native_lfo_apply(&s_layers[layer_idx], track,
                             &seq_track_vp(layer_idx, track)->lfo);
}

#endif /* CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO */

/* The row's SILENT-head oscs, or 0 when its voice carries the filter and dist
 * stages the flat way (broadcast / base osc). Every osc-addressed per-voice
 * push resolves the layout through here. */
static uint8_t melodic_heads_mask(uint8_t layer_idx, uint8_t track)
{
    seq_voice_layout_t vl;
    if (!seq_track_voice_layout(layer_idx, track, &vl)) return 0u;
    return vl.heads_mask;
}

/* Push one EG1 breakpoint set to every osc that can carry a filter-env rail
 * (sequencer_core_push_envelope_eg1() says why each needs it).
 * voice_mask is where the per-voice stages live: the SILENT heads on a headed
 * row, the two cluster oscs on an engine row, the copies on a fan row, osc 0
 * on a plain wave row. */
static void melodic_eg1_push(uint8_t layer_idx, uint8_t track,
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

static void melodic_filter_apply(uint8_t layer_idx, uint8_t track,
                                 const seq_filter_t *f);

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
static void filter_push_eg_depths(amy_event *e, bool own, const seq_filter_t *f)
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
static void dist_push_eg_depths(uint8_t synth, bool own, const seq_filter_t *f)
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
static void scan_push_eg_depths(uint8_t synth, uint8_t mask, bool own,
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

/* Push one dist block where the row's layout carries the stage, then, on an
 * owning row, its routing depths (filter_push_eg_depths). */
static void melodic_dist_apply(uint8_t layer_idx, uint8_t track,
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

/* Restore the resting coefficient for every target the LFO was driving, so
 * nothing stays modulated after it is switched off. FILTER restores the
 * track's authored cutoff; the rest push a neutral constant. */
static void lfo_restore_target_neutrals(uint8_t layer_idx, uint8_t track,
                                        const seq_lfo_t *lfo)
{
    const voice_params_t *vp = seq_track_vp(layer_idx, track);
    for (int t = 0; t < LFO_TARGET_COUNT; t++) {
        if (!(lfo->targets & LFO_TGT_BIT(t))) continue;
        if (t == LFO_TARGET_FILTER)
            melodic_filter_apply(layer_idx, track, &vp->filter);
        else if (t == LFO_TARGET_DIST_DRIVE || t == LFO_TARGET_DIST_MIX) {
            /* Neutral = the committed dist block; only this caller has it,
             * so lfo_push_target_neutral leaves DIST alone (like FILTER).
             * The two dist bits share one restore event - re-pushing the same
             * block for the second bit would just cost a redundant event. */
            if (t == LFO_TARGET_DIST_DRIVE ||
                !LFO_HAS_TGT(lfo, LFO_TARGET_DIST_DRIVE))
                melodic_dist_apply(layer_idx, track, &vp->dist);
        }
        else
            lfo_push_target_neutral(s_layers[layer_idx].synth_id[track],
                                    s_layers[layer_idx].track_patch[track],
                                    (lfo_target_t)t);
    }
    /* The AMP and PAN neutrals are voice-wide pushes: they overwrite the
     * copies' blend weights and the heads' spread, so restate them. */
    uint8_t copies = seq_track_unison_copies(layer_idx, track);
    if (copies > 1u) {
        voice_unison_t eff = sequencer_core_get_track_unison(layer_idx, track);
        eff.count = copies;
        voice_push_unison_live(s_layers[layer_idx].synth_id[track], &eff, 1.0f);
    }
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

static void melodic_push_peers(uint8_t layer_idx, uint8_t track,
                               void (*push)(uint8_t, uint8_t))
{
    uint8_t peers[SEQ_TRACKS];
    uint8_t n = sequencer_core_melodic_vp_peers(layer_idx, track, peers);
    for (uint8_t i = 0; i < n; ++i) push(layer_idx, peers[i]);
}

/* Log tag: which block a row's commit landed in. */
static const char *melodic_vp_where(uint8_t layer_idx, uint8_t track)
{
    return (s_layers[layer_idx].vp_src[track] == SEQ_VP_SRC_LAYER) ? "layer"
                                                                    : "track";
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

/* ── Per-row melodic filter (runtime-editable) ─────────────────────────── */

/* Maps filter Q [0.51,8.0] linearly onto [0,1]. Predates
 * seq_filter_t.feedback, when KS string decay was derived from Q; live apply
 * paths use the feedback field directly and nothing calls this. */
float sequencer_core_ks_feedback_from_q(float q)
{
    float n = (q - 0.51f) / (8.0f - 0.51f);
    return SEQ_CLAMP_F32(n, 0.0f, 1.0f);
}

/* One filter event, either broadcast to the whole voice (osc < 0) or addressed
 * to one voice-relative osc - the SILENT-head layout carries the stage on each
 * head instead of on every sounding osc. */
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
static void melodic_filter_apply(uint8_t layer_idx, uint8_t track,
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
static void melodic_filter_commit_track(uint8_t layer_idx, uint8_t track)
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

/* Active editor-preview slot; contract at the "Live-preview pushes" section
 * below. WRITTEN by the editor handlers (encoder/button tasks), READ by
 * sequencer_core_lfo_service() on synth_ui_task - same core, same priority
 * tier, so a round-robin boundary can land mid-write. seq_core is lock-free
 * by discipline, so consistency is a generation counter (seqlock): writers
 * bump to odd, mutate, bump to even; the reader snapshots once per service
 * tick and falls back to the store if it caught a writer. An aligned 32-bit
 * counter is single-copy atomic on Xtensa. */
typedef struct {
    bool         active;
    uint8_t      li, tr;
    bool         filter_valid;
    seq_filter_t filter;
    bool         lfo_valid;
    seq_lfo_t    lfo;
} melodic_preview_t;
static melodic_preview_t s_preview;
static volatile uint32_t s_preview_gen;   /* odd = writer mid-update */

/* Re-point the slot at (li, tr), dropping stale scratch from another track.
 * Callers hold the write side (odd s_preview_gen). */
static void preview_slot_touch(uint8_t li, uint8_t tr)
{
    if (!s_preview.active || s_preview.li != li || s_preview.tr != tr) {
        s_preview = (melodic_preview_t){0};
        s_preview.active = true;
        s_preview.li = li;
        s_preview.tr = tr;
    }
}

/* Consistent snapshot for the service loop; returns false when no preview is
 * active or a writer was mid-update (caller then reads the store as usual -
 * one 50 ms tick of committed values, self-healing). */
static bool preview_snapshot(melodic_preview_t *out)
{
    uint32_t g0 = s_preview_gen;
    if (g0 & 1u) return false;
    *out = s_preview;
    if (s_preview_gen != g0) return false;
    return out->active;
}

/* Push an EXPLICIT lfo struct to one track's engine state (native topology or
 * software-service arming) without touching the store. Shared by the
 * committing setter (which passes the just-stored struct), the live preview
 * (editor scratch) and cancel-restore (stored struct). */
static void melodic_lfo_apply_runtime(uint8_t layer_idx, uint8_t track,
                                      const seq_lfo_t *lfo)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
    /* Native topology is melodic-only: it writes voice-relative oscs 1 and 2,
     * and AMY applies the base_osc offset without a bounds check, so on a
     * 1-osc-per-voice synth (a PCM drum slot) those events would land on the
     * NEXT synth's oscillators. A drum row's stored patch is only its
     * SYNTH-mode selection, so it must never enable the native path. */
    bool is_native = layer->type == SEQ_LAYER_MELODIC &&
                     sequencer_core_lfo_native_layout(layer->track_patch[track],
                                                      NULL, NULL);
    if (is_native) {
        melodic_native_lfo_apply(layer, track, lfo);
        /* Restore the static target value when disabled: native clears COEF_MOD
         * but does not push the neutral coef. */
        if (!lfo->enabled || !is_native_lfo_track(lfo)) {
            lfo_restore_target_neutrals(layer_idx, track, lfo);
        }
        /* s_lfo_hz = 0 makes the software service loop skip native tracks;
         * every target, DIST included, rides the carrier. */
        s_lfo_hz[layer_idx][track] = 0.0f;
        return;
    }
#endif
    /* Software path: patches without a carrier pair, or native LFO compiled out. */
    if (!lfo->enabled) {
        lfo_restore_target_neutrals(layer_idx, track, lfo);
        s_lfo_hz[layer_idx][track] = 0.0f;
    } else {
        s_lfo_hz[layer_idx][track] = seq_lfo_sw_hz(lfo->rate, s_bpm);
    }
}

/* The LFO a block actually runs: its stored one while authored, otherwise a
 * disabled copy. A released block keeps its values (so re-authoring restores
 * them) but must not modulate anything meanwhile. */
static seq_lfo_t melodic_effective_lfo(const voice_params_t *vp)
{
    seq_lfo_t l = vp->lfo;
    if (!vp->lfo_authored) l.enabled = false;
    return l;
}

/* Runtime re-push of one row's stored LFO; the push callback shape the
 * fan-out helper takes. */
static void melodic_reapply_lfo_track(uint8_t layer_idx, uint8_t track)
{
    seq_lfo_t l = melodic_effective_lfo(seq_track_vp(layer_idx, track));
    melodic_lfo_apply_runtime(layer_idx, track, &l);
}

void sequencer_core_set_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    const seq_lfo_t *lfo)
{
    if (!lfo || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    voice_params_t *vp = seq_track_vp(layer_idx, track);

    vp->lfo = *lfo;
    if (vp->lfo.depth > 100) vp->lfo.depth = 100;
    vp->lfo_authored = true;

    melodic_push_peers(layer_idx, track, melodic_reapply_lfo_track);
    ESP_LOGI(TAG, "LFO L%u T%u [%s] %s %.2f Hz d=%u tgt=0x%02x",
             layer_idx + 1u, track + 1u, melodic_vp_where(layer_idx, track),
             lfo->enabled ? "ON" : "OFF",
             (double)s_lfo_hz[layer_idx][track], lfo->depth, lfo->targets);
}

/* Cancel-restore for the LFO live preview: re-push the stored (committed)
 * state. Safe on a never-authored row - the effective LFO is disabled with
 * an empty target set, so the restore is a no-op push. */
void sequencer_core_reapply_melodic_lfo(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    melodic_reapply_lfo_track(layer_idx, track);
}

bool sequencer_core_get_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    seq_lfo_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = seq_track_vp(layer_idx, track)->lfo;
    return true;
}

void __attribute__((optimize("O3", "unroll-loops", "fast-math"))) sequencer_core_lfo_service(void) 
{
    const float DT = 0.05f; /* 20 Hz */
    /* One consistent preview snapshot per service tick: an open editor's
     * scratch overrides the store for its one track, so relational edits are
     * audible live instead of being stomped back to committed values. */
    melodic_preview_t prev;
    bool prev_ok = preview_snapshot(&prev);
    for (int li = 0; li < s_num_layers; li++) {
        for (int tr = 0; tr < s_layers[li].num_tracks; tr++) {
            bool prev_here = prev_ok &&
                             prev.li == (uint8_t)li &&
                             prev.tr == (uint8_t)tr;
            bool lfo_previewed = prev_here && prev.lfo_valid;
            const voice_params_t *vp = seq_track_vp((uint8_t)li, (uint8_t)tr);
            if (!lfo_previewed && !vp->lfo_authored) continue;
            const seq_lfo_t *lfo = lfo_previewed ? &prev.lfo : &vp->lfo;
            if (!lfo->enabled) continue;
            float hz = s_lfo_hz[li][tr];
            if (hz <= 0.0f) continue;

            float ph = s_lfo_phase[li][tr] + hz * DT;
            if (ph >= 1.0f) {
                ph -= 1.0f;
                if (lfo->wave == LFO_WAVE_RANDOM)
                    s_lfo_rnd[li][tr] = lfo_next_rand();
            }
            s_lfo_phase[li][tr] = ph;

            float val;
            switch (lfo->wave) {
                case LFO_WAVE_SINE:
                    val = sinf(2.0f * 3.14159265f * ph);          break;
                case LFO_WAVE_TRIANGLE:
                    val = (ph < 0.5f) ? (4.0f*ph - 1.0f)
                                      : (3.0f - 4.0f*ph);         break;
                case LFO_WAVE_SAW_UP:   val =  2.0f*ph - 1.0f;   break;
                case LFO_WAVE_SAW_DOWN: val =  1.0f - 2.0f*ph;   break;
                case LFO_WAVE_SQUARE:   val = (ph < 0.5f) ? 1.0f : -1.0f; break;
                case LFO_WAVE_RANDOM:   val = s_lfo_rnd[li][tr];  break;
                default:                val = 0.0f;                break;
            }

            float d   = (float)lfo->depth / 100.0f;
            uint8_t syn = s_layers[li].synth_id[tr];

#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
            /* Native tracks disarm the stepper (s_lfo_hz = 0 in
             * melodic_lfo_apply_runtime) and never reach this body; the guard
             * is belt-and-braces, since stepping a native track would
             * double-modulate. */
            bool native_track =
                s_layers[li].type == SEQ_LAYER_MELODIC &&
                sequencer_core_lfo_native_layout(s_layers[li].track_patch[tr],
                                                 NULL, NULL);
#else
            const bool native_track = false;
#endif
            if (!native_track) {
            amy_event *e = amy_helpers_event_begin();
            e->synth = syn;
            /* Multi-target: each checked target modulates its own COEF_CONST
             * from the same LFO value. SCAN has no software analog (a
             * wavetable voice always takes the native path); DIST is handled
             * after this block. */
            if (LFO_HAS_TGT(lfo, LFO_TARGET_FILTER)) {
                const seq_filter_t *fb = (prev_here && prev.filter_valid)
                                         ? &prev.filter
                                         : &vp->filter;
                float base = (fb->enabled && fb->cutoff_hz > 0.0f)
                             ? sequencer_core_filter_const_hz(fb) : 1000.0f;
                e->filter_freq_coefs[COEF_CONST] =
                    base * powf(2.0f, voice_lfo_filter_octaves(lfo) * val);
            }
            if (LFO_HAS_TGT(lfo, LFO_TARGET_PAN))
                e->pan_coefs[COEF_CONST] = 0.5f + d*0.5f*val;
            amy_helpers_event_send(e);
            if (LFO_HAS_TGT(lfo, LFO_TARGET_AMP))
                lfo_push_amp(syn, s_layers[li].track_patch[tr],
                             1.0f - d*(0.5f - 0.5f*val));
            } /* !native_track */

            /* DIST on a PATCH-mode track: step drive/mix around the committed
             * dist block (voice_push_dist_lfo). Inert while the shaper is OFF. */
            if (!native_track && (lfo->targets & LFO_TGT_DIST_MASK))
                voice_push_dist_lfo(syn, &vp->dist, lfo, val);

            if (!native_track && LFO_HAS_TGT(lfo, LFO_TARGET_PITCH)) {
                /* Anchored on SEQ_LFO_PITCH_BASE_HZ, so the constant term is
                 * exactly d*val octaves. Pushed to osc 0 ONLY: a synth-wide
                 * event would rewrite patch-internal modulator oscs' freq
                 * CONST, which is their rate, and patch LFOs (chorus/PWM)
                 * would not recover short of a patch reload. */
                amy_event *pe = amy_helpers_event_begin();
                pe->synth = syn;
                pe->osc   = 0;
                pe->freq_coefs[COEF_CONST] =
                    SEQ_LFO_PITCH_BASE_HZ * powf(2.0f, d * VOICE_LFO_DEPTH_PITCH * val);
                amy_helpers_event_send(pe);
            }
        }
    }
}

/* ── Native LFO rebuild helpers ─────────────────────────────────────────────
 * Called after a patch/synth rebuild and after BPM changes, so native LFO
 * carrier state stays consistent with the layer's patch and tempo. */

/* Re-apply the authored native LFO for every row whose patch reserves a
 * carrier pair. Skipped rows fall to the software loop. */
void sequencer_configure_melodic_lfo(uint8_t layer_idx, uint8_t rows)
{
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
    const seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < rows; t++) {
        /* Per row: a mixed layer can have wave rows next to string rows, and
         * only the former reserve a carrier pair. */
        if (!sequencer_core_lfo_native_layout(layer->track_patch[t], NULL, NULL))
            continue;
        if (!seq_track_vp(layer_idx, t)->lfo_authored) continue;
        melodic_configure_native_lfo_track(layer_idx, t);
        /* Keep s_lfo_hz in sync so the service loop skips native tracks
         * (mirrors melodic_lfo_apply_runtime). */
        s_lfo_hz[layer_idx][t] = 0.0f;
    }
#else
    (void)layer_idx;
    (void)rows;
#endif
}

/* Update the carrier frequency on all active native-LFO tracks after a BPM
 * change. Mirrors arp_core_refresh_lfo_freq(). */
void melodic_lfo_refresh_native_freq(void)
{
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
    for (int li = 0; li < s_num_layers; li++) {
        const seq_layer_t *layer = &s_layers[li];
        /* Melodic-only, same reason as sequencer_core_set_melodic_lfo(): the
         * carrier oscs don't exist on non-melodic (1-osc) synths. */
        if (layer->type != SEQ_LAYER_MELODIC)
            continue;
        for (int tr = 0; tr < layer->num_tracks; tr++) {
            if (!sequencer_core_lfo_native_layout(layer->track_patch[tr], NULL, NULL))
                continue;
            const voice_params_t *vp = seq_track_vp((uint8_t)li, (uint8_t)tr);
            if (!vp->lfo_authored) continue;
            const seq_lfo_t *lfo = &vp->lfo;
            if (!is_native_lfo_track(lfo)) continue;
            seq_voice_layout_t vl;
            /* Carrier index is per track under unison (copy fans differ). */
            if (!seq_track_voice_layout((uint8_t)li, (uint8_t)tr, &vl))
                continue;
            uint8_t carrier = vl.carrier;
            amy_event *e = amy_helpers_event_begin();
            e->synth                  = layer->synth_id[tr];
            e->osc                    = carrier;
            e->freq_coefs[COEF_CONST] = lfo_rate_to_hz(lfo->rate, s_bpm);
            amy_helpers_event_send(e);
            /* Keep the wobble modulator (carrier+1) BPM-synced as well. */
            e = amy_helpers_event_begin();
            e->synth                  = layer->synth_id[tr];
            e->osc                    = (uint8_t)(carrier + 1u);
            e->freq_coefs[COEF_CONST] = lfo_rate_to_hz((note_div_t)lfo->wob_rate, s_bpm);
            amy_helpers_event_send(e);
        }
    }
#endif
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

/* ── Live-preview pushes (AMY only; the store is untouched) ──────────────────
 * Contract (preview, commit, cancel): ENGINE-SEMANTICS.md, "Editor preview and
 * cancel".
 *
 * The single active-preview slot below is what makes RELATIONAL edits
 * composable: the software-LFO service recomputes swept COEF_CONST values
 * every 50 ms from the model, so without it an uncommitted cutoff or LFO
 * scratch would be stomped back toward the store on the next tick (or, for
 * the LFO, not heard at all until commit). Editors register their scratch
 * here; the service prefers it over the store for the matching track. One
 * slot suffices - only one editor is open at a time, and filter/EG1/LFO
 * previews for the SAME track compose (each keeps its own valid flag).
 * (Slot definitions live above the service loop, which reads them.) */

void sequencer_core_preview_melodic_clear(void)
{
    s_preview_gen++;
    s_preview = (melodic_preview_t){0};
    s_preview_gen++;
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

void sequencer_core_preview_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                        const seq_lfo_t *lfo)
{
    if (!lfo || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    /* Same clamp as the committing setter. */
    seq_lfo_t tmp = *lfo;
    if (tmp.depth > 100) tmp.depth = 100;

    s_preview_gen++;
    preview_slot_touch(layer_idx, track);
    s_preview.lfo       = tmp;
    s_preview.lfo_valid = true;
    s_preview_gen++;

    /* Native tracks hear the scratch immediately via the carrier topology;
     * software tracks are armed here (rate/phase) and the service loop reads
     * the slot for the rest. s_lfo_hz is runtime pacing, not authored state -
     * cancel restores it via sequencer_core_reapply_melodic_lfo(). */
    melodic_lfo_apply_runtime(layer_idx, track, &tmp);
}

bool sequencer_core_melodic_env_authored(uint8_t layer_idx, uint8_t track,
                                         uint8_t eg_index)
{
    return sequencer_core_melodic_group_authored(
        layer_idx, track, (eg_index == 1) ? SEQ_VP_GROUP_ENV1 : SEQ_VP_GROUP_ENV);
}

bool sequencer_core_melodic_filter_authored(uint8_t layer_idx, uint8_t track)
{
    return sequencer_core_melodic_group_authored(layer_idx, track, SEQ_VP_GROUP_FILTER);
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
 * (software pacing, native carrier) is re-armed first either way: the reload
 * path only covers native tracks, and its neutral restores must land before
 * the block's filter/dist pushes, not after. */
static void melodic_repush_track(uint8_t layer_idx, uint8_t track)
{
    const voice_params_t *vp = seq_track_vp(layer_idx, track);
    melodic_reapply_lfo_track(layer_idx, track);
    bool all_authored = vp->env_authored && vp->env1_authored &&
                        vp->filter_authored && vp->dist_authored;
    if (!all_authored) {
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
    seq_lfo_t off = melodic_effective_lfo(seq_track_vp(layer_idx, track));
    off.enabled = false;
    melodic_lfo_apply_runtime(layer_idx, track, &off);

    layer->vp_src[track] = (uint8_t)src;
    melodic_repush_track(layer_idx, track);
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
             * the next tempo change: disarm the stepper / native carrier and
             * restore the rails before the reload re-imposes the patch. */
            melodic_push_peers(layer_idx, track, melodic_reapply_lfo_track);
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
