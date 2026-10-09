/* Project snapshot serializer + loader orchestration.
 *
 * Field order IS the format: every ser_ writer and its de_/parse_ reader must
 * walk fields in the same sequence or saved projects corrupt on load. The
 * shared env/filter/lfo helpers keep LAYR's voice_params_t and the ARP/DRON
 * sections from drifting apart.
 *
 * Two-phase load: parse_* stages into heap scratch, never touching live state;
 * apply runs only after every section parses clean. Any failure frees scratch
 * and returns false with zero live-state changes.
 */

#include "project/project_snapshot.h"
#include "project_store.h"
#include "project_tlv.h"
#include "project_fs.h"

#include "sequencer_core.h"
#include "seq_core_config.h"   /* SEQ_SWING_MAX - same-component engine limits */
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"
#include "custompatches/clip_bounce.h"
#include "custompatches/clip_player.h"
#include "amy_fx.h"
#include "amy.h"               /* amy_num_algorithms - fm_algo_override clamp */
#include "quantizer.h"
#include "voice_config.h"
#include "seq_defaults.h"       /* melodic env defaults for the v14 layer block */
#include "synth_ui/synth_ui_internal.h"   /* synth_ui_reload_mirror_from_core() */
#include "sdkconfig.h"
#if CONFIG_SYNTH_CUSTOM_WT
#include "custompatches/wt_builder.h"
#endif

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "seq_clamp.h"
#include <string.h>

static const char *TAG = "project_snapshot";

/* Section tags (u32, ASCII little-endian). */
#define TAG_GLOB 0x424F4C47u
#define TAG_LAYR 0x5259414Cu
#define TAG_ARP  0x20505241u
#define TAG_DRON 0x4E4F5244u
#define TAG_DSTD 0x44545344u
#define TAG_PROG 0x474F5250u
#define TAG_CHRD 0x44524843u
#define TAG_CLIP 0x50494C43u
#define TAG_PGEN 0x4E454750u
#define TAG_WTCU 0x55435457u

#define PROJECT_SER_BUF_CAP (64 * 1024)

/* LAYR section version: the writer's tag and the loader's only accepted
 * value. */
#define LAYR_VERSION 25

_Static_assert(SEQ_TRACKS == 5 && SEQ_MAX_STEPS == 32,
               "LAYR format assumes 5x32; bump LAYR_VERSION");

/* ── Shared env/filter/lfo sub-block codecs ──────────────────────────────
 * Used by voice_params_t (LAYR, per track) and directly by ARP/DRON, so the
 * three engines' persisted shapes cannot drift apart. */

static void ser_env(tlv_writer_t *w, const seq_env_t *e)
{
    tlv_put_u32(w, e->attack_ms);
    tlv_put_u32(w, e->decay_ms);
    tlv_put_u8(w,  e->sustain_pct);
    tlv_put_u32(w, e->release_ms);
    tlv_put_u8(w,  e->eg_type);
}

static bool de_env(tlv_reader_t *r, seq_env_t *e)
{
    if (!tlv_get_u32(r, &e->attack_ms))  return false;
    if (!tlv_get_u32(r, &e->decay_ms))   return false;
    if (!tlv_get_u8(r, &e->sustain_pct)) return false;
    if (e->sustain_pct > 100) e->sustain_pct = 100;
    if (!tlv_get_u32(r, &e->release_ms)) return false;
    if (!tlv_get_u8(r, &e->eg_type))     return false;
    return true;
}

static void ser_filter(tlv_writer_t *w, const seq_filter_t *f)
{
    tlv_put_u8(w,  f->filter_type);
    tlv_put_f32(w, f->cutoff_hz);
    tlv_put_f32(w, f->resonance);
    tlv_put_u8(w,  f->enabled ? 1 : 0);
    tlv_put_f32(w, f->feedback);   /* KS string decay */
    /* The envelope routing matrix, [eg][target]. */
    for (int eg = 0; eg < 2; eg++)
        for (int t = 0; t < SEQ_EGT_COUNT; t++)
            tlv_put_f32(w, f->eg_depth[eg][t]);
    tlv_put_f32(w, f->ks_duty_ofs);   /* KS pluck duty - 0.5 */
    tlv_put_f32(w, f->key_track);     /* filter key tracking, oct/oct */
}

/* Reads every field first (keeps the reader position correct), then bypasses
 * the whole sub-block if filter_type is out of range rather than passing a
 * bogus enum downstream. One fixed shape: the sections that carry this block
 * accept exactly one version. */
static bool de_filter(tlv_reader_t *r, seq_filter_t *f)
{
    uint8_t ft, en;
    float cutoff, resonance, feedback, duty_ofs, key_track;
    float depth[2][SEQ_EGT_COUNT];
    if (!tlv_get_u8(r, &ft))          return false;
    if (!tlv_get_f32(r, &cutoff))     return false;
    if (!tlv_get_f32(r, &resonance))  return false;
    if (!tlv_get_u8(r, &en))          return false;
    if (!tlv_get_f32(r, &feedback))   return false;
    feedback = SEQ_CLAMP_F32(feedback, 0.0f, 1.0f);
    for (int eg = 0; eg < 2; eg++) {
        for (int t = 0; t < SEQ_EGT_COUNT; t++) {
            if (!tlv_get_f32(r, &depth[eg][t])) return false;
            depth[eg][t] = SEQ_CLAMP_F32(depth[eg][t],
                                         -seq_eg_depth_max((seq_eg_target_t)t),
                                         seq_eg_depth_max((seq_eg_target_t)t));
        }
    }
    if (!tlv_get_f32(r, &duty_ofs))   return false;
    duty_ofs = SEQ_CLAMP_F32(duty_ofs, -0.5f, 0.5f);
    if (!tlv_get_f32(r, &key_track))  return false;
    key_track = SEQ_CLAMP_F32(key_track, 0.0f, 1.0f);

    if (ft >= SEQ_FILTER_COUNT) {
        *f = (seq_filter_t){0};
        return true;
    }
    f->filter_type       = ft;
    f->cutoff_hz         = cutoff;
    f->resonance         = resonance;
    f->enabled           = en != 0;
    f->feedback          = feedback;
    memcpy(f->eg_depth, depth, sizeof(f->eg_depth));
    f->ks_duty_ofs       = duty_ofs;
    f->key_track         = key_track;
    return true;
}

/* ── Distortion codec ─────────────────────────────────────────────────────
 * Values are re-clamped on read: a truncated or hand-edited body must not
 * push an out-of-range type into AMY. */
static void ser_dist(tlv_writer_t *w, const seq_dist_t *d)
{
    tlv_put_u8(w, d->type);
    tlv_put_u8(w, d->drive);
    tlv_put_u8(w, d->bits);
    tlv_put_u8(w, d->rate);
    tlv_put_u8(w, d->mix);
}

static bool de_dist(tlv_reader_t *r, seq_dist_t *d)
{
    uint8_t type, drive, bits, rate, mix;
    if (!tlv_get_u8(r, &type))  return false;
    if (!tlv_get_u8(r, &drive)) return false;
    if (!tlv_get_u8(r, &bits))  return false;
    if (!tlv_get_u8(r, &rate))  return false;
    if (!tlv_get_u8(r, &mix))   return false;
    d->type = type; d->drive = drive; d->bits = bits; d->rate = rate; d->mix = mix;
    voice_dist_clamp(d);
    return true;
}

static void ser_lfo(tlv_writer_t *w, const seq_lfo_t *l)
{
    tlv_put_u8(w, l->enabled ? 1 : 0);
    tlv_put_u8(w, (uint8_t)l->mode);
    tlv_put_u8(w, (uint8_t)l->wave);
    tlv_put_u8(w, (uint8_t)l->rate);
    tlv_put_u8(w, l->depth);
    tlv_put_u8(w, l->targets);   /* target-set bitmask */
    tlv_put_u8(w, l->wob_rate);  /* WOBBLE second-order LFO */
    tlv_put_u8(w, l->wob_depth);
    tlv_put_u8(w, l->wob_reach);  /* reach: 0 both, 1 depth, 2 rate */
    tlv_put_u8(w, l->flt_oct_q);  /* FILTER swing in quarter-octaves; 0 =
                                   * depth-derived */
}

