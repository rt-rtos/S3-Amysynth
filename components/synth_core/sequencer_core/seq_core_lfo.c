#include "sequencer_core/seq_core_internal.h"
#include "voice_config.h"
#include "lfo_bank.h"
#include "lfo_targets.h"
#include "synth_level.h"

/* ── State definitions — owns LFO phase accumulators ────────────────── */
/* Software LFO state (phase accumulator, per-layer/track) */
float    s_lfo_phase[MAX_LAYERS][SEQ_TRACKS]; /* 0..1 normalized */
float    s_lfo_hz[MAX_LAYERS][SEQ_TRACKS];    /* Hz from rate+BPM; 0 = native, bank or inactive */
float    s_lfo_rnd[MAX_LAYERS][SEQ_TRACKS];   /* S&H held value   */
uint32_t s_lfo_rng_state = 0xDEADBEEFu;
#if CONFIG_SEQ_LFO_SHARED
uint8_t  s_lfo_synth[MAX_LAYERS][SEQ_TRACKS]; /* bank reference; LFO_BANK_NONE = none */
uint8_t  s_lfo2_synth[MAX_LAYERS][SEQ_TRACKS]; /* the same for LFO2 (mod slot 0) */

/* Release the row's bank reference, if any. Sends no AMY event: callers reach
 * it after a patch re-apply (which cleared the row's mod slot 1) or after
 * their own unwire. */
static void melodic_lfo_bank_drop(uint8_t layer_idx, uint8_t track)
{
    lfo_bank_unbind(&s_lfo_synth[layer_idx][track]);
}

/* The same for LFO2's reference. Also the collapsed-LFO2 path: it sends
 * nothing, so a patch's own slot-0 wiring stays. */
static void melodic_lfo2_bank_drop(uint8_t layer_idx, uint8_t track)
{
    lfo_bank_unbind(&s_lfo2_synth[layer_idx][track]);
}
#endif

void melodic_lfo_bank_drop_layer(uint8_t layer_idx)
{
#if CONFIG_SEQ_LFO_SHARED
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        melodic_lfo_bank_drop(layer_idx, t);
        melodic_lfo2_bank_drop(layer_idx, t);
    }
#else
    (void)layer_idx;
#endif
}

bool sequencer_core_track_lfo_bound(uint8_t layer_idx, uint8_t track)
{
#if CONFIG_SEQ_LFO_SHARED
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    return s_lfo_synth[layer_idx][track] != LFO_BANK_NONE;
#else
    (void)layer_idx;
    (void)track;
    return false;
#endif
}

bool sequencer_core_track_lfo2_bound(uint8_t layer_idx, uint8_t track)
{
#if CONFIG_SEQ_LFO_SHARED
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    return s_lfo2_synth[layer_idx][track] != LFO_BANK_NONE;
#else
    (void)layer_idx;
    (void)track;
    return false;
#endif
}

void melodic_lfo_bank_drop_row(uint8_t layer_idx, uint8_t track)
{
#if CONFIG_SEQ_LFO_SHARED
    melodic_lfo_bank_drop(layer_idx, track);
    melodic_lfo2_bank_drop(layer_idx, track);
#else
    (void)layer_idx;
    (void)track;
#endif
}

/* ── AMY native LFO helpers (wave patches only) ──────────────────────── */

#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO && !CONFIG_SEQ_LFO_SHARED

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

#endif /* CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO && !CONFIG_SEQ_LFO_SHARED */

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
     * cluster oscs' group gain and spread pan, so restate them. */
    uint8_t copies = seq_track_unison_copies(layer_idx, track);
    if (copies > 1u) {
        voice_unison_t eff = sequencer_core_get_track_unison(layer_idx, track);
        eff.count = copies;
        voice_push_unison_live(s_layers[layer_idx].synth_id[track], &eff, 1.0f);
    }
}

/* Active editor-preview slot; contract at the "Live-preview pushes" section
 * below. WRITTEN by the editor handlers (encoder/button tasks), READ by
 * sequencer_core_lfo_service() on synth_ui_task - same core, same priority
 * tier, so a round-robin boundary can land mid-write. seq_core is lock-free
 * by discipline, so consistency is a generation counter (seqlock): writers
 * bump to odd, mutate, bump to even; the reader snapshots once per service
 * tick and falls back to the store if it caught a writer. An aligned 32-bit
 * counter is single-copy atomic on Xtensa. */
melodic_preview_t s_preview;
volatile uint32_t s_preview_gen;   /* odd = writer mid-update */

/* Re-point the slot at (li, tr), dropping stale scratch from another track.
 * Callers hold the write side (odd s_preview_gen). */
void preview_slot_touch(uint8_t li, uint8_t tr)
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

/* Push an EXPLICIT lfo struct to one track's engine state (shared LFO synth,
 * native topology or software-service arming) without touching the store.
 * Shared by the committing setter (which passes the just-stored struct), the
 * live preview (editor scratch) and cancel-restore (stored struct). */
#if CONFIG_SEQ_LFO_SHARED
/* The oscs a row's shared LFO synths reach: a wave-topology row's as-built
 * layout (SCAN rides the pitch mask), else a patch string's lfo_targets map.
 * False for an unmapped patch and for every drum row (its stored patch is
 * just its SYNTH-mode selection). */
static bool melodic_lfo_target_map(uint8_t layer_idx, uint8_t track,
                                   lfo_target_map_t *tm)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return false;
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
    seq_voice_layout_t vl;
    if (seq_track_voice_layout(layer_idx, track, &vl)) {
        *tm = (lfo_target_map_t){ .voice_mask = vl.voice_mask,
                                  .pitch_mask = vl.pitch_mask,
                                  .scan_mask  = vl.pitch_mask };
        return true;
    }
#endif
    return lfo_targets_of_patch(layer->track_patch[track], tm);
}
#endif

void melodic_lfo_apply_runtime(uint8_t layer_idx, uint8_t track,
                               const seq_lfo_t *lfo)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
#if CONFIG_SEQ_LFO_SHARED
    /* Every mapped melodic row binds a shared LFO synth through mod slot 1
     * (lfo_bank.h). Acquire before release, so an unchanged source keeps its
     * synth and phase. A full bank or a disabled LFO leaves the row to the
     * software path below. */
    lfo_target_map_t tm;
    bool mapped = melodic_lfo_target_map(layer_idx, track, &tm);
    if (mapped &&
        lfo_bank_bind(layer->synth_id[track], VOICE_LFO_SLOT_LFO1, &tm, lfo, s_bpm,
                      &s_lfo_synth[layer_idx][track]) == LFO_BIND_BANK) {
        /* The stepper was driving this row: put its last CONST writes back
         * to their static values. */
        if (s_lfo_hz[layer_idx][track] != 0.0f)
            lfo_restore_target_neutrals(layer_idx, track, lfo);
        s_lfo_hz[layer_idx][track] = 0.0f;
        return;
    }
    /* Every row that reaches the stepper holds no bank reference. */
    melodic_lfo_bank_drop(layer_idx, track);
#elif CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
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
#else
    (void)layer;
#endif
    /* Software path: no shared LFO synth (bank full, LFO off, no target map)
     * and no native carrier pair, or native LFO compiled out. */
    if (!lfo->enabled) {
        lfo_restore_target_neutrals(layer_idx, track, lfo);
        s_lfo_hz[layer_idx][track] = 0.0f;
    } else {
        s_lfo_hz[layer_idx][track] = seq_lfo_sw_hz(lfo->rate, s_bpm);
    }
}

/* Push an EXPLICIT LFO2 struct to one row (mod slot 0) without touching the
 * store, by the slot-0 rule (ENGINE-SEMANTICS.md, "LFO: native carrier vs
 * software stepper"): open, a mapped row is wired to its shared LFO synth, or
 * unwired while lfo is NULL, off, without targets or the bank is full;
 * collapsed (!open), nothing is sent and only the reference is dropped, so a
 * Juno/DX7 patch keeps its own LFO there. No stepper and no native carrier:
 * without CONFIG_SEQ_LFO_SHARED LFO2 does nothing. */