/* Same "read everything, then validate" shape as de_filter. */
static bool de_lfo(tlv_reader_t *r, seq_lfo_t *l)
{
    uint8_t en, mode, wave, rate, depth, tgt, wrate, wdepth, wdeponly, foct;
    if (!tlv_get_u8(r, &en))       return false;
    if (!tlv_get_u8(r, &mode))     return false;
    if (!tlv_get_u8(r, &wave))     return false;
    if (!tlv_get_u8(r, &rate))     return false;
    if (!tlv_get_u8(r, &depth))    return false;
    if (!tlv_get_u8(r, &tgt))      return false;
    if (!tlv_get_u8(r, &wrate))    return false;
    if (!tlv_get_u8(r, &wdepth))   return false;
    if (!tlv_get_u8(r, &wdeponly)) return false;
    if (!tlv_get_u8(r, &foct))     return false;

    if (mode > LFO_MODE_RETRIG || wave >= LFO_WAVE_COUNT) {
        *l = (seq_lfo_t){ .rate = NOTE_DIV_1_8, .wob_rate = NOTE_DIV_1_8 };
        return true;
    }
    l->enabled = en != 0;
    l->mode    = (lfo_mode_t)mode;
    l->wave    = (lfo_wave_t)wave;
    l->rate    = (rate < NOTE_DIV_COUNT) ? (note_div_t)rate : NOTE_DIV_1_8;
    l->depth   = (depth > 100) ? 100 : depth;
    l->targets = (uint8_t)(tgt & LFO_TGT_ALL);
    l->wob_rate  = (wrate < NOTE_DIV_COUNT) ? wrate : (uint8_t)NOTE_DIV_1_8;
    /* Snap to a whole-dB authoring step (voice_config.h), so an off-grid byte
     * cannot read as OFF while the modulator still runs. */
    l->wob_depth = voice_wob_db_to_depth(voice_wob_depth_to_db(wdepth));
    l->wob_reach = (wdeponly < WOB_REACH_COUNT) ? wdeponly : 0;
    l->flt_oct_q = (foct > VOICE_LFO_FLT_OCT_Q_MAX)
                   ? (uint8_t)VOICE_LFO_FLT_OCT_Q_MAX : foct;
    return true;
}

/* ── Shared voice_params_t codec (LAYR per track, DRON, DSTD) ───────────── */

static void ser_vp(tlv_writer_t *w, const voice_params_t *vp)
{
    ser_env(w, &vp->env);
    ser_env(w, &vp->env1);
    ser_filter(w, &vp->filter);
    ser_lfo(w, &vp->lfo);
    tlv_put_u8(w, vp->env_authored ? 1 : 0);
    tlv_put_u8(w, vp->env1_authored ? 1 : 0);
    tlv_put_u8(w, vp->filter_authored ? 1 : 0);
    tlv_put_u8(w, vp->lfo_authored ? 1 : 0);
    tlv_put_f32(w, vp->amp_trim);
    ser_dist(w, &vp->dist);
    tlv_put_u8(w, vp->dist_authored ? 1 : 0);
}

static bool de_vp(tlv_reader_t *r, voice_params_t *vp)
{
    voice_params_init_defaults(vp);   /* zeroed baseline + unity amp_trim */
    if (!de_env(r, &vp->env))       return false;
    if (!de_env(r, &vp->env1))      return false;
    if (!de_filter(r, &vp->filter)) return false;
    if (!de_lfo(r, &vp->lfo))       return false;
    uint8_t ea, e1a, fa, la;
    if (!tlv_get_u8(r, &ea))  return false;
    if (!tlv_get_u8(r, &e1a)) return false;
    if (!tlv_get_u8(r, &fa))  return false;
    if (!tlv_get_u8(r, &la))  return false;
    vp->env_authored    = ea  != 0;
    vp->env1_authored   = e1a != 0;
    vp->filter_authored = fa  != 0;
    vp->lfo_authored    = la  != 0;
    if (!tlv_get_f32(r, &vp->amp_trim)) return false;
    vp->amp_trim = SEQ_CLAMP_F32(vp->amp_trim, 0.0f, 1.0f);
    if (!de_dist(r, &vp->dist)) return false;
    uint8_t da;
    if (!tlv_get_u8(r, &da)) return false;
    vp->dist_authored = da != 0;
    return true;
}

/* ── GLOB section ─────────────────────────────────────────────────────────
 * Tail tolerance WITHIN the current version: a shorter GLOB body just runs out
 * of bytes and fields past that keep their seeded live-state value, so a new
 * tail field costs no reader work. tlv_reader_t.err sticks on the first short
 * read, so a failed tlv_get_* here means "no more data", not corruption.
 * Older section versions are rejected outright (no migration) - the dispatcher
 * checks the version before this runs. */

typedef struct {
    uint16_t bpm;
    float    master_volume;
    bool     quant_enabled;
    uint8_t  quant_root;
    uint8_t  quant_scale;
    seq_drum_engine_t drum_engine;
    fx_state_t fx[FX_BUS_COUNT];
    bool     presets_alter_global;
    uint8_t  split_flags;   /* bit fx_group_t: that group renders on its own bus */
} staged_glob_t;

static void ser_glob(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_GLOB, 6);  /* v6: echo note is a note_div_t */
    tlv_put_u16(w, sequencer_core_get_bpm());
    tlv_put_f32(w, amy_fx_get_master_volume());
    tlv_put_u8(w, sequencer_core_get_quantizer_enabled() ? 1 : 0);
    tlv_put_u8(w, sequencer_core_get_quantizer_root_note());
    tlv_put_u8(w, sequencer_core_get_quantizer_scale());
    tlv_put_u8(w, (uint8_t)sequencer_core_get_drum_engine());
    tlv_put_u8(w, s_fx_presets_alter_global ? 1 : 0);
    {
        uint8_t flags = 0;
        for (fx_group_t g = FX_GROUP_DRUMS; g < FX_GROUP_COUNT; g++) {
            if (fx_group_is_split(g)) flags |= (uint8_t)(1u << g);
        }
        tlv_put_u8(w, flags);
    }
    for (uint8_t bus = 0; bus < FX_BUS_COUNT; bus++) {
        const fx_state_t *f = &s_fx[bus];
        tlv_put_i8(w,  f->eq_low_db);
        tlv_put_i8(w,  f->eq_mid_db);
        tlv_put_i8(w,  f->eq_high_db);
        tlv_put_u8(w,  f->echo_level);
        tlv_put_u8(w,  f->chorus_level);
        tlv_put_u8(w,  f->reverb_level);
        tlv_put_i16(w, f->echo_delay_ms);
        tlv_put_i16(w, f->echo_feedback);
        tlv_put_i16(w, f->echo_tone);
        tlv_put_u8(w,  f->echo_sync ? 1 : 0);
        tlv_put_u8(w,  f->echo_div);
        tlv_put_i16(w, f->reverb_liveness);
        tlv_put_i16(w, f->reverb_damping);
        tlv_put_i16(w, f->reverb_xover_hz);
        tlv_put_i16(w, f->chorus_rate);
        tlv_put_i16(w, f->chorus_depth);
        tlv_put_u8(w,  f->bus_dist_type);
        tlv_put_u8(w,  f->bus_dist_drive);
        tlv_put_u8(w,  f->bus_dist_bits);
        tlv_put_u8(w,  f->bus_dist_rate);
        tlv_put_u8(w,  f->bus_dist_mix);
        tlv_put_u8(w,  f->level);
        tlv_put_i16(w, f->chorus_delay);
    }
    tlv_end_section(w, h);
}

static bool parse_glob(tlv_reader_t *b, staged_glob_t *g)
{
    /* Seed from live state so a short section leaves the tail as-is. */
    g->bpm           = sequencer_core_get_bpm();
    g->master_volume = amy_fx_get_master_volume();
    g->quant_enabled = sequencer_core_get_quantizer_enabled();
    g->quant_root    = sequencer_core_get_quantizer_root_note();
    g->quant_scale   = sequencer_core_get_quantizer_scale();
    g->drum_engine   = sequencer_core_get_drum_engine();
    memcpy(g->fx, s_fx, sizeof g->fx);
    g->presets_alter_global = s_fx_presets_alter_global;
    g->split_flags = 0;
    for (fx_group_t grp = FX_GROUP_DRUMS; grp < FX_GROUP_COUNT; grp++) {
        if (fx_group_is_split(grp)) g->split_flags |= (uint8_t)(1u << grp);
    }

    uint8_t v;
    if (!tlv_get_u16(b, &g->bpm))           return true;
    if (!tlv_get_f32(b, &g->master_volume)) return true;
    if (!tlv_get_u8(b, &v))                 return true;
    g->quant_enabled = v != 0;
    if (!tlv_get_u8(b, &g->quant_root))     return true;
    if (!tlv_get_u8(b, &g->quant_scale))    return true;
    if (!tlv_get_u8(b, &v))                 return true;
    g->drum_engine = (v == SEQ_DRUM_PCM) ? SEQ_DRUM_PCM : SEQ_DRUM_SYNTH;
    if (!tlv_get_u8(b, &v))                 return true;
    g->presets_alter_global = v != 0;
    if (!tlv_get_u8(b, &g->split_flags))    return true;
    for (uint8_t bus = 0; bus < FX_BUS_COUNT; bus++) {
        fx_state_t *f = &g->fx[bus];
        if (!tlv_get_i8(b, &f->eq_low_db))        return true;
        if (!tlv_get_i8(b, &f->eq_mid_db))        return true;
        if (!tlv_get_i8(b, &f->eq_high_db))       return true;
        if (!tlv_get_u8(b, &f->echo_level))       return true;
        if (!tlv_get_u8(b, &f->chorus_level))     return true;
        if (!tlv_get_u8(b, &f->reverb_level))     return true;
        if (!tlv_get_i16(b, &f->echo_delay_ms))   return true;
        if (!tlv_get_i16(b, &f->echo_feedback))   return true;
        if (!tlv_get_i16(b, &f->echo_tone))       return true;
        if (!tlv_get_u8(b, &v))                   return true;
        f->echo_sync = v != 0;
        if (!tlv_get_u8(b, &f->echo_div))         return true;
        if (f->echo_div >= NOTE_DIV_COUNT) f->echo_div = NOTE_DIV_1_8D;
        if (!tlv_get_i16(b, &f->reverb_liveness)) return true;
        if (!tlv_get_i16(b, &f->reverb_damping))  return true;
        if (!tlv_get_i16(b, &f->reverb_xover_hz)) return true;
        if (!tlv_get_i16(b, &f->chorus_rate))     return true;
        if (!tlv_get_i16(b, &f->chorus_depth))    return true;
        if (!tlv_get_u8(b, &f->bus_dist_type))    return true;
        f->bus_dist_type &= 7;  /* stray data reads as a valid stage mask */
        if (!tlv_get_u8(b, &f->bus_dist_drive))   return true;
        if (!tlv_get_u8(b, &f->bus_dist_bits))    return true;
        if (!tlv_get_u8(b, &f->bus_dist_rate))    return true;
        if (!tlv_get_u8(b, &f->bus_dist_mix))     return true;
        if (!tlv_get_u8(b, &f->level))            return true;
        if (f->level > 200) f->level = 200;
        if (!tlv_get_i16(b, &f->chorus_delay))    return true;
        if (f->chorus_delay != FX_PARAM_UNSET)
            f->chorus_delay = (int16_t)SEQ_CLAMP_INT(f->chorus_delay, 16, 512);
    }
    return true;
}

static void apply_glob(const staged_glob_t *g)
{
    sequencer_core_set_bpm(g->bpm);
    sequencer_core_set_quantizer_enabled(g->quant_enabled);
    sequencer_core_set_quantizer_root_note(g->quant_root);
    sequencer_core_set_quantizer_scale(g->quant_scale);
    sequencer_core_set_drum_engine(g->drum_engine);
    s_fx_presets_alter_global = g->presets_alter_global;
    for (fx_group_t grp = FX_GROUP_DRUMS; grp < FX_GROUP_COUNT; grp++) {
        fx_bus_set_split(grp, (g->split_flags & (1u << grp)) != 0);
    }
    memcpy(s_fx, g->fx, sizeof s_fx);
    /* Volume last of the caches: the per-bus levels it multiplies are already
     * in place, so one call lands the right product on every bus. */
    amy_fx_set_master_volume(g->master_volume);
    /* Re-tags the movable groups onto the buses the flags just named and
     * pushes every bus's FX - the muting rule keeps unsplit buses silent. */
    amy_fx_apply_routing();
}

/* ── LAYR section ─────────────────────────────────────────────────────────
 * Strict: any truncated read rejects the whole file - unlike GLOB, a partial
 * LAYR body cannot be defaulted field-by-field once step arrays are involved.
 * Out-of-range values clamp rather than reject, except the layer-count/type/
 * first-layer-is-drum invariants the caller enforces after all LAYR sections
 * have parsed. */

static void ser_layer(tlv_writer_t *w, const seq_layer_t *L)
{
    /* One fixed shape per version; the loader rejects any other version
     * (no migration), so a format change is a bump here and in
     * project_snapshot_load. Field history: git log. */
    size_t h = tlv_begin_section(w, TAG_LAYR, LAYR_VERSION);
    tlv_put_u8(w, (uint8_t)L->type);
    tlv_put_u8(w, L->num_steps);
    tlv_put_u16(w, L->patch);
    tlv_put_u32(w, L->synth_flags);
    tlv_put_u8(w, L->num_voices);
    tlv_put_u8(w, L->chord_mode ? 1 : 0);
    tlv_put_u8(w, L->chord_root);
    tlv_put_u8(w, (uint8_t)L->chord_type);
    tlv_put_u8(w, L->swing_pct);
    for (int t = 0; t < SEQ_TRACKS; t++) {
        tlv_put_u8(w, L->track_base_note[t]);
        tlv_put_u16(w, L->track_patch[t]);
        tlv_put_u16(w, L->track_pcm_preset[t]);
        tlv_put_u8(w, L->track_pcm_mode[t]);
        tlv_put_u8(w, L->track_unison[t].count);
        tlv_put_u8(w, L->track_unison[t].detune_cents);
        tlv_put_u8(w, L->track_unison[t].spread_pct);
        tlv_put_u8(w, L->track_unison[t].blend_pct);
        tlv_put_u8(w, L->repeat_rate[t]);
        tlv_put_u8(w, L->mute[t] ? 1 : 0);
        tlv_put_u8(w, L->solo[t] ? 1 : 0);
        tlv_put_u8(w, L->follow[t]);
        tlv_put_u8(w, L->wt_frame[t]);
        ser_vp(w, &L->vp[t]);
    }
    tlv_put_bytes(w, L->grid,               sizeof L->grid);
    tlv_put_bytes(w, L->step_note,          sizeof L->step_note);
    tlv_put_bytes(w, L->step_pitch_ofs,     sizeof L->step_pitch_ofs);
    tlv_put_bytes(w, L->step_prob,          sizeof L->step_prob);
    tlv_put_bytes(w, L->step_ratchet,       sizeof L->step_ratchet);
    tlv_put_bytes(w, L->step_every,         sizeof L->step_every);
    tlv_put_bytes(w, L->step_prev,          sizeof L->step_prev);
    tlv_put_bytes(w, L->step_transform,     sizeof L->step_transform);
    tlv_put_bytes(w, L->step_quant_bypass,  sizeof L->step_quant_bypass);
    tlv_put_bytes(w, L->step_nudge,         sizeof L->step_nudge);
    tlv_put_bytes(w, L->step_frame,         sizeof L->step_frame);
    tlv_put_bytes(w, L->step_velocity_adj,  sizeof L->step_velocity_adj);
    tlv_put_bytes(w, L->step_ratchet_taper, sizeof L->step_ratchet_taper);
    /* Note FX: gate length (drum and melodic), glide, GROOVE accent amount. */
    tlv_put_u16(w, L->gate_pct);
    tlv_put_u16(w, L->portamento_ms);
    tlv_put_u8(w, L->groove_pct);
    /* Live FM algorithm override (Shift+Turn). */
    tlv_put_u8(w, L->fm_algo_override);
    /* Voice-block source selector per row + the layer's shared block. */
    for (int t = 0; t < SEQ_TRACKS; t++) tlv_put_u8(w, L->vp_src[t]);
    ser_vp(w, &L->vp_layer);
    /* Melodic patch scope. track_patch[] above already carries the per-row
     * patches in both scopes, so this one byte is all the scope needs. */
    tlv_put_u8(w, L->patch_scope);
    /* Live row count; rows above it are written and read but not built. */
    tlv_put_u8(w, L->num_tracks);
    tlv_end_section(w, h);
}

static uint16_t clamp_patch(uint16_t patch)
{
    if (patch > SEQ_PATCH_FULL_MAX) return 0;
    if (sequencer_core_patch_compiled_out(patch)) return 0;
    return patch;
}