static void melodic_lfo2_apply_runtime(uint8_t layer_idx, uint8_t track,
                                       const seq_lfo_t *lfo, bool open)
{
#if CONFIG_SEQ_LFO_SHARED
    lfo_target_map_t tm;
    if (open && melodic_lfo_target_map(layer_idx, track, &tm)) {
        (void)lfo_bank_bind(s_layers[layer_idx].synth_id[track], VOICE_LFO_SLOT_LFO2,
                            &tm, lfo, s_bpm, &s_lfo2_synth[layer_idx][track]);
        return;
    }
    melodic_lfo2_bank_drop(layer_idx, track);
#else
    (void)layer_idx;
    (void)track;
    (void)lfo;
    (void)open;
#endif
}

/* The LFO (idx 0 = LFO1, 1 = LFO2) a row actually runs: an open editor's
 * preview copy for that row and index, else its stored one while authored,
 * otherwise a disabled copy. A released block keeps its values (so
 * re-authoring restores them) but must not modulate anything meanwhile. Read
 * by every reload and re-push, so a layer reload during a preview rebinds the
 * previewed LFO, not the stored one. UI task (the preview writers' task). */
seq_lfo_t melodic_effective_lfo_idx(uint8_t layer_idx, uint8_t track,
                                    uint8_t idx)
{
    bool here = s_preview.active && s_preview.li == layer_idx &&
                s_preview.tr == track;
    if (here && idx == 0u && s_preview.lfo_valid)  return s_preview.lfo;
    if (here && idx == 1u && s_preview.lfo2_valid) return s_preview.lfo2;
    const voice_params_t *vp = seq_track_vp(layer_idx, track);
    seq_lfo_t l  = idx ? vp->lfo2 : vp->lfo;
    bool authored = idx ? vp->lfo2_authored : vp->lfo_authored;
    if (!authored) l.enabled = false;
    return l;
}

/* LFO2 open on this row: authored, or on an open LFO2 editor page (the page
 * exists only while LFO2 is open). */
bool melodic_lfo2_open(uint8_t layer_idx, uint8_t track)
{
    if (s_preview.active && s_preview.li == layer_idx && s_preview.tr == track &&
        s_preview.lfo2_valid)
        return true;
    return seq_track_vp(layer_idx, track)->lfo2_authored;
}

/* Runtime re-push of one row's stored LFO; the push callback shape the
 * fan-out helper takes. */
void melodic_reapply_lfo_track(uint8_t layer_idx, uint8_t track)
{
    seq_lfo_t l = melodic_effective_lfo_idx(layer_idx, track, 0u);
    melodic_lfo_apply_runtime(layer_idx, track, &l);
}

/* The same for LFO2. */
void melodic_reapply_lfo2_track(uint8_t layer_idx, uint8_t track)
{
    seq_lfo_t l = melodic_effective_lfo_idx(layer_idx, track, 1u);
    melodic_lfo2_apply_runtime(layer_idx, track, &l,
                               melodic_lfo2_open(layer_idx, track));
}

/* Unwire LFO2 from slot 0 (collapse on a family whose patch does not use the
 * slot; the reload that follows restores a Juno/DX7 patch LFO). */
void melodic_unwire_lfo2_track(uint8_t layer_idx, uint8_t track)
{
    melodic_lfo2_apply_runtime(layer_idx, track, NULL, true);
}