static bool parse_layer(tlv_reader_t *b, seq_layer_t *L)
{
    memset(L, 0, sizeof *L);

    uint8_t type_raw;
    if (!tlv_get_u8(b, &type_raw)) return false;
    L->type = (type_raw > SEQ_LAYER_MELODIC) ? SEQ_LAYER_MELODIC
                                              : (seq_layer_type_t)type_raw;

    if (!tlv_get_u8(b, &L->num_steps)) return false;
    if (L->num_steps != 16 && L->num_steps != 32) L->num_steps = 16;

    if (!tlv_get_u16(b, &L->patch)) return false;
    L->patch = clamp_patch(L->patch);

    if (!tlv_get_u32(b, &L->synth_flags)) return false;
    if (!tlv_get_u8(b, &L->num_voices))   return false;
    /* AMY's instrument_init() aborts outside 1..MAX_VOICES_PER_INSTRUMENT
     * (32); a CRC-valid but hand-edited file must not crash the load. */
    L->num_voices = SEQ_CLAMP_U8(L->num_voices, 1, 32);
    { uint8_t v; if (!tlv_get_u8(b, &v)) return false; L->chord_mode = v != 0; }
    if (!tlv_get_u8(b, &L->chord_root)) return false;
    { uint8_t v; if (!tlv_get_u8(b, &v)) return false;
      L->chord_type = (v >= CHORD_TYPE_COUNT) ? CHORD_MAJ : (chord_type_t)v; }
    if (!tlv_get_u8(b, &L->swing_pct)) return false;
    /* Bulk import bypasses sequencer_core_set_layer_swing(), so enforce the
     * engine ceiling here: swing-offset math requires the delay to stay
     * short of one full step. */
    if (L->swing_pct > SEQ_SWING_MAX) L->swing_pct = SEQ_SWING_MAX;

    for (int t = 0; t < SEQ_TRACKS; t++) {
        if (!tlv_get_u8(b, &L->track_base_note[t]))  return false;
        if (!tlv_get_u16(b, &L->track_patch[t]))      return false;
        L->track_patch[t] = clamp_patch(L->track_patch[t]);
        if (!tlv_get_u16(b, &L->track_pcm_preset[t])) return false;
        if (!tlv_get_u8(b, &L->track_pcm_mode[t]))    return false;
        /* Unison raw fields; count 0 (never set) survives the round trip. */
        voice_unison_t *u = &L->track_unison[t];
        if (!tlv_get_u8(b, &u->count))        return false;
        if (!tlv_get_u8(b, &u->detune_cents)) return false;
        if (!tlv_get_u8(b, &u->spread_pct))   return false;
        if (!tlv_get_u8(b, &u->blend_pct))    return false;
        u->count        = SEQ_CLAMP_U8(u->count, 0u, VOICE_UNISON_MAX_COPIES);
        u->detune_cents = SEQ_CLAMP_U8(u->detune_cents, 0u, VOICE_UNISON_MAX_DETUNE);
        u->spread_pct   = SEQ_CLAMP_U8(u->spread_pct, 0u, 100u);
        u->blend_pct    = SEQ_CLAMP_U8(u->blend_pct, 0u, 100u);
        { uint8_t v; if (!tlv_get_u8(b, &v)) return false; L->repeat_rate[t] = v; }
        { uint8_t v; if (!tlv_get_u8(b, &v)) return false; L->mute[t] = v != 0; }
        { uint8_t v; if (!tlv_get_u8(b, &v)) return false; L->solo[t] = v != 0; }
        { uint8_t v; if (!tlv_get_u8(b, &v)) return false;
          L->follow[t] = (v >= SEQ_FOLLOW_COUNT) ? (uint8_t)SEQ_FOLLOW_CHORD : v; }
        { uint8_t v; if (!tlv_get_u8(b, &v)) return false;
          L->wt_frame[t] = (v > 64u) ? 0u : v; }
        if (!de_vp(b, &L->vp[t])) return false;
    }

    if (!tlv_get_bytes(b, L->grid,               sizeof L->grid))               return false;
    if (!tlv_get_bytes(b, L->step_note,          sizeof L->step_note))          return false;
    if (!tlv_get_bytes(b, L->step_pitch_ofs,     sizeof L->step_pitch_ofs))     return false;
    if (!tlv_get_bytes(b, L->step_prob,          sizeof L->step_prob))          return false;
    if (!tlv_get_bytes(b, L->step_ratchet,       sizeof L->step_ratchet))       return false;
    if (!tlv_get_bytes(b, L->step_every,         sizeof L->step_every))         return false;
    if (!tlv_get_bytes(b, L->step_prev,          sizeof L->step_prev))          return false;
    if (!tlv_get_bytes(b, L->step_transform,     sizeof L->step_transform))     return false;
    if (!tlv_get_bytes(b, L->step_quant_bypass,  sizeof L->step_quant_bypass))  return false;
    if (!tlv_get_bytes(b, L->step_nudge,         sizeof L->step_nudge))         return false;
    if (!tlv_get_bytes(b, L->step_frame,         sizeof L->step_frame))         return false;
    if (!tlv_get_bytes(b, L->step_velocity_adj,  sizeof L->step_velocity_adj))  return false;
    if (!tlv_get_bytes(b, L->step_ratchet_taper, sizeof L->step_ratchet_taper)) return false;

    for (int t = 0; t < SEQ_TRACKS; t++) {
        for (int s = 0; s < SEQ_MAX_STEPS; s++) {
            L->grid[t][s]              = L->grid[t][s] ? 1 : 0;
            L->step_quant_bypass[t][s] = L->step_quant_bypass[t][s] ? 1 : 0;
            if (L->step_prob[t][s] > 100) L->step_prob[t][s] = 100;
            L->step_ratchet[t][s] = SEQ_CLAMP_U8(L->step_ratchet[t][s], 1, SEQ_MAX_RATCHET);
            L->step_every[t][s] = SEQ_CLAMP_U8(L->step_every[t][s], 1, SEQ_STEP_EVERY_MAX);
            L->step_prev[t][s]  = L->step_prev[t][s] ? 1 : 0;
            if (L->step_transform[t][s] >= SEQ_STEP_TRANSFORM_COUNT) L->step_transform[t][s] = SEQ_STEP_TRANSFORM_NONE;
            if (L->step_frame[t][s] > 64u) L->step_frame[t][s] = 0u;
        }
    }

    /* Note fields hold a playable pitch or (melodic only) a chord preset
     * sentinel (seq_chords.h). Anything else - corrupt, or a sentinel family
     * this build doesn't know - clamps into the playable range so it cannot
     * leak into pitch math. */
    for (int t = 0; t < SEQ_TRACKS; t++) {
        bool mel = L->type == SEQ_LAYER_MELODIC;
        if (!(mel && SEQ_NOTE_IS_CHORD(L->track_base_note[t]))) {
            L->track_base_note[t] = SEQ_CLAMP_U8(L->track_base_note[t],
                                                 SEQ_MEL_NOTE_MIN, SEQ_MEL_NOTE_MAX);
        }
        for (int s = 0; s < SEQ_MAX_STEPS; s++) {
            if (!(mel && SEQ_NOTE_IS_CHORD(L->step_note[t][s]))) {
                L->step_note[t][s] = SEQ_CLAMP_U8(L->step_note[t][s],
                                                  SEQ_MEL_NOTE_MIN, SEQ_MEL_NOTE_MAX);
            }
        }
    }

    /* Note FX, clamped to the live control ranges. */
    if (!tlv_get_u16(b, &L->gate_pct))      return false;
    if (!tlv_get_u16(b, &L->portamento_ms)) return false;
    if (L->gate_pct != SEQ_GATE_HOLD)
        L->gate_pct = SEQ_CLAMP_U16(L->gate_pct, SEQ_GATE_PCT_MIN, SEQ_GATE_PCT_MAX);
    if (L->portamento_ms > SEQ_MELODIC_PORTAMENTO_MAX_MS)
        L->portamento_ms = SEQ_MELODIC_PORTAMENTO_MAX_MS;

    if (!tlv_get_u8(b, &L->groove_pct)) return false;
    if (L->groove_pct > 100) L->groove_pct = 100;

    /* Live FM algorithm override; corrupt values fall back to none. The
     * configure path reasserts it after the load's patch apply; anything past
     * AMY's algorithm table would be an unchecked OOB index there. */
    if (!tlv_get_u8(b, &L->fm_algo_override)) return false;
    if (L->fm_algo_override != SEQ_FM_ALGO_NONE &&
        L->fm_algo_override >= amy_num_algorithms)
        L->fm_algo_override = SEQ_FM_ALGO_NONE;

    /* Per-row voice-block source + the layer's shared block. The layer block
     * is only meaningful on melodic layers; drum rows are forced to TRACK so
     * the bank seeding never resolves onto the shared block. */
    for (int t = 0; t < SEQ_TRACKS; t++) {
        if (!tlv_get_u8(b, &L->vp_src[t])) return false;
        if (L->vp_src[t] > SEQ_VP_SRC_LAYER || L->type != SEQ_LAYER_MELODIC)
            L->vp_src[t] = SEQ_VP_SRC_TRACK;
    }
    if (!de_vp(b, &L->vp_layer)) return false;

    /* Melodic patch scope. */
    if (!tlv_get_u8(b, &L->patch_scope)) return false;
    if (L->patch_scope > SEQ_PATCH_SCOPE_TRACK)
        L->patch_scope = SEQ_PATCH_SCOPE_LAYER;
    if (L->type != SEQ_LAYER_MELODIC) L->patch_scope = SEQ_PATCH_SCOPE_LAYER;
    /* Invariant repair: in LAYER scope every melodic row plays `patch`, and
     * the configure path reads track_patch[] only. Cheap enough to do
     * unconditionally rather than trust the file. */
    if (L->type == SEQ_LAYER_MELODIC && L->patch_scope == SEQ_PATCH_SCOPE_LAYER) {
        for (int t = 0; t < SEQ_TRACKS; t++) L->track_patch[t] = L->patch;
    }

    if (!tlv_get_u8(b, &L->num_tracks)) return false;
    L->num_tracks = SEQ_CLAMP_U8(L->num_tracks, SEQ_TRACKS_DEFAULT, SEQ_TRACKS);

    return true;
}

/* ── ARP section ──────────────────────────────────────────────────────────── */

typedef struct {
    bool         enabled;
    uint16_t     patch;
    arp_dir_t    dir;
    uint8_t      octaves;
    note_div_t   rate;
    uint8_t      gate_pct;
    uint8_t      scale;
    uint8_t      root;
    uint8_t      quant_mode;
    uint16_t     portamento_ms;
    float        amp_scale;
    int16_t      slots[ARP_MAX_SLOTS];
    seq_env_t    env, env2;
    seq_filter_t filter;
    seq_lfo_t    lfo;
    seq_dist_t   dist;
} staged_arp_t;

static void ser_arp(tlv_writer_t *w)
{
    /* One fixed shape per version, as for LAYR. Field history: git log. */
    size_t h = tlv_begin_section(w, TAG_ARP, 14);
    tlv_put_u8(w, arp_get_enabled() ? 1 : 0);
    tlv_put_u16(w, arp_get_patch());
    tlv_put_u8(w, (uint8_t)arp_get_direction());
    tlv_put_u8(w, arp_get_octaves());
    tlv_put_u8(w, (uint8_t)arp_get_rate());
    tlv_put_u8(w, arp_get_gate_pct());
    tlv_put_u8(w, arp_get_scale());
    tlv_put_u8(w, arp_get_root_note());
    tlv_put_u16(w, arp_get_portamento_ms());
    tlv_put_f32(w, arp_get_amp_scale());
    for (int i = 0; i < ARP_MAX_SLOTS; i++) tlv_put_i16(w, arp_get_slot((uint8_t)i));
    seq_env_t e; arp_get_envelope(&e);   ser_env(w, &e);
    seq_env_t e2; arp_get_envelope2(&e2); ser_env(w, &e2);
    seq_filter_t f; arp_get_filter(&f);  ser_filter(w, &f);
    seq_lfo_t l; arp_get_lfo(&l);        ser_lfo(w, &l);
    tlv_put_u8(w, (uint8_t)arp_get_quant_mode());
    seq_dist_t d; arp_get_dist(&d);      ser_dist(w, &d);
    tlv_end_section(w, h);
}

static bool parse_arp(tlv_reader_t *b, staged_arp_t *a)
{
    uint8_t v;
    if (!tlv_get_u8(b, &v)) return false;
    a->enabled = v != 0;
    if (!tlv_get_u16(b, &a->patch)) return false;
    a->patch = clamp_patch(a->patch);
    if (!tlv_get_u8(b, &v)) return false;
    a->dir = (v >= ARP_DIR_COUNT) ? ARP_UP : (arp_dir_t)v;
    if (!tlv_get_u8(b, &a->octaves)) return false;
    a->octaves = SEQ_CLAMP_U8(a->octaves, 1, ARP_OCT_MAX);
    if (!tlv_get_u8(b, &v)) return false;
    a->rate = (v >= NOTE_DIV_COUNT) ? NOTE_DIV_1_4 : (note_div_t)v;
    if (!tlv_get_u8(b, &a->gate_pct)) return false;
    a->gate_pct = SEQ_CLAMP_U8(a->gate_pct, 10, 100);
    if (!tlv_get_u8(b, &a->scale)) return false;
    if (a->scale >= quantizer_scale_count()) a->scale = 0;
    if (!tlv_get_u8(b, &a->root)) return false;
    if (!tlv_get_u16(b, &a->portamento_ms)) return false;
    if (a->portamento_ms > ARP_PORTAMENTO_MAX_MS) a->portamento_ms = ARP_PORTAMENTO_MAX_MS;
    if (!tlv_get_f32(b, &a->amp_scale)) return false;
    a->amp_scale = SEQ_CLAMP_F32(a->amp_scale, 0.0f, 1.0f);
    for (int i = 0; i < ARP_MAX_SLOTS; i++) {
        if (!tlv_get_i16(b, &a->slots[i])) return false;
        if (a->slots[i] < ARP_REST || a->slots[i] > 127) a->slots[i] = -1;
    }
    if (!de_env(b, &a->env))       return false;
    if (!de_env(b, &a->env2))      return false;
    if (!de_filter(b, &a->filter)) return false;
    if (!de_lfo(b, &a->lfo))       return false;
    /* Scale source (arp_quant_mode_t). */
    if (!tlv_get_u8(b, &v)) return false;
    a->quant_mode = (v >= ARP_QUANT_COUNT) ? ARP_QUANT_CHORD : v;
    if (!de_dist(b, &a->dist)) return false;
    return true;
}

/* patch first so later param pushes land on the rebuilt slot; enabled LAST. */
static void apply_arp(const staged_arp_t *a)
{
    arp_set_patch(a->patch);
    arp_set_direction(a->dir);
    arp_set_octaves(a->octaves);
    arp_set_rate(a->rate);
    arp_set_gate_pct(a->gate_pct);
    arp_set_scale(a->scale);
    arp_set_root_note(a->root);
    arp_set_quant_mode((arp_quant_mode_t)a->quant_mode);
    arp_set_portamento_ms(a->portamento_ms);
    arp_set_amp_scale(a->amp_scale);
    for (int i = 0; i < ARP_MAX_SLOTS; i++) arp_set_slot((uint8_t)i, a->slots[i]);
    arp_set_envelope(&a->env);
    arp_set_envelope2(&a->env2);
    arp_set_filter(&a->filter);
    arp_set_lfo(&a->lfo);
    arp_set_dist(&a->dist);
    arp_set_enabled(a->enabled);
}

/* ── DRON section ─────────────────────────────────────────────────────────── */

typedef struct {
    bool            enabled;
    drone_source_t  source;
    uint16_t        wave;
    chord_type_t    chord;
    uint8_t         root;
    uint16_t        patch;
    float           resonance;
    float           amp_peak;
    float           amp_duck;
    note_div_t      rate;
    bool            sub_enabled;
    int8_t          sub_interval;
    float           sweep_lo, sweep_hi;
    uint8_t         sweep_bars;
    float           gate_len;
    uint8_t         swing;
    float           blip;
    drone_pattern_t pattern;
    drone_follow_t  follow;
    voice_params_t  vp;
} staged_drone_t;

static void ser_drone(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_DRON, 4);
    tlv_put_u8(w, drone_get_enabled() ? 1 : 0);
    tlv_put_u8(w, (uint8_t)drone_get_source());
    tlv_put_u16(w, drone_get_wave());
    tlv_put_u8(w, (uint8_t)drone_get_chord());
    tlv_put_u8(w, drone_get_root_note());
    tlv_put_u16(w, drone_get_patch());
    tlv_put_f32(w, drone_get_resonance());
    tlv_put_f32(w, drone_get_amp_peak());
    tlv_put_f32(w, drone_get_amp_duck());
    tlv_put_u8(w, (uint8_t)drone_get_rate());
    tlv_put_u8(w, drone_get_sub_enabled() ? 1 : 0);
    tlv_put_i8(w, drone_get_sub_interval());
    tlv_put_f32(w, drone_get_sweep_lo());
    tlv_put_f32(w, drone_get_sweep_hi());
    tlv_put_u8(w, drone_get_sweep_bars());
    tlv_put_f32(w, drone_get_gate_len());
    tlv_put_u8(w, drone_get_swing());
    tlv_put_f32(w, drone_get_blip());
    tlv_put_u8(w, (uint8_t)drone_get_pattern());
    tlv_put_u8(w, (uint8_t)drone_get_follow());
    voice_params_t vp; drone_get_voice_params(&vp); ser_vp(w, &vp);
    tlv_end_section(w, h);
}

static bool parse_drone(tlv_reader_t *b, staged_drone_t *d)
{
    uint8_t v;
    if (!tlv_get_u8(b, &v)) return false;
    d->enabled = v != 0;
    if (!tlv_get_u8(b, &v)) return false;
    d->source = (v > DRONE_SRC_PATCH) ? DRONE_SRC_PATCH : (drone_source_t)v;
    if (!tlv_get_u16(b, &d->wave)) return false;
    if (!tlv_get_u8(b, &v)) return false;
    d->chord = (v >= CHORD_TYPE_COUNT) ? CHORD_MAJ : (chord_type_t)v;
    if (!tlv_get_u8(b, &d->root))  return false;
    if (!tlv_get_u16(b, &d->patch)) return false;
    d->patch = clamp_patch(d->patch);
    if (!tlv_get_f32(b, &d->resonance)) return false;
    if (!tlv_get_f32(b, &d->amp_peak))  return false;
    d->amp_peak = SEQ_CLAMP_F32(d->amp_peak, 0.0f, 1.0f);
    if (!tlv_get_f32(b, &d->amp_duck))  return false;
    d->amp_duck = SEQ_CLAMP_F32(d->amp_duck, 0.0f, 1.0f);
    if (!tlv_get_u8(b, &v)) return false;
    d->rate = (v >= NOTE_DIV_COUNT) ? NOTE_DIV_1_4 : (note_div_t)v;
    { uint8_t se; if (!tlv_get_u8(b, &se)) return false; d->sub_enabled = se != 0; }
    if (!tlv_get_i8(b, &d->sub_interval)) return false;
    if (!tlv_get_f32(b, &d->sweep_lo)) return false;
    if (!tlv_get_f32(b, &d->sweep_hi)) return false;
    if (!tlv_get_u8(b, &d->sweep_bars)) return false;
    if (!tlv_get_f32(b, &d->gate_len)) return false;
    d->gate_len = SEQ_CLAMP_F32(d->gate_len, 0.05f, 0.95f);
    if (!tlv_get_u8(b, &d->swing)) return false;
    if (d->swing > 66) d->swing = 66;   /* mirrors the private SEQ/DRONE_SWING_MAX ceiling */
    if (!tlv_get_f32(b, &d->blip)) return false;
    d->blip = SEQ_CLAMP_F32(d->blip, 0.0f, 1.0f);
    if (!tlv_get_u8(b, &v)) return false;
    d->pattern = (v >= DRONE_PAT_COUNT) ? DRONE_PAT_FULL : (drone_pattern_t)v;
    if (!tlv_get_u8(b, &v)) return false;
    d->follow = (v >= DRONE_FOLLOW_COUNT) ? DRONE_FOLLOW_OFF : (drone_follow_t)v;
    if (!de_vp(b, &d->vp)) return false;
    return true;
}