void sequencer_core_set_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    const seq_lfo_t *lfo)
{
    if (!lfo || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    voice_params_t *vp = seq_track_vp(layer_idx, track);

    vp->lfo = *lfo;
    if (vp->lfo.depth > 100) vp->lfo.depth = 100;
    vp->lfo_authored = true;
    /* One LFO per target (seq_model.h): LFO1 keeps a shared one. */
    uint8_t shared = vp->lfo2.targets & vp->lfo.targets;
    vp->lfo2.targets &= (uint8_t)~shared;

    if (shared) melodic_push_peers(layer_idx, track, melodic_reapply_lfo2_track);
    melodic_push_peers(layer_idx, track, melodic_reapply_lfo_track);
    ESP_LOGI(TAG, "LFO L%u T%u [%s] %s %.2f Hz d=%u tgt=0x%02x",
             layer_idx + 1u, track + 1u, melodic_vp_where(layer_idx, track),
             lfo->enabled ? "ON" : "OFF",
             (double)s_lfo_hz[layer_idx][track], lfo->depth, lfo->targets);
}

void sequencer_core_set_melodic_lfo2(uint8_t layer_idx, uint8_t track,
                                     const seq_lfo_t *lfo, bool open)
{
    if (!lfo || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    voice_params_t *vp = seq_track_vp(layer_idx, track);

    vp->lfo2 = *lfo;
    if (vp->lfo2.depth > 100) vp->lfo2.depth = 100;
    vp->lfo2.targets &= (uint8_t)~vp->lfo.targets;   /* one LFO per target */
    ESP_LOGI(TAG, "LFO2 L%u T%u [%s] %s %s d=%u tgt=0x%02x",
             layer_idx + 1u, track + 1u, melodic_vp_where(layer_idx, track),
             open ? "OPEN" : "COLLAPSED", lfo->enabled ? "ON" : "OFF",
             lfo->depth, vp->lfo2.targets);
    if (!open) {
        /* Collapse is release to patch; a collapsed LFO2 just keeps its
         * fields. */
        if (vp->lfo2_authored)
            sequencer_core_release_melodic_group(layer_idx, track, SEQ_VP_GROUP_LFO2);
        return;
    }
    vp->lfo2_authored = true;
    melodic_push_peers(layer_idx, track, melodic_reapply_lfo2_track);
}

/* Cancel-restore for the LFO live preview: re-push the stored (committed)
 * state. Safe on a never-authored row - the effective LFO is disabled with
 * an empty target set, so the restore is a no-op push. */
void sequencer_core_reapply_melodic_lfo(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    melodic_reapply_lfo_track(layer_idx, track);
}

void sequencer_core_reapply_melodic_lfo2(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    melodic_reapply_lfo2_track(layer_idx, track);
}

bool sequencer_core_get_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    seq_lfo_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = seq_track_vp(layer_idx, track)->lfo;
    return true;
}

bool sequencer_core_get_melodic_lfo2(uint8_t layer_idx, uint8_t track,
                                     seq_lfo_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = seq_track_vp(layer_idx, track)->lfo2;
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

#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO && !CONFIG_SEQ_LFO_SHARED
            /* Native tracks disarm the stepper (s_lfo_hz = 0 in
             * melodic_lfo_apply_runtime) and never reach this body; the guard
             * is belt-and-braces, since stepping a native track would
             * double-modulate. With CONFIG_SEQ_LFO_SHARED a wave row has no
             * carriers and the stepper is its fallback; bound rows keep
             * s_lfo_hz == 0. */
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
             * wavetable voice takes a shared LFO synth or, without
             * CONFIG_SEQ_LFO_SHARED, the native path; on a full bank it goes
             * unswept); DIST is handled after this block. */
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
                synth_level_set_sw_lfo(syn, 1.0f - d*(0.5f - 0.5f*val));
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

/* Post-apply LFO tail. With CONFIG_SEQ_LFO_SHARED: re-wire every row with a
 * live LFO and a target map to its shared LFO synth (the patch re-apply
 * cleared its mod slot 1), then LFO2 on slot 0. Without it: re-apply the
 * authored native LFO for every row whose patch reserves a carrier pair.
 * Skipped rows fall to the software loop. Both LFOs read through
 * melodic_effective_lfo_idx(), so a reload under an open preview keeps the
 * previewed LFO. Never re-applies a patch itself: the LFO2 collapse re-apply
 * is release to patch's own reload, which ends here. */
void sequencer_configure_melodic_lfo(uint8_t layer_idx, uint8_t rows)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
#if CONFIG_SEQ_LFO_SHARED
    /* Only melodic rows take the bank; a drum row never holds a reference. */
    if (layer->type != SEQ_LAYER_MELODIC) {
        melodic_lfo_bank_drop_layer(layer_idx);
        return;
    }
#endif
    for (uint8_t t = 0; t < rows; t++) {
#if CONFIG_SEQ_LFO_SHARED
        /* Per row: a mixed layer can have wave rows next to string rows. A
         * row without a live LFO1 takes the send-nothing
         * melodic_lfo_bank_drop(): the fresh re-apply carries no slot-1
         * wiring and no stepper residue, and a neutral restore would
         * overwrite the patch's own values. */
        uint16_t patch = layer->track_patch[t];
        bool mapped = lfo_targets_of_patch(patch, NULL);
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
        mapped = mapped || sequencer_core_lfo_wave_layout(patch, NULL);
#endif
        seq_lfo_t l = melodic_effective_lfo_idx(layer_idx, t, 0u);
        if (l.enabled && l.targets && mapped)
            melodic_lfo_apply_runtime(layer_idx, t, &l);
        else
            melodic_lfo_bank_drop(layer_idx, t);
        /* LFO2 after LFO1. Open: wired, or unwired while off (a Juno/DX7
         * patch LFO the re-apply just restored stays off while LFO2 is
         * open). Collapsed: nothing sent, so that patch LFO plays. */
        if (mapped && melodic_lfo2_open(layer_idx, t)) {
            seq_lfo_t l2 = melodic_effective_lfo_idx(layer_idx, t, 1u);
            melodic_lfo2_apply_runtime(layer_idx, t, &l2, true);
        } else {
            melodic_lfo2_bank_drop(layer_idx, t);
        }
#elif CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
        /* Per row: a mixed layer can have wave rows next to string rows, and
         * only the former reserve a carrier pair. */
        if (!sequencer_core_lfo_native_layout(layer->track_patch[t], NULL, NULL))
            continue;
        if (!seq_track_vp(layer_idx, t)->lfo_authored) continue;
        melodic_configure_native_lfo_track(layer_idx, t);
        /* Keep s_lfo_hz in sync so the service loop skips native tracks
         * (mirrors melodic_lfo_apply_runtime). */
        s_lfo_hz[layer_idx][t] = 0.0f;
#endif
    }
#if CONFIG_SEQ_LFO_SHARED
    for (uint8_t t = rows; t < SEQ_TRACKS; t++) {
        melodic_lfo_bank_drop(layer_idx, t);
        melodic_lfo2_bank_drop(layer_idx, t);
    }
#endif
    (void)layer;
    (void)rows;
}

/* Update the carrier frequency on all active native-LFO tracks after a BPM
 * change. Mirrors arp_core_refresh_lfo_freq(). */
void melodic_lfo_refresh_native_freq(void)
{
    /* With CONFIG_SEQ_LFO_SHARED there are no per-voice carriers: a bound
     * row's shared synth is retuned by lfo_bank_set_bpm(), a stepper row by
     * sequencer_core_set_bpm(). */
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO && !CONFIG_SEQ_LFO_SHARED
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
            /* Carrier index is per track under unison (the stereo flag moves
             * it). */
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

void sequencer_core_preview_melodic_lfo2(uint8_t layer_idx, uint8_t track,
                                         const seq_lfo_t *lfo)
{
    if (!lfo || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_lfo_t tmp = *lfo;
    if (tmp.depth > 100) tmp.depth = 100;

    s_preview_gen++;
    preview_slot_touch(layer_idx, track);
    s_preview.lfo2       = tmp;
    s_preview.lfo2_valid = true;
    s_preview_gen++;

    /* LFO2 has no stepper: the shared LFO synth takes the scratch at once.
     * The LFO2 page exists only while LFO2 is open. */
    melodic_lfo2_apply_runtime(layer_idx, track, &tmp, true);
}