/* enabled LAST; patch is skipped (leaving the live patch) when the value
 * falls in a range the drone's excitation model can't play. */
static void apply_drone(const staged_drone_t *d)
{
    drone_set_source(d->source);
    drone_set_wave(d->wave);
    drone_set_chord(d->chord);
    drone_set_root_note(d->root);
    if (!drone_patch_excluded(d->patch)) drone_set_patch(d->patch);
    drone_set_resonance(d->resonance);
    drone_set_amp_peak(d->amp_peak);
    drone_set_amp_duck(d->amp_duck);
    drone_set_rate(d->rate);
    drone_set_sub_interval(d->sub_interval);
    drone_set_sub_enabled(d->sub_enabled);
    drone_set_sweep_lo(d->sweep_lo);
    drone_set_sweep_hi(d->sweep_hi);
    drone_set_sweep_bars(d->sweep_bars);
    drone_set_gate_len(d->gate_len);
    drone_set_swing(d->swing);
    drone_set_blip(d->blip);
    drone_set_pattern(d->pattern);
    drone_set_follow(d->follow);
    drone_set_voice_params(&d->vp);
    drone_set_enabled(d->enabled);
}

/* ── DSTD section (normal drone) ──────────────────────────────────────────── */

typedef struct {
    bool            enabled;
    drone_source_t  source;
    uint16_t        wave;
    chord_type_t    chord;
    uint8_t         root;
    drone_follow_t  follow;
    float           level;
    uint16_t        patch;
    bool            sub_enabled;
    int8_t          sub_interval;
    voice_params_t  vp;
} staged_drone_std_t;

static void ser_drone_std(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_DSTD, 2);
    tlv_put_u8(w, drone_std_get_enabled() ? 1 : 0);
    tlv_put_u8(w, (uint8_t)drone_std_get_source());
    tlv_put_u16(w, drone_std_get_wave());
    tlv_put_u8(w, (uint8_t)drone_std_get_chord());
    tlv_put_u8(w, drone_std_get_root_note());
    tlv_put_u8(w, (uint8_t)drone_std_get_follow());
    tlv_put_f32(w, drone_std_get_level());
    tlv_put_u16(w, drone_std_get_patch());
    tlv_put_u8(w, drone_std_get_sub_enabled() ? 1 : 0);
    tlv_put_i8(w, drone_std_get_sub_interval());
    voice_params_t vp; drone_std_get_voice_params(&vp); ser_vp(w, &vp);
    tlv_end_section(w, h);
}

static bool parse_drone_std(tlv_reader_t *b, staged_drone_std_t *d)
{
    uint8_t v;
    if (!tlv_get_u8(b, &v)) return false;
    d->enabled = v != 0;
    if (!tlv_get_u8(b, &v)) return false;
    d->source = (v > DRONE_SRC_PATCH) ? DRONE_SRC_PATCH : (drone_source_t)v;
    if (!tlv_get_u16(b, &d->wave)) return false;
    if (!tlv_get_u8(b, &v)) return false;
    d->chord = (v >= CHORD_TYPE_COUNT) ? CHORD_MAJ : (chord_type_t)v;
    if (!tlv_get_u8(b, &d->root)) return false;
    if (!tlv_get_u8(b, &v)) return false;
    d->follow = (v >= DRONE_FOLLOW_COUNT) ? DRONE_FOLLOW_OFF : (drone_follow_t)v;
    if (!tlv_get_f32(b, &d->level)) return false;
    d->level = SEQ_CLAMP_F32(d->level, 0.0f, 1.0f);
    if (!tlv_get_u16(b, &d->patch)) return false;
    d->patch = clamp_patch(d->patch);
    { uint8_t se; if (!tlv_get_u8(b, &se)) return false; d->sub_enabled = se != 0; }
    if (!tlv_get_i8(b, &d->sub_interval)) return false;
    if (!de_vp(b, &d->vp)) return false;
    return true;
}

/* As apply_drone(): enabled LAST, an unplayable patch leaves the live one. */
static void apply_drone_std(const staged_drone_std_t *d)
{
    drone_std_set_source(d->source);
    drone_std_set_wave(d->wave);
    drone_std_set_chord(d->chord);
    drone_std_set_root_note(d->root);
    drone_std_set_follow(d->follow);
    drone_std_set_level(d->level);
    if (!drone_patch_excluded(d->patch)) drone_std_set_patch(d->patch);
    drone_std_set_sub_interval(d->sub_interval);
    drone_std_set_sub_enabled(d->sub_enabled);
    drone_std_set_voice_params(&d->vp);
    drone_std_set_enabled(d->enabled);
}

/* ── CLIP section ─────────────────────────────────────────────────────────── */

/* The bounce shape and each clip slot's settings. Clip audio is not
 * persisted (a clip is lost on power cycle); a load shows the slots empty
 * but configured. */
typedef struct {
    uint8_t bars;
    bool    stereo;
    uint8_t tail;
    uint8_t level[CLIP_SLOT_COUNT];
    uint8_t mode[CLIP_SLOT_COUNT];
    uint8_t after;
} staged_clip_t;

static void ser_clip(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_CLIP, 2);   /* v2: +after */
    clip_bounce_shape_t sh;
    clip_bounce_get_shape(&sh);
    tlv_put_u8(w, sh.max_bars);
    tlv_put_u8(w, sh.stereo ? 1 : 0);
    tlv_put_u8(w, (uint8_t)sh.tail);
    tlv_put_u8(w, CLIP_SLOT_COUNT);
    for (uint8_t s = 0; s < CLIP_SLOT_COUNT; s++) {
        tlv_put_u8(w, clip_player_get_level(s));
        tlv_put_u8(w, (uint8_t)clip_player_get_tempo_mode(s));
    }
    tlv_put_u8(w, (uint8_t)sh.after);   /* v2 */
    tlv_end_section(w, h);
}

/* Slots beyond this build's count are consumed but not stored. */
static bool parse_clip(tlv_reader_t *b, staged_clip_t *c)
{
    uint8_t v, nslots;
    if (!tlv_get_u8(b, &c->bars)) return false;
    if (!tlv_get_u8(b, &v)) return false;
    c->stereo = v != 0;
    if (!tlv_get_u8(b, &c->tail)) return false;
    if (c->tail > CLIP_TAIL_BAR) c->tail = CLIP_TAIL_BAR;
    if (!tlv_get_u8(b, &nslots)) return false;
    for (uint8_t s = 0; s < nslots; s++) {
        uint8_t level, mode;
        if (!tlv_get_u8(b, &level)) return false;
        if (!tlv_get_u8(b, &mode))  return false;
        if (s < CLIP_SLOT_COUNT) {
            c->level[s] = (level > 100) ? 100 : level;
            c->mode[s]  = (mode > CLIP_TEMPO_VARI) ? CLIP_TEMPO_STRETCH : mode;
        }
    }
    if (!tlv_get_u8(b, &c->after)) return false;
    if (c->after > CLIP_AFTER_CLEAR) c->after = CLIP_AFTER_MUTE;
    return true;
}

static void apply_clip(const staged_clip_t *c)
{
    clip_bounce_shape_t sh = { .max_bars = c->bars, .stereo = c->stereo,
                               .tail = (clip_tail_t)c->tail,
                               .after = (clip_after_t)c->after };
    clip_bounce_set_shape(&sh);   /* clamps bars to the 1/2/4/8/16 list */
    for (uint8_t s = 0; s < CLIP_SLOT_COUNT; s++) {
        clip_player_set_level(s, c->level[s]);
        clip_player_set_tempo_mode(s, (clip_tempo_mode_t)c->mode[s]);
    }
}

/* ── PROG section ─────────────────────────────────────────────────────────── */

/* Static cap for the staged array; the real ceiling is
 * sequencer_core_progression_get_max() at runtime. Entries beyond either
 * bound are still consumed from the stream (reader position) but not stored. */
#define PROJECT_PROG_MAX_ENTRIES 16

typedef struct {
    bool         enabled;
    uint8_t      count;
    uint8_t      root[PROJECT_PROG_MAX_ENTRIES];
    chord_type_t chord_type[PROJECT_PROG_MAX_ENTRIES];
    uint8_t      duration_bars[PROJECT_PROG_MAX_ENTRIES];
} staged_prog_t;

static void ser_prog(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_PROG, 1);
    tlv_put_u8(w, sequencer_core_progression_get_enabled() ? 1 : 0);
    uint8_t count = sequencer_core_progression_get_count();
    tlv_put_u8(w, count);
    for (uint8_t i = 0; i < count; i++) {
        uint8_t root, bars; chord_type_t ct;
        sequencer_core_progression_get_entry(i, &root, &ct, &bars);
        tlv_put_u8(w, root);
        tlv_put_u8(w, (uint8_t)ct);
        tlv_put_u8(w, bars);
    }
    tlv_end_section(w, h);
}

static bool parse_prog(tlv_reader_t *b, staged_prog_t *p)
{
    uint8_t v, raw_count;
    if (!tlv_get_u8(b, &v)) return false;
    p->enabled = v != 0;
    if (!tlv_get_u8(b, &raw_count)) return false;

    uint8_t max_entries = sequencer_core_progression_get_max();
    if (max_entries > PROJECT_PROG_MAX_ENTRIES) max_entries = PROJECT_PROG_MAX_ENTRIES;
    p->count = (raw_count > max_entries) ? max_entries : raw_count;

    for (uint8_t i = 0; i < raw_count; i++) {
        uint8_t root, ctraw, bars;
        if (!tlv_get_u8(b, &root))  return false;
        if (!tlv_get_u8(b, &ctraw)) return false;
        if (!tlv_get_u8(b, &bars))  return false;
        if (i < p->count) {
            p->root[i]          = root;
            p->chord_type[i]    = (ctraw >= CHORD_TYPE_COUNT) ? CHORD_MAJ : (chord_type_t)ctraw;
            p->duration_bars[i] = bars;
        }
    }
    return true;
}

static void apply_prog(const staged_prog_t *p)
{
    sequencer_core_progression_set_count(p->count);
    for (uint8_t i = 0; i < p->count; i++) {
        sequencer_core_progression_set_entry(i, p->root[i], p->chord_type[i], p->duration_bars[i]);
    }
    sequencer_core_progression_set_enabled(p->enabled);
}

/* ── PGEN section ─────────────────────────────────────────────────────────── */

/* The progression generator's settings. The chords they produced are the PROG
 * section above; this is what the Prog Gen page shows for them. */
typedef struct {
    prog_gen_params_t params;
} staged_pgen_t;

static void ser_pgen(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_PGEN, 1);
    prog_gen_params_t p;
    sequencer_core_progression_gen_params_get(&p);
    tlv_put_u8(w, p.style);
    tlv_put_u8(w, p.len);
    tlv_put_u8(w, p.bars);
    tlv_put_u8(w, p.ext);
    tlv_put_u8(w, p.var);
    tlv_put_u16(w, p.seed);
    tlv_end_section(w, h);
}

/* No clamping here: the generator clamps every field on use. */
static bool parse_pgen(tlv_reader_t *b, staged_pgen_t *g)
{
    if (!tlv_get_u8(b, &g->params.style)) return false;
    if (!tlv_get_u8(b, &g->params.len))   return false;
    if (!tlv_get_u8(b, &g->params.bars))  return false;
    if (!tlv_get_u8(b, &g->params.ext))   return false;
    if (!tlv_get_u8(b, &g->params.var))   return false;
    if (!tlv_get_u16(b, &g->params.seed)) return false;
    return true;
}

static void apply_pgen(const staged_pgen_t *g)
{
    sequencer_core_progression_gen_params_set(&g->params);
}

/* ── WTCU section (custom wavetable, CONFIG_SYNTH_CUSTOM_WT) ───────────────
 * Version 2: the builder's sixteen parameter bytes in wt_params_t order
 * (shape, width, bright, sync, peak per keyframe A, M, B, then range); the
 * table itself is never stored, the load rebuilds it. Written only when the
 * builder is compiled in; a build without it skips the section as unknown. */
#if CONFIG_SYNTH_CUSTOM_WT
static void ser_wtcu(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_WTCU, 2);
    wt_params_t p;
    wt_builder_get_params(&p);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_put_u8(w, p.shape[k]);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_put_u8(w, p.width[k]);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_put_u8(w, p.bright[k]);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_put_u8(w, p.sync[k]);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_put_u8(w, p.peak[k]);
    tlv_put_u8(w, p.range);
    tlv_end_section(w, h);
}

/* Clamped here (crash-safety for foreign data); the builder clamps again. */
static bool parse_wtcu(tlv_reader_t *b, wt_params_t *p)
{
    for (uint8_t k = 0; k < WT_KEYS; k++) if (!tlv_get_u8(b, &p->shape[k]))  return false;
    for (uint8_t k = 0; k < WT_KEYS; k++) if (!tlv_get_u8(b, &p->width[k]))  return false;
    for (uint8_t k = 0; k < WT_KEYS; k++) if (!tlv_get_u8(b, &p->bright[k])) return false;
    for (uint8_t k = 0; k < WT_KEYS; k++) if (!tlv_get_u8(b, &p->sync[k]))   return false;
    for (uint8_t k = 0; k < WT_KEYS; k++) if (!tlv_get_u8(b, &p->peak[k]))   return false;
    if (!tlv_get_u8(b, &p->range)) return false;
    wt_params_clamp(p);
    return true;
}
#endif

/* ── CHRD section (chord presets, seq_chords.h) ──────────────────────────
 * v1: u8 slot count, then per slot u8 tone count + SEQ_CHORD_MAX_NOTES note
 * bytes (fixed width; a wider voicing bumps the version). Old firmware skips
 * the unknown section: chords vanish there and any chord sentinel in that
 * file's LAYR notes range-clamps to a top-of-range note, no crash. */

static void ser_chrd(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_CHRD, 1);
    tlv_put_u8(w, SEQ_CHORD_SLOTS);
    for (uint8_t s = 0; s < SEQ_CHORD_SLOTS; s++) {
        seq_chord_t c;
        if (!seq_chords_get(s, &c)) memset(&c, 0, sizeof c);
        tlv_put_u8(w, c.count);
        for (uint8_t i = 0; i < SEQ_CHORD_MAX_NOTES; i++) {
            tlv_put_u8(w, c.notes[i]);
        }
    }
    tlv_end_section(w, h);
}

static bool parse_chrd(tlv_reader_t *b, seq_chord_t slots[SEQ_CHORD_SLOTS])
{
    uint8_t nslots;
    if (!tlv_get_u8(b, &nslots)) return false;
    for (uint8_t s = 0; s < nslots; s++) {
        uint8_t count;
        uint8_t notes[SEQ_CHORD_MAX_NOTES];
        if (!tlv_get_u8(b, &count)) return false;
        for (uint8_t i = 0; i < SEQ_CHORD_MAX_NOTES; i++) {
            if (!tlv_get_u8(b, &notes[i])) return false;
        }
        if (s < SEQ_CHORD_SLOTS) {
            /* count is advisory: seq_chords_import re-derives it from the
             * non-zero tones, which also normalizes hand-edited files. */
            memcpy(slots[s].notes, notes, sizeof notes);
            slots[s].count = (count > SEQ_CHORD_MAX_NOTES)
                             ? SEQ_CHORD_MAX_NOTES : count;
        }
    }
    return true;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool project_snapshot_save(uint8_t slot, const char *name)
{
    if (!project_fs_ok()) return false;

    uint8_t *buf = heap_caps_malloc(PROJECT_SER_BUF_CAP, MALLOC_CAP_SPIRAM);
    seq_layer_t *scratch = heap_caps_malloc(sizeof(seq_layer_t), MALLOC_CAP_SPIRAM);
    if (!buf || !scratch) {
        free(buf);
        free(scratch);
        ESP_LOGE(TAG, "save slot %u: SPIRAM allocation failed", slot);
        return false;
    }

    tlv_writer_t w;
    tlv_writer_init(&w, buf, PROJECT_SER_BUF_CAP);
    ser_glob(&w);
    uint8_t n = sequencer_core_get_num_layers();
    for (uint8_t i = 0; i < n; i++) {
        if (sequencer_core_export_layer(i, scratch)) ser_layer(&w, scratch);
    }
    ser_arp(&w);
    ser_drone(&w);
    ser_drone_std(&w);
    ser_prog(&w);
    ser_chrd(&w);
    ser_clip(&w);
    ser_pgen(&w);
#if CONFIG_SYNTH_CUSTOM_WT
    ser_wtcu(&w);
#endif

    bool ok = !w.err && project_store_write(slot, name, buf, w.len);

    free(scratch);
    free(buf);
    ESP_LOGI(TAG, "save slot %u: %s (%u layer(s), %u bytes)",
             slot, ok ? "OK" : "FAILED", n, (unsigned)w.len);
    return ok;
}

bool project_snapshot_load_buffer(const uint8_t *payload, size_t len, const char *name)
{
    if (!payload) return false;
    if (!name) name = "";

    /* Heap, not stack: the drones' voice_params_t blocks would put this frame
     * past the 1 KB -Wstack-usage limit. */
    typedef struct {
        staged_drone_t     drone;
        staged_drone_std_t dstd;
    } staged_drones_t;
    seq_layer_t *staged_layers =
        heap_caps_malloc(sizeof(seq_layer_t) * MAX_LAYERS, MALLOC_CAP_SPIRAM);
    staged_drones_t *sd = heap_caps_calloc(1, sizeof(*sd), MALLOC_CAP_SPIRAM);
    if (!staged_layers || !sd) {
        free(staged_layers);
        free(sd);
        ESP_LOGE(TAG, "load '%s': SPIRAM allocation failed", name);
        return false;
    }

    staged_glob_t  staged_glob;  memset(&staged_glob, 0, sizeof staged_glob);
    staged_arp_t   staged_arp;   memset(&staged_arp, 0, sizeof staged_arp);
    staged_prog_t  staged_prog;  memset(&staged_prog, 0, sizeof staged_prog);
    staged_clip_t  staged_clip;  memset(&staged_clip, 0, sizeof staged_clip);
    staged_pgen_t  staged_pgen;  memset(&staged_pgen, 0, sizeof staged_pgen);
    seq_chord_t    staged_chords[SEQ_CHORD_SLOTS];
    memset(staged_chords, 0, sizeof staged_chords);
    uint8_t staged_layer_count = 0;
    bool got_glob = false, got_arp = false, got_drone = false, got_prog = false;
    bool got_chrd = false, got_clip = false, got_pgen = false, got_dstd = false;
#if CONFIG_SYNTH_CUSTOM_WT
    wt_params_t staged_wtcu;
    bool got_wtcu = false;
#endif

    tlv_reader_t r;
    tlv_reader_init(&r, payload, len);
    bool ok = true;
    uint32_t tag; uint8_t ver; tlv_reader_t body;
    while (ok && tlv_next_section(&r, &tag, &ver, &body)) {
        switch (tag) {
        case TAG_GLOB:
            if (got_glob || ver != 6) { ok = false; break; }
            ok = parse_glob(&body, &staged_glob);
            got_glob = ok;
            break;
        case TAG_LAYR:
            /* Exactly ser_layer()'s version: older files are rejected outright
             * (no migration), and the ceiling must track the writer or the
             * firmware rejects its own files. */
            if (ver != LAYR_VERSION || staged_layer_count >= MAX_LAYERS) { ok = false; break; }
            ok = parse_layer(&body, &staged_layers[staged_layer_count]);
            if (ok) staged_layer_count++;
            break;
        case TAG_ARP:
            /* Exactly ser_arp()'s version (see TAG_LAYR). */
            if (got_arp || ver != 14) { ok = false; break; }
            ok = parse_arp(&body, &staged_arp);
            got_arp = ok;
            break;
        case TAG_DRON:
            if (got_drone || ver != 4) { ok = false; break; }
            ok = parse_drone(&body, &sd->drone);
            got_drone = ok;
            break;
        case TAG_DSTD:
            if (got_dstd || ver != 2) { ok = false; break; }
            ok = parse_drone_std(&body, &sd->dstd);
            got_dstd = ok;
            break;
        case TAG_PROG:
            if (got_prog || ver != 1) { ok = false; break; }
            ok = parse_prog(&body, &staged_prog);
            got_prog = ok;
            break;
        case TAG_CHRD:
            if (got_chrd || ver != 1) { ok = false; break; }
            ok = parse_chrd(&body, staged_chords);
            got_chrd = ok;
            break;
        case TAG_CLIP:
            if (got_clip || ver != 2) { ok = false; break; }
            ok = parse_clip(&body, &staged_clip);
            got_clip = ok;
            break;
        case TAG_PGEN:
            if (got_pgen || ver != 1) { ok = false; break; }
            ok = parse_pgen(&body, &staged_pgen);
            got_pgen = ok;
            break;
#if CONFIG_SYNTH_CUSTOM_WT
        case TAG_WTCU:
            if (got_wtcu || ver != 2) { ok = false; break; }
            ok = parse_wtcu(&body, &staged_wtcu);
            got_wtcu = ok;
            break;
#endif
        default:
            break;   /* unknown section: ignore (forward-compat) */
        }
    }
    if (r.err) ok = false;   /* truncated/corrupt outer stream */

    if (ok && (!got_glob || staged_layer_count < 1 ||
               staged_layers[0].type != SEQ_LAYER_DRUM)) {
        ok = false;
    }
    /* Exactly one drum layer, and only at index 0: add_layer(DRUM) always
     * binds the fixed SEQ_DRUM_SYNTH_BASE slots, so a second drum layer
     * would alias the first one's AMY synths. */
    for (uint8_t i = 1; ok && i < staged_layer_count; i++) {
        if (staged_layers[i].type == SEQ_LAYER_DRUM) ok = false;
    }

    if (!ok) {
        free(staged_layers);
        free(sd);
        ESP_LOGW(TAG, "load '%s': validation failed, no changes made", name);
        return false;
    }

    /* ── Phase 2: apply. Nothing above this point touched live state. ── */
    sequencer_core_set_playing(false);
    arp_core_clear_all();

    /* Chord table BEFORE the layer imports: import sizes each row's voice
     * count from the chord a loaded sentinel references (seq_track_num_voices),
     * so the table must already hold the file's voicings. A file without a
     * CHRD section clears the table - the project is the whole persisted
     * state, chords included. */
    seq_chords_import(got_chrd ? staged_chords : NULL);

    /* Split flags BEFORE the layer imports: a synth the import creates takes
     * its bus from the flags at send time (the amy_helpers ingress hook), and
     * apply_glob's re-tag cannot reach it - it skips slots AMY has not built
     * yet, and the import's events are still queued when it runs. */
    for (fx_group_t grp = FX_GROUP_DRUMS; grp < FX_GROUP_COUNT; grp++) {
        fx_bus_set_split(grp, (staged_glob.split_flags & (1u << grp)) != 0);
    }

    while (sequencer_core_get_num_layers() > 1) {
        sequencer_core_delete_layer((uint8_t)(sequencer_core_get_num_layers() - 1));
    }
    for (uint8_t i = 1; i < staged_layer_count; i++) {
        sequencer_core_add_layer(staged_layers[i].type, staged_layers[i].num_steps);
    }
    for (uint8_t i = 0; i < staged_layer_count; i++) {
        sequencer_core_import_layer(i, &staged_layers[i]);
    }

    apply_glob(&staged_glob);
    if (got_arp)   apply_arp(&staged_arp);
    /* A file without a drone section leaves that drone off, not playing
     * whatever the session had (the project is the whole persisted state). */
    if (got_drone) apply_drone(&sd->drone);
    else           drone_set_enabled(false);
    if (got_dstd)  apply_drone_std(&sd->dstd);
    else           drone_std_set_enabled(false);
    if (got_prog)  apply_prog(&staged_prog);
    if (got_pgen)  apply_pgen(&staged_pgen);
    if (got_clip)  apply_clip(&staged_clip);
#if CONFIG_SYNTH_CUSTOM_WT
    /* A file without the section leaves the builder at its defaults, as for
     * the drones above. */
    if (!got_wtcu) wt_params_default(&staged_wtcu);
    wt_builder_set_params(&staged_wtcu);
#endif

    /* The layer import writes solo[] wholesale rather than through the setter,
     * so nothing has applied the loaded solo state yet. Do it after the arp and
     * drone applies, or their duck would be undone by the enable that follows. */
    sequencer_core_notify_solo_changed();

    synth_ui_reload_mirror_from_core();

    free(staged_layers);
    free(sd);
    ESP_LOGI(TAG, "load '%s' OK: %u layer(s)", name, staged_layer_count);
    return true;
}

bool project_snapshot_load(uint8_t slot)
{
    uint8_t *payload = NULL;
    size_t   len = 0;
    char     name[PROJECT_NAME_LEN];
    if (!project_store_read(slot, &payload, &len, name)) return false;

    bool ok = project_snapshot_load_buffer(payload, len, name);
    free(payload);
    return ok;
}

#if CONFIG_SYNTH_PROJECT_SELFTEST
void project_snapshot_selftest(void)
{
    /* Never over a real project: the test deletes the slot afterwards. */
    uint8_t slot = CONFIG_SYNTH_PROJECT_MAX_SLOTS - 1;
    project_slot_info_t info;
    if (project_store_slot_info(slot, &info) && info.used) {
        ESP_LOGW(TAG, "SNAPSHOT SELFTEST SKIPPED (slot P%02u in use)", (unsigned)slot);
        return;
    }
    bool pass = project_snapshot_save(slot, "st2");
    pass = pass && project_snapshot_load(slot);
    project_store_delete(slot);
    ESP_LOGI(TAG, "SNAPSHOT SELFTEST %s", pass ? "PASS" : "FAIL");
}
#endif
