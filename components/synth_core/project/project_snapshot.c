/* Project snapshot serializer + loader orchestration.
 *
 * Field order IS the format: every ser_ writer and its de_/parse_ reader must
 * walk fields in the same sequence or saved projects corrupt on load. The
 * shared env/filter/lfo/dist/vp helpers keep LAYR's voice_params_t and the
 * ARP/DROP/DRON/DSTD sections from drifting apart.
 *
 * Format rules:
 * - Records: every struct a section writes field by field is a block
 *   (tlv_begin_block/tlv_get_block, project_tlv.h); blocks nest. A new field
 *   is appended at the end of the block of the record it belongs to, and that
 *   needs no version bump.
 * - Short records keep the fallback default: each parser seeds its record
 *   from the owner's fallback default (a pure *_defaults() function that
 *   never reads Kconfig, so a file loads the same on every build), then reads
 *   the fields in writer order. A field past the end of its block (the file
 *   predates it) fails to read and keeps the seed, as does every field after
 *   it, because a failed read leaves its output untouched and the block's
 *   reader stays failed. So parsers read straight through and clamp every
 *   field, read or seeded. Bytes after the last field this build knows (a
 *   newer build's appends) are ignored.
 * - Version bump only for a reshape: reorder, removal, enum remap, type
 *   change, or a fixed array length (SEQ_TRACKS, SEQ_MAX_STEPS, FX_BUS_COUNT,
 *   ARP_MAX_SLOTS, DROP_SEG_MAX, SEQ_CHORD_SLOTS, ...). Each section's
 *   <TAG>_VERSION sits next to its writer and is the loader's only accepted
 *   value.
 * - A reshaped optional section (any but GLOB and LAYR) is skipped and loads
 *   as if absent; project_snapshot_skipped() reports it. A GLOB or LAYR at
 *   another version refuses the file.
 * - An absent or skipped section applies its fallback defaults whole, through
 *   the same apply_* as a parsed one.
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
#include "drop_core.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"
#include "custompatches/drone_common.h"   /* DRONE_SUB_INTERVAL_MIN/MAX */
#include "custompatches/clip_bounce.h"
#include "custompatches/clip_player.h"
#include "mixer.h"
#include "amy_fx.h"
#include "transport_stop.h"
#include "amy.h"               /* amy_num_algorithms - fm_algo_override clamp */
#include "quantizer.h"
#include "voice_config.h"
#include "seq_defaults.h"       /* melodic env defaults for the v14 layer block */
#include "synth_ui/synth_ui_internal.h"   /* synth_ui_reload_mirror_from_core(), ui_clock_* */
#include "sdkconfig.h"
#if CONFIG_SYNTH_CUSTOM_WT
#include "custompatches/wt_builder.h"
#endif
#if CONFIG_SYNTH_CUSTOM_FM
#include "custompatches/fm_voice.h"
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
#define TAG_DROP 0x504F5244u
#define TAG_DRON 0x4E4F5244u
#define TAG_DSTD 0x44545344u
#define TAG_PROG 0x474F5250u
#define TAG_CHRD 0x44524843u
#define TAG_CLIP 0x50494C43u
#define TAG_PGEN 0x4E454750u
#define TAG_WTCU 0x55435457u
#define TAG_FMCU 0x55434D46u
#define TAG_MIXR 0x5258494Du

#define PROJECT_SER_BUF_CAP (64 * 1024)

/* ── Shared env/filter/lfo record codecs ─────────────────────────────────
 * Used by voice_params_t (LAYR per track, DROP, DRON, DSTD) and directly by
 * ARP, so the engines' persisted shapes cannot drift apart. */

static void ser_env(tlv_writer_t *w, const seq_env_t *e)
{
    size_t h = tlv_begin_block(w);
    tlv_put_u32(w, e->attack_ms);
    tlv_put_u32(w, e->decay_ms);
    tlv_put_u8(w,  e->sustain_pct);
    tlv_put_u32(w, e->release_ms);
    tlv_put_u8(w,  e->eg_type);
    tlv_end_block(w, h);
}

/* Seed: *e as the caller left it (its record's fallback default); a field
 * appended here takes its value from this seed. */
static bool de_env(tlv_reader_t *r, seq_env_t *e)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    tlv_get_u32(&b, &e->attack_ms);
    tlv_get_u32(&b, &e->decay_ms);
    tlv_get_u8(&b, &e->sustain_pct);
    if (e->sustain_pct > 100) e->sustain_pct = 100;
    tlv_get_u32(&b, &e->release_ms);
    tlv_get_u8(&b, &e->eg_type);
    return true;
}

static void ser_filter(tlv_writer_t *w, const seq_filter_t *f)
{
    size_t h = tlv_begin_block(w);
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
    tlv_end_block(w, h);
}

/* Reads every field into temporaries first, then bypasses the whole record
 * if filter_type is out of range rather than passing a bogus enum
 * downstream. Seed: *f as the caller left it (its record's fallback default;
 * the temporaries start from it, so a short record validates the seed); a
 * field appended here takes its value from this seed. */
static bool de_filter(tlv_reader_t *r, seq_filter_t *f)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    uint8_t ft = f->filter_type;
    bool en = f->enabled;
    float cutoff = f->cutoff_hz, resonance = f->resonance, feedback = f->feedback;
    float duty_ofs = f->ks_duty_ofs, key_track = f->key_track;
    float depth[2][SEQ_EGT_COUNT];
    memcpy(depth, f->eg_depth, sizeof depth);
    tlv_get_u8(&b, &ft);
    tlv_get_f32(&b, &cutoff);
    tlv_get_f32(&b, &resonance);
    tlv_get_bool(&b, &en);
    tlv_get_f32(&b, &feedback);
    feedback = SEQ_CLAMP_F32(feedback, 0.0f, 1.0f);
    for (int eg = 0; eg < 2; eg++) {
        for (int t = 0; t < SEQ_EGT_COUNT; t++) {
            tlv_get_f32(&b, &depth[eg][t]);
            depth[eg][t] = SEQ_CLAMP_F32(depth[eg][t],
                                         -seq_eg_depth_max((seq_eg_target_t)t),
                                         seq_eg_depth_max((seq_eg_target_t)t));
        }
    }
    tlv_get_f32(&b, &duty_ofs);
    duty_ofs = SEQ_CLAMP_F32(duty_ofs, -0.5f, 0.5f);
    tlv_get_f32(&b, &key_track);
    key_track = SEQ_CLAMP_F32(key_track, 0.0f, 1.0f);

    if (ft >= SEQ_FILTER_COUNT) {
        *f = (seq_filter_t){0};
        return true;
    }
    f->filter_type       = ft;
    f->cutoff_hz         = cutoff;
    f->resonance         = resonance;
    f->enabled           = en;
    f->feedback          = feedback;
    memcpy(f->eg_depth, depth, sizeof(f->eg_depth));
    f->ks_duty_ofs       = duty_ofs;
    f->key_track         = key_track;
    return true;
}

/* ── Distortion codec ─────────────────────────────────────────────────────
 * Values are re-clamped on read: a hand-edited record must not push an
 * out-of-range type into AMY. */
static void ser_dist(tlv_writer_t *w, const seq_dist_t *d)
{
    size_t h = tlv_begin_block(w);
    tlv_put_u8(w, d->type);
    tlv_put_u8(w, d->drive);
    tlv_put_u8(w, d->bits);
    tlv_put_u8(w, d->rate);
    tlv_put_u8(w, d->mix);
    tlv_end_block(w, h);
}

/* Seed: *d as the caller left it (its record's fallback default); a field
 * appended here takes its value from this seed. */
static bool de_dist(tlv_reader_t *r, seq_dist_t *d)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    uint8_t type = d->type, drive = d->drive, bits = d->bits, rate = d->rate, mix = d->mix;
    tlv_get_u8(&b, &type);
    tlv_get_u8(&b, &drive);
    tlv_get_u8(&b, &bits);
    tlv_get_u8(&b, &rate);
    tlv_get_u8(&b, &mix);
    d->type = type; d->drive = drive; d->bits = bits; d->rate = rate; d->mix = mix;
    voice_dist_clamp(d);
    return true;
}

static void ser_lfo(tlv_writer_t *w, const seq_lfo_t *l)
{
    size_t h = tlv_begin_block(w);
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
    tlv_end_block(w, h);
}

/* Same "read everything, then validate" shape and seed as de_filter. */
static bool de_lfo(tlv_reader_t *r, seq_lfo_t *l)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    bool en = l->enabled;
    uint8_t mode = (uint8_t)l->mode, wave = (uint8_t)l->wave, rate = (uint8_t)l->rate;
    uint8_t depth = l->depth, tgt = l->targets, wrate = l->wob_rate;
    uint8_t wdepth = l->wob_depth, wdeponly = l->wob_reach, foct = l->flt_oct_q;
    tlv_get_bool(&b, &en);
    tlv_get_u8(&b, &mode);
    tlv_get_u8(&b, &wave);
    tlv_get_u8(&b, &rate);
    tlv_get_u8(&b, &depth);
    tlv_get_u8(&b, &tgt);
    tlv_get_u8(&b, &wrate);
    tlv_get_u8(&b, &wdepth);
    tlv_get_u8(&b, &wdeponly);
    tlv_get_u8(&b, &foct);

    if (mode > LFO_MODE_RETRIG || wave >= LFO_WAVE_COUNT) {
        *l = (seq_lfo_t){ .rate = NOTE_DIV_1_8, .wob_rate = NOTE_DIV_1_8 };
        return true;
    }
    l->enabled = en;
    l->mode    = (lfo_mode_t)mode;
    l->wave    = (lfo_wave_t)wave;
    l->rate    = (rate < NOTE_DIV_COUNT) ? (note_div_t)rate : NOTE_DIV_1_8;
    l->depth   = (depth > 100) ? 100 : depth;
    l->targets = (uint8_t)(tgt & LFO_TGT_ALL);
    l->wob_rate  = (wrate < NOTE_DIV_STD_COUNT) ? wrate : (uint8_t)NOTE_DIV_1_8;
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
    size_t h = tlv_begin_block(w);
    ser_env(w, &vp->env);
    ser_env(w, &vp->env1);
    ser_filter(w, &vp->filter);
    ser_lfo(w, &vp->lfo);
    ser_lfo(w, &vp->lfo2);
    tlv_put_u8(w, vp->env_authored ? 1 : 0);
    tlv_put_u8(w, vp->env1_authored ? 1 : 0);
    tlv_put_u8(w, vp->filter_authored ? 1 : 0);
    tlv_put_u8(w, vp->lfo_authored ? 1 : 0);
    tlv_put_u8(w, vp->lfo2_authored ? 1 : 0);
    tlv_put_f32(w, vp->amp_trim);
    ser_dist(w, &vp->dist);
    tlv_put_u8(w, vp->dist_authored ? 1 : 0);
    tlv_end_block(w, h);
}

/* Seed: *vp as the caller left it (its record's fallback default); a field
 * appended here takes its value from this seed. */
static bool de_vp(tlv_reader_t *r, voice_params_t *vp)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    if (!de_env(&b, &vp->env))       return false;
    if (!de_env(&b, &vp->env1))      return false;
    if (!de_filter(&b, &vp->filter)) return false;
    if (!de_lfo(&b, &vp->lfo))       return false;
    if (!de_lfo(&b, &vp->lfo2))      return false;
    /* Each target on at most one LFO (seq_model.h); LFO1 keeps a shared one. */
    vp->lfo2.targets &= (uint8_t)~vp->lfo.targets;
    tlv_get_bool(&b, &vp->env_authored);
    tlv_get_bool(&b, &vp->env1_authored);
    tlv_get_bool(&b, &vp->filter_authored);
    tlv_get_bool(&b, &vp->lfo_authored);
    tlv_get_bool(&b, &vp->lfo2_authored);
    tlv_get_f32(&b, &vp->amp_trim);
    vp->amp_trim = SEQ_CLAMP_F32(vp->amp_trim, 0.0f, 1.0f);
    if (!de_dist(&b, &vp->dist)) return false;
    tlv_get_bool(&b, &vp->dist_authored);
    return true;
}

/* ── Shared voice_unison_t codec (LAYR per track, ARP, DROP voices, DRON,
 * DSTD) ─────────────────────────────────────────────────────────────────── */

static void ser_unison(tlv_writer_t *w, const voice_unison_t *u)
{
    size_t h = tlv_begin_block(w);
    tlv_put_u8(w, u->count);
    tlv_put_u8(w, u->detune_cents);
    tlv_put_u8(w, u->spread_pct);
    tlv_put_u8(w, u->blend_pct);
    tlv_put_u8(w, u->stereo);
    tlv_put_u8(w, u->bypass);
    tlv_put_u8(w, u->shift);
    tlv_put_u8(w, 0u);           /* retired semitone interval, kept in place */
    tlv_put_u8(w, u->stack);
    tlv_put_u8(w, u->phase_random);
    tlv_end_block(w, h);
}

/* Raw fields, clamped; count 0 (never set) survives the round trip. Seed: *u
 * as the caller left it (its record's fallback default); a field appended
 * here takes its value from this seed. */
static bool de_unison(tlv_reader_t *r, voice_unison_t *u)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    tlv_get_u8(&b, &u->count);
    tlv_get_u8(&b, &u->detune_cents);
    tlv_get_u8(&b, &u->spread_pct);
    tlv_get_u8(&b, &u->blend_pct);
    tlv_get_u8(&b, &u->stereo);
    tlv_get_u8(&b, &u->bypass);
    tlv_get_u8(&b, &u->shift);
    tlv_get_u8(&b, &u->interval_unused);
    tlv_get_u8(&b, &u->stack);
    tlv_get_u8(&b, &u->phase_random);
    u->count        = SEQ_CLAMP_U8(u->count, 0u, VOICE_UNISON_MAX_COPIES);
    u->detune_cents = SEQ_CLAMP_U8(u->detune_cents, 0u, VOICE_UNISON_MAX_DETUNE);
    u->spread_pct   = SEQ_CLAMP_U8(u->spread_pct, 0u, 100u);
    u->blend_pct    = SEQ_CLAMP_U8(u->blend_pct, 0u, 100u);
    u->stereo       = SEQ_CLAMP_U8(u->stereo, 0u, 1u);
    u->bypass       = SEQ_CLAMP_U8(u->bypass, 0u, 1u);
    u->shift        = SEQ_CLAMP_U8(u->shift, 0u, VOICE_UNISON_MAX_SHIFT);
    u->interval_unused = 0u;
    u->stack        = SEQ_CLAMP_U8(u->stack, 0u, VOICE_UNISON_MAX_STACK);
    u->phase_random = SEQ_CLAMP_U8(u->phase_random, 0u, 1u);
    return true;
}

/* ── GLOB section ─────────────────────────────────────────────────────────
 * The section scalars, one record per FX bus, then the trailing scalars
 * (transport stop, start mode, clock phrase). */

#define GLOB_VERSION 7

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
    uint8_t  stop_mode;     /* tstop_mode_t */
    uint8_t  stop_fade_idx;
    bool     silent_start;
    uint8_t  phrase_idx;    /* ui_clock phrase index */
} staged_glob_t;

/* GLOB's fallback default, composed from the owners' fallback values. */
static void glob_defaults(staged_glob_t *g)
{
    memset(g, 0, sizeof *g);
    g->bpm           = SEQ_DEFAULT_BPM;
    g->master_volume = AMY_FX_MASTER_VOLUME_DEFAULT;
    g->quant_enabled = SEQ_QUANTIZER_FALLBACK_ENABLED;
    g->quant_root    = SEQ_QUANTIZER_FALLBACK_ROOT_NOTE;
    g->quant_scale   = SEQ_QUANTIZER_FALLBACK_SCALE;
    g->drum_engine   = SEQ_DRUM_ENGINE_DEFAULT;
    for (uint8_t bus = 0; bus < FX_BUS_COUNT; bus++) amy_fx_bus_defaults(bus, &g->fx[bus]);
    g->presets_alter_global = AMY_FX_PRESETS_ALTER_GLOBAL_DEFAULT;
    g->split_flags   = FX_BUS_SPLIT_FLAGS_DEFAULT;
    transport_stop_params_t ts;
    transport_stop_defaults(&ts);
    g->stop_mode     = ts.mode;
    g->stop_fade_idx = ts.fade_idx;
    g->silent_start  = false;
    g->phrase_idx    = ui_clock_defaults();
}

static void ser_glob(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_GLOB, GLOB_VERSION);
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
        size_t hb = tlv_begin_block(w);
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
        tlv_end_block(w, hb);
    }
    tlv_put_u8(w, (uint8_t)transport_stop_get_mode());
    tlv_put_u8(w, transport_stop_get_fade_idx());
    tlv_put_u8(w, sequencer_core_get_silent_start() ? 1 : 0);
    tlv_put_u8(w, ui_clock_get_phrase_idx());
    tlv_end_section(w, h);
}

/* One FX bus record; seed: *f as parse_glob left it (amy_fx_bus_defaults). */
static bool parse_glob_bus(tlv_reader_t *r, fx_state_t *f)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    tlv_get_i8(&b, &f->eq_low_db);
    tlv_get_i8(&b, &f->eq_mid_db);
    tlv_get_i8(&b, &f->eq_high_db);
    tlv_get_u8(&b, &f->echo_level);
    tlv_get_u8(&b, &f->chorus_level);
    tlv_get_u8(&b, &f->reverb_level);
    tlv_get_i16(&b, &f->echo_delay_ms);
    tlv_get_i16(&b, &f->echo_feedback);
    tlv_get_i16(&b, &f->echo_tone);
    tlv_get_bool(&b, &f->echo_sync);
    tlv_get_u8(&b, &f->echo_div);
    if (f->echo_div >= NOTE_DIV_STD_COUNT) f->echo_div = NOTE_DIV_1_8D;
    tlv_get_i16(&b, &f->reverb_liveness);
    tlv_get_i16(&b, &f->reverb_damping);
    tlv_get_i16(&b, &f->reverb_xover_hz);
    tlv_get_i16(&b, &f->chorus_rate);
    tlv_get_i16(&b, &f->chorus_depth);
    tlv_get_u8(&b, &f->bus_dist_type);
    f->bus_dist_type &= 7;  /* stray data reads as a valid stage mask */
    tlv_get_u8(&b, &f->bus_dist_drive);
    tlv_get_u8(&b, &f->bus_dist_bits);
    tlv_get_u8(&b, &f->bus_dist_rate);
    tlv_get_u8(&b, &f->bus_dist_mix);
    tlv_get_u8(&b, &f->level);
    if (f->level > 200) f->level = 200;
    tlv_get_i16(&b, &f->chorus_delay);
    if (f->chorus_delay != FX_PARAM_UNSET)
        f->chorus_delay = (int16_t)SEQ_CLAMP_INT(f->chorus_delay, 16, 512);
    return true;
}

static bool parse_glob(tlv_reader_t *b, staged_glob_t *g)
{
    /* Seed: fallback default (glob_defaults); a field appended here takes
     * its value from this seed. */
    glob_defaults(g);

    tlv_get_u16(b, &g->bpm);
    tlv_get_f32(b, &g->master_volume);
    tlv_get_bool(b, &g->quant_enabled);
    tlv_get_u8(b, &g->quant_root);
    tlv_get_u8(b, &g->quant_scale);
    {
        uint8_t v = (uint8_t)g->drum_engine;
        tlv_get_u8(b, &v);
        g->drum_engine = (v == SEQ_DRUM_PCM) ? SEQ_DRUM_PCM : SEQ_DRUM_SYNTH;
    }
    tlv_get_bool(b, &g->presets_alter_global);
    tlv_get_u8(b, &g->split_flags);
    for (uint8_t bus = 0; bus < FX_BUS_COUNT; bus++) {
        if (!parse_glob_bus(b, &g->fx[bus])) return false;
    }
    tlv_get_u8(b, &g->stop_mode);
    if (g->stop_mode >= TSTOP_MODE_COUNT) g->stop_mode = TSTOP_MODE_COUNT - 1;
    tlv_get_u8(b, &g->stop_fade_idx);
    if (g->stop_fade_idx >= TSTOP_FADE_COUNT) g->stop_fade_idx = TSTOP_FADE_COUNT - 1;
    tlv_get_bool(b, &g->silent_start);
    tlv_get_u8(b, &g->phrase_idx);
    if (g->phrase_idx >= UI_CLOCK_PHRASE_COUNT) g->phrase_idx = UI_CLOCK_PHRASE_COUNT - 1;
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
    transport_stop_set_mode(g->stop_mode);
    transport_stop_set_fade_idx(g->stop_fade_idx);
    sequencer_core_set_silent_start(g->silent_start);
    ui_clock_set_phrase_idx(g->phrase_idx);
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
 * Records: the layer head, one per row (its unison and voice blocks nested),
 * the step arrays, then the note-FX tail (the layer's shared voice block
 * nested).
 * Out-of-range values clamp rather than reject, except the layer-count/type/
 * first-layer-is-drum invariants the caller enforces after all LAYR sections
 * have parsed. */

#define LAYR_VERSION 27

_Static_assert(SEQ_TRACKS == 5 && SEQ_MAX_STEPS == 32,
               "LAYR format assumes 5x32; bump LAYR_VERSION");

static void ser_layer(tlv_writer_t *w, const seq_layer_t *L)
{
    size_t h = tlv_begin_section(w, TAG_LAYR, LAYR_VERSION);
    size_t hb = tlv_begin_block(w);   /* head */
    tlv_put_u8(w, (uint8_t)L->type);
    tlv_put_u8(w, L->num_steps);
    tlv_put_u16(w, L->patch);
    tlv_put_u32(w, L->synth_flags);
    tlv_put_u8(w, L->num_voices);
    tlv_put_u8(w, L->chord_mode ? 1 : 0);
    tlv_put_u8(w, L->chord_root);
    tlv_put_u8(w, (uint8_t)L->chord_type);
    tlv_put_u8(w, L->swing_pct);
    tlv_end_block(w, hb);
    for (int t = 0; t < SEQ_TRACKS; t++) {
        hb = tlv_begin_block(w);   /* row t */
        tlv_put_u8(w, L->track_base_note[t]);
        tlv_put_u16(w, L->track_patch[t]);
        tlv_put_u16(w, L->track_pcm_preset[t]);
        tlv_put_u8(w, L->track_pcm_mode[t]);
        ser_unison(w, &L->track_unison[t]);
        tlv_put_u8(w, L->repeat_rate[t]);
        tlv_put_u8(w, L->mute[t] ? 1 : 0);
        tlv_put_u8(w, L->solo[t] ? 1 : 0);
        tlv_put_u8(w, L->follow[t]);
        tlv_put_u8(w, L->wt_frame[t]);
        ser_vp(w, &L->vp[t]);
        tlv_end_block(w, hb);
    }
    hb = tlv_begin_block(w);   /* step arrays */
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
    tlv_end_block(w, hb);
    hb = tlv_begin_block(w);   /* note FX + tail */
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
    tlv_end_block(w, hb);
    tlv_end_section(w, h);
}

static uint16_t clamp_patch(uint16_t patch)
{
    if (patch > SEQ_PATCH_FULL_MAX) return 0;
    if (sequencer_core_patch_compiled_out(patch)) return 0;
    return patch;
}

/* The LAYR record parsers below take parse_layer's seed. */

static bool parse_layer_head(tlv_reader_t *r, seq_layer_t *L)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    {
        uint8_t v = (uint8_t)L->type;
        tlv_get_u8(&b, &v);
        L->type = (v > SEQ_LAYER_MELODIC) ? SEQ_LAYER_MELODIC : (seq_layer_type_t)v;
    }
    tlv_get_u8(&b, &L->num_steps);
    if (L->num_steps != 16 && L->num_steps != 32) L->num_steps = 16;

    tlv_get_u16(&b, &L->patch);
    L->patch = clamp_patch(L->patch);

    tlv_get_u32(&b, &L->synth_flags);
    tlv_get_u8(&b, &L->num_voices);
    /* AMY's instrument_init() aborts outside 1..MAX_VOICES_PER_INSTRUMENT
     * (32); a CRC-valid but hand-edited file must not crash the load. */
    L->num_voices = SEQ_CLAMP_U8(L->num_voices, 1, 32);
    tlv_get_bool(&b, &L->chord_mode);
    tlv_get_u8(&b, &L->chord_root);
    {
        uint8_t v = (uint8_t)L->chord_type;
        tlv_get_u8(&b, &v);
        L->chord_type = (v >= CHORD_TYPE_COUNT) ? CHORD_MAJ : (chord_type_t)v;
    }
    tlv_get_u8(&b, &L->swing_pct);
    /* Bulk import bypasses sequencer_core_set_layer_swing(), so enforce the
     * engine ceiling here: swing-offset math requires the delay to stay
     * short of one full step. */
    if (L->swing_pct > SEQ_SWING_MAX) L->swing_pct = SEQ_SWING_MAX;
    return true;
}

static bool parse_layer_row(tlv_reader_t *r, seq_layer_t *L, int t)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    tlv_get_u8(&b, &L->track_base_note[t]);
    tlv_get_u16(&b, &L->track_patch[t]);
    L->track_patch[t] = clamp_patch(L->track_patch[t]);
    tlv_get_u16(&b, &L->track_pcm_preset[t]);
    tlv_get_u8(&b, &L->track_pcm_mode[t]);
    if (!de_unison(&b, &L->track_unison[t])) return false;
    tlv_get_u8(&b, &L->repeat_rate[t]);
    tlv_get_bool(&b, &L->mute[t]);
    tlv_get_bool(&b, &L->solo[t]);
    tlv_get_u8(&b, &L->follow[t]);
    if (L->follow[t] >= SEQ_FOLLOW_COUNT) L->follow[t] = (uint8_t)SEQ_FOLLOW_CHORD;
    tlv_get_u8(&b, &L->wt_frame[t]);
    if (L->wt_frame[t] > 64u) L->wt_frame[t] = 0u;
    return de_vp(&b, &L->vp[t]);
}

static bool parse_layer_steps(tlv_reader_t *r, seq_layer_t *L)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    tlv_get_bytes(&b, L->grid,               sizeof L->grid);
    tlv_get_bytes(&b, L->step_note,          sizeof L->step_note);
    tlv_get_bytes(&b, L->step_pitch_ofs,     sizeof L->step_pitch_ofs);
    tlv_get_bytes(&b, L->step_prob,          sizeof L->step_prob);
    tlv_get_bytes(&b, L->step_ratchet,       sizeof L->step_ratchet);
    tlv_get_bytes(&b, L->step_every,         sizeof L->step_every);
    tlv_get_bytes(&b, L->step_prev,          sizeof L->step_prev);
    tlv_get_bytes(&b, L->step_transform,     sizeof L->step_transform);
    tlv_get_bytes(&b, L->step_quant_bypass,  sizeof L->step_quant_bypass);
    tlv_get_bytes(&b, L->step_nudge,         sizeof L->step_nudge);
    tlv_get_bytes(&b, L->step_frame,         sizeof L->step_frame);
    tlv_get_bytes(&b, L->step_velocity_adj,  sizeof L->step_velocity_adj);
    tlv_get_bytes(&b, L->step_ratchet_taper, sizeof L->step_ratchet_taper);

    /* grid is bool and the file bytes went in raw: a byte other than 0 or 1
     * is not a valid bool, so fold them through a byte view before any bool
     * load reads one. */
    unsigned char *grid_bytes = (unsigned char *)L->grid;
    for (size_t i = 0; i < sizeof L->grid; i++) grid_bytes[i] = grid_bytes[i] != 0;

    for (int t = 0; t < SEQ_TRACKS; t++) {
        for (int s = 0; s < SEQ_MAX_STEPS; s++) {
            L->step_quant_bypass[t][s] = L->step_quant_bypass[t][s] ? 1 : 0;
            if (L->step_prob[t][s] > 100) L->step_prob[t][s] = 100;
            L->step_ratchet[t][s] = SEQ_CLAMP_U8(L->step_ratchet[t][s], 1, SEQ_MAX_RATCHET);
            L->step_every[t][s] = SEQ_CLAMP_U8(L->step_every[t][s], 1, SEQ_STEP_EVERY_MAX);
            L->step_prev[t][s]  = L->step_prev[t][s] ? 1 : 0;
            if (L->step_transform[t][s] >= SEQ_STEP_TRANSFORM_COUNT) L->step_transform[t][s] = SEQ_STEP_TRANSFORM_NONE;
            if (L->step_frame[t][s] > 64u) L->step_frame[t][s] = 0u;
        }
    }
    return true;
}

static bool parse_layer_tail(tlv_reader_t *r, seq_layer_t *L)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    /* Note FX, clamped to the live control ranges. */
    tlv_get_u16(&b, &L->gate_pct);
    tlv_get_u16(&b, &L->portamento_ms);
    if (L->gate_pct != SEQ_GATE_HOLD)
        L->gate_pct = SEQ_CLAMP_U16(L->gate_pct, SEQ_GATE_PCT_MIN, SEQ_GATE_PCT_MAX);
    if (L->portamento_ms > SEQ_MELODIC_PORTAMENTO_MAX_MS)
        L->portamento_ms = SEQ_MELODIC_PORTAMENTO_MAX_MS;

    tlv_get_u8(&b, &L->groove_pct);
    if (L->groove_pct > 100) L->groove_pct = 100;

    /* Live FM algorithm override; corrupt values fall back to none. The
     * configure path reasserts it after the load's patch apply; anything past
     * AMY's algorithm table would be an unchecked OOB index there. */
    tlv_get_u8(&b, &L->fm_algo_override);
    if (L->fm_algo_override != SEQ_FM_ALGO_NONE &&
        L->fm_algo_override >= amy_num_algorithms)
        L->fm_algo_override = SEQ_FM_ALGO_NONE;

    /* Per-row voice-block source + the layer's shared block. */
    for (int t = 0; t < SEQ_TRACKS; t++) tlv_get_u8(&b, &L->vp_src[t]);
    if (!de_vp(&b, &L->vp_layer)) return false;

    /* Melodic patch scope. */
    tlv_get_u8(&b, &L->patch_scope);
    tlv_get_u8(&b, &L->num_tracks);
    L->num_tracks = SEQ_CLAMP_U8(L->num_tracks, SEQ_TRACKS_DEFAULT, SEQ_TRACKS);
    return true;
}

/* Seed: fallback default (seq_layer_defaults) for the layer type the head
 * record names; a field appended to any LAYR record takes its value from this
 * seed. The rules that span records run after all four have parsed. */
static bool parse_layer(tlv_reader_t *b, seq_layer_t *L)
{
    /* The type is the head record's first field; a head too short to carry
     * it seeds a drum layer. */
    uint8_t type_raw = (uint8_t)SEQ_LAYER_DRUM;
    {
        tlv_reader_t peek = *b, hb;
        tlv_get_block(&peek, &hb);
        tlv_get_u8(&hb, &type_raw);
    }
    seq_layer_defaults((type_raw > SEQ_LAYER_MELODIC) ? SEQ_LAYER_MELODIC
                                                      : (seq_layer_type_t)type_raw, L);
    if (!parse_layer_head(b, L)) return false;
    for (int t = 0; t < SEQ_TRACKS; t++) {
        if (!parse_layer_row(b, L, t)) return false;
    }
    if (!parse_layer_steps(b, L)) return false;
    if (!parse_layer_tail(b, L)) return false;

    /* Note fields hold a playable pitch or (melodic only) a chord preset
     * sentinel (seq_chords.h). Anything else - corrupt, or a sentinel family
     * this build doesn't know - clamps into the playable range so it cannot
     * leak into pitch math. */
    bool mel = L->type == SEQ_LAYER_MELODIC;
    for (int t = 0; t < SEQ_TRACKS; t++) {
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

    /* The layer block is only meaningful on melodic layers; drum rows are
     * forced to TRACK so the bank seeding never resolves onto the shared
     * block. */
    for (int t = 0; t < SEQ_TRACKS; t++) {
        if (L->vp_src[t] > SEQ_VP_SRC_LAYER || !mel) L->vp_src[t] = SEQ_VP_SRC_TRACK;
    }

    if (L->patch_scope > SEQ_PATCH_SCOPE_TRACK)
        L->patch_scope = SEQ_PATCH_SCOPE_LAYER;
    if (!mel) L->patch_scope = SEQ_PATCH_SCOPE_LAYER;
    /* Invariant repair: in LAYER scope every melodic row plays `patch`, and
     * the configure path reads track_patch[] only. Cheap enough to do
     * unconditionally rather than trust the file. */
    if (mel && L->patch_scope == SEQ_PATCH_SCOPE_LAYER) {
        for (int t = 0; t < SEQ_TRACKS; t++) L->track_patch[t] = L->patch;
    }
    return true;
}

/* ── ARP section ──────────────────────────────────────────────────────────── */

/* The arp's record (arp_params_t, arp_core.h) plus its solo, which the
 * sequencer owns. */
typedef struct {
    arp_params_t arp;
    bool         solo;
} staged_arp_t;

/* Fallback default of the ARP section. */
static void arp_section_defaults(staged_arp_t *st)
{
    arp_params_defaults(&st->arp);
    st->solo = false;
}

/* One record: the arp's scalars, its slots as a raw array, and its env/
 * filter/lfo/dist/unison records nested. */
#define ARP_VERSION 18

static void ser_arp(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_ARP, ARP_VERSION);
    size_t hb = tlv_begin_block(w);
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
    seq_lfo_t l2; bool l2a = arp_get_lfo2(&l2); ser_lfo(w, &l2);
    tlv_put_u8(w, l2a ? 1 : 0);
    tlv_put_u8(w, (uint8_t)arp_get_quant_mode());
    seq_dist_t d; arp_get_dist(&d);      ser_dist(w, &d);
    tlv_put_u8(w, arp_get_muted() ? 1 : 0);
    tlv_put_u8(w, sequencer_core_get_arp_solo() ? 1 : 0);
    voice_unison_t u; arp_get_unison(&u); ser_unison(w, &u);
    tlv_put_u8(w, arp_get_repeat_rate());
    tlv_end_block(w, hb);
    tlv_end_section(w, h);
}

/* Seed: fallback default (arp_section_defaults); a field appended here takes
 * its value from this seed. */
static bool parse_arp(tlv_reader_t *r, staged_arp_t *st)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    arp_section_defaults(st);
    arp_params_t *a = &st->arp;
    uint8_t v;
    tlv_get_bool(&b, &a->enabled);
    tlv_get_u16(&b, &a->patch);
    a->patch = clamp_patch(a->patch);
    v = (uint8_t)a->dir;
    tlv_get_u8(&b, &v);
    a->dir = (v >= ARP_DIR_COUNT) ? ARP_UP : (arp_dir_t)v;
    tlv_get_u8(&b, &a->octaves);
    a->octaves = SEQ_CLAMP_U8(a->octaves, 1, ARP_OCT_MAX);
    v = (uint8_t)a->rate;
    tlv_get_u8(&b, &v);
    a->rate = (v >= NOTE_DIV_STD_COUNT) ? NOTE_DIV_1_4 : (note_div_t)v;
    tlv_get_u8(&b, &a->gate_pct);
    a->gate_pct = SEQ_CLAMP_U8(a->gate_pct, 10, 100);
    tlv_get_u8(&b, &a->scale);
    if (a->scale >= quantizer_scale_count()) a->scale = 0;
    tlv_get_u8(&b, &a->root);
    tlv_get_u16(&b, &a->portamento_ms);
    if (a->portamento_ms > ARP_PORTAMENTO_MAX_MS) a->portamento_ms = ARP_PORTAMENTO_MAX_MS;
    tlv_get_f32(&b, &a->amp_scale);
    a->amp_scale = SEQ_CLAMP_F32(a->amp_scale, 0.0f, 1.0f);
    for (int i = 0; i < ARP_MAX_SLOTS; i++) {
        tlv_get_i16(&b, &a->slots[i]);
        if (a->slots[i] < ARP_REST || a->slots[i] > 127) a->slots[i] = -1;
    }
    if (!de_env(&b, &a->env))       return false;
    if (!de_env(&b, &a->env2))      return false;
    if (!de_filter(&b, &a->filter)) return false;
    if (!de_lfo(&b, &a->lfo))       return false;
    if (!de_lfo(&b, &a->lfo2))      return false;
    a->lfo2.targets &= (uint8_t)~a->lfo.targets;   /* as de_vp */
    tlv_get_bool(&b, &a->lfo2_authored);
    /* Scale source (arp_quant_mode_t). */
    tlv_get_u8(&b, &a->quant_mode);
    if (a->quant_mode >= ARP_QUANT_COUNT) a->quant_mode = ARP_QUANT_CHORD;
    if (!de_dist(&b, &a->dist)) return false;
    tlv_get_bool(&b, &a->muted);
    tlv_get_bool(&b, &st->solo);
    if (!de_unison(&b, &a->unison)) return false;
    tlv_get_u8(&b, &a->repeat_rate);
    a->repeat_rate = (a->repeat_rate >= 8) ? 8 : (a->repeat_rate >= 4) ? 4
                   : (a->repeat_rate >= 2) ? 2 : 1;
    return true;
}

/* patch, then unison (both may rebuild the slot), first so later param
 * pushes land on the rebuilt slot; enabled, then mute and solo, LAST. */
static void apply_arp(const staged_arp_t *st)
{
    const arp_params_t *a = &st->arp;
    arp_set_patch(a->patch);
    arp_set_unison(&a->unison);
    arp_set_direction(a->dir);
    arp_set_octaves(a->octaves);
    arp_set_repeat_rate(a->repeat_rate);
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
    arp_set_lfo2(&a->lfo2, a->lfo2_authored);
    arp_set_dist(&a->dist);
    arp_set_enabled(a->enabled);
    arp_set_muted(a->muted);
    sequencer_core_set_arp_solo(st->solo);
}

/* ── DROP section ──────────────────────────────────────────────────────────
 * Records: the loop record (enabled, rate, loop_bars, octaves, seed,
 * seed_frozen, scale_index, root_note, sub_root_note, quant_mode, seg_count,
 * DROP_SEG_MAX segment records of 7 bytes nested, the jackpot cell map as a
 * raw DROP_JACKPOT_BYTES array, bounce), then one record per DROP and SUB
 * voice:
 * patch, glide, the voice_params_t block (ser_vp, authored flags included),
 * the unison block, muted, solo and gate_pct. */

#define DROP_VERSION 5

/* The drop's record (drop_params_t, drop_core.h) plus each voice's solo,
 * which the sequencer owns. */
typedef struct {
    drop_params_t drop;
    bool          solo[DROP_VOICE_COUNT];
} staged_drop_t;

/* Fallback default of the DROP section. */
static void drop_section_defaults(staged_drop_t *st)
{
    drop_params_defaults(&st->drop);
    memset(st->solo, 0, sizeof st->solo);
}

static void ser_drop(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_DROP, DROP_VERSION);
    size_t hb = tlv_begin_block(w);
    tlv_put_u8(w, drop_get_enabled() ? 1 : 0);
    tlv_put_u8(w, (uint8_t)drop_get_rate());
    tlv_put_u8(w, drop_get_loop_bars());
    tlv_put_u8(w, drop_get_octaves());
    tlv_put_u32(w, drop_get_seed());
    tlv_put_u8(w, drop_get_seed_frozen() ? 1 : 0);
    tlv_put_u8(w, drop_get_scale());
    tlv_put_u8(w, drop_get_root_note());
    tlv_put_u8(w, drop_get_sub_root_note());
    tlv_put_u8(w, (uint8_t)drop_get_quant_mode());
    tlv_put_u8(w, drop_get_seg_count());
    for (uint8_t i = 0; i < DROP_SEG_MAX; i++) {
        drop_seg_t s = { 0 };
        (void)drop_get_seg(i, &s);
        size_t hs = tlv_begin_block(w);
        tlv_put_u8(w, s.col);
        tlv_put_u8(w, s.row);
        tlv_put_u8(w, s.angle);
        tlv_put_u8(w, s.type);
        tlv_put_u8(w, s.ratio);
        tlv_put_i8(w, s.pitch_ofs);
        tlv_put_i8(w, s.vel_adj);
        tlv_end_block(w, hs);
    }
    uint8_t jp[DROP_JACKPOT_BYTES];
    drop_jackpot_get_map(jp);
    for (uint8_t i = 0; i < DROP_JACKPOT_BYTES; i++) tlv_put_u8(w, jp[i]);
    tlv_put_u8(w, drop_get_bounce());
    tlv_end_block(w, hb);
    for (uint8_t v = 0; v < DROP_VOICE_COUNT; v++) {
        drop_which_t which = (drop_which_t)v;
        hb = tlv_begin_block(w);
        tlv_put_u16(w, drop_get_patch(which));
        tlv_put_u16(w, drop_get_portamento_ms(which));
        voice_params_t vp;
        drop_get_vp(which, &vp);
        ser_vp(w, &vp);
        voice_unison_t u;
        drop_get_unison(which, &u);
        ser_unison(w, &u);
        tlv_put_u8(w, drop_get_muted(which) ? 1 : 0);
        tlv_put_u8(w, sequencer_core_get_drop_solo((uint8_t)which) ? 1 : 0);
        tlv_put_u8(w, drop_get_gate_pct(which));
        tlv_end_block(w, hb);
    }
    tlv_end_section(w, h);
}

/* The DROP record parsers below take parse_drop's seed. */

static bool parse_drop_seg(tlv_reader_t *r, drop_seg_t *s)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    tlv_get_u8(&b, &s->col);
    tlv_get_u8(&b, &s->row);
    tlv_get_u8(&b, &s->angle);
    tlv_get_u8(&b, &s->type);
    tlv_get_u8(&b, &s->ratio);
    tlv_get_i8(&b, &s->pitch_ofs);
    tlv_get_i8(&b, &s->vel_adj);
    /* A cell out of the field drops the segment (drop_set_seg refuses
     * it); the rest clamps there. */
    s->angle = SEQ_CLAMP_U8(s->angle, 0, DROP_SEG_ANGLES - 1);
    s->type  = SEQ_CLAMP_U8(s->type, 0, DROP_SEG_TYPE_COUNT - 1);
    s->ratio = SEQ_CLAMP_U8(s->ratio, 0, 100);
    s->pitch_ofs = (int8_t)SEQ_CLAMP_INT(s->pitch_ofs, -SEQ_STEP_PITCH_OFS_MAX, SEQ_STEP_PITCH_OFS_MAX);
    s->vel_adj   = (int8_t)SEQ_CLAMP_INT(s->vel_adj, -SEQ_STEP_VEL_ADJ_MAX, SEQ_STEP_VEL_ADJ_MAX);
    return true;
}

static bool parse_drop_voice(tlv_reader_t *r, drop_voice_params_t *dv, bool *solo)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    tlv_get_u16(&b, &dv->patch);
    dv->patch = clamp_patch(dv->patch);
    tlv_get_u16(&b, &dv->portamento_ms);
    if (dv->portamento_ms > DROP_PORTAMENTO_MAX_MS) dv->portamento_ms = DROP_PORTAMENTO_MAX_MS;
    if (!de_vp(&b, &dv->vp)) return false;
    if (!de_unison(&b, &dv->unison)) return false;
    tlv_get_bool(&b, &dv->muted);
    tlv_get_bool(&b, solo);
    tlv_get_u8(&b, &dv->gate_pct);
    dv->gate_pct = SEQ_CLAMP_U8(dv->gate_pct, 10, 100);
    return true;
}

/* Seed: fallback default (drop_section_defaults); a field appended to any
 * DROP record takes its value from this seed. */
static bool parse_drop(tlv_reader_t *r, staged_drop_t *st)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    drop_section_defaults(st);
    drop_params_t *d = &st->drop;
    uint8_t v;
    tlv_get_bool(&b, &d->enabled);
    v = (uint8_t)d->rate;
    tlv_get_u8(&b, &v);
    d->rate = (v >= NOTE_DIV_STD_COUNT) ? NOTE_DIV_1_4 : (note_div_t)v;
    tlv_get_u8(&b, &d->loop_bars);
    d->loop_bars = SEQ_CLAMP_U8(d->loop_bars, 1, DROP_LOOP_BARS_MAX);
    tlv_get_u8(&b, &d->octaves);
    d->octaves = SEQ_CLAMP_U8(d->octaves, 1, DROP_OCT_MAX);
    tlv_get_u32(&b, &d->seed);
    tlv_get_bool(&b, &d->seed_frozen);
    tlv_get_u8(&b, &d->scale);
    if (d->scale >= quantizer_scale_count()) d->scale = 0;
    tlv_get_u8(&b, &d->root);
    d->root = SEQ_CLAMP_U8(d->root, 0, 127);
    tlv_get_u8(&b, &d->sub_root);
    d->sub_root = SEQ_CLAMP_U8(d->sub_root, 0, 127);
    tlv_get_u8(&b, &d->quant_mode);
    if (d->quant_mode >= ARP_QUANT_COUNT) d->quant_mode = ARP_QUANT_CHORD;
    tlv_get_u8(&b, &d->seg_count);
    if (d->seg_count > DROP_SEG_MAX) d->seg_count = DROP_SEG_MAX;
    for (uint8_t i = 0; i < DROP_SEG_MAX; i++) {
        if (!parse_drop_seg(&b, &d->segs[i])) return false;
    }
    tlv_get_bytes(&b, d->jackpot, sizeof d->jackpot);
    tlv_get_u8(&b, &d->bounce);
    d->bounce = SEQ_CLAMP_U8(d->bounce, DROP_BOUNCE_MIN, DROP_BOUNCE_MAX);
    for (uint8_t i = 0; i < DROP_VOICE_COUNT; i++) {
        if (!parse_drop_voice(r, &d->voice[i], &st->solo[i])) return false;
    }
    return true;
}

/* Voices first (each rebuilds its slot), then the loop parameters and the
 * field (each marks dirty), enabled, then mute and solo last, as apply_arp. */
static void apply_drop(const staged_drop_t *st)
{
    const drop_params_t *d = &st->drop;
    for (uint8_t i = 0; i < DROP_VOICE_COUNT; i++) {
        const drop_voice_params_t *dv = &d->voice[i];
        drop_set_voice((drop_which_t)i, dv->patch, dv->portamento_ms, &dv->vp, &dv->unison);
    }
    drop_set_rate(d->rate);
    drop_set_loop_bars(d->loop_bars);
    drop_set_octaves(d->octaves);
    drop_set_bounce(d->bounce);
    for (uint8_t i = 0; i < DROP_VOICE_COUNT; i++) {
        drop_set_gate_pct((drop_which_t)i, d->voice[i].gate_pct);
    }
    drop_set_seed(d->seed);
    drop_set_seed_frozen(d->seed_frozen);
    drop_set_scale(d->scale);
    drop_set_root_note(d->root);
    drop_set_sub_root_note(d->sub_root);
    drop_set_quant_mode((arp_quant_mode_t)d->quant_mode);
    /* The field is replaced: every cell cleared, then the file's segments. */
    for (uint8_t i = drop_get_seg_count(); i > 0; i--) {
        drop_seg_t s;
        if (drop_get_seg((uint8_t)(i - 1), &s)) drop_remove_seg(s.col, s.row);
    }
    for (uint8_t i = 0; i < d->seg_count; i++) (void)drop_set_seg(&d->segs[i]);
    drop_jackpot_set_map(d->jackpot);
    drop_set_enabled(d->enabled);
    for (uint8_t i = 0; i < DROP_VOICE_COUNT; i++) {
        drop_set_muted((drop_which_t)i, d->voice[i].muted);
        sequencer_core_set_drop_solo(i, st->solo[i]);
    }
}

/* ── DRON section ─────────────────────────────────────────────────────────── */

typedef drone_params_t staged_drone_t;   /* drone_core.h */

/* Records: the drone's scalars, then its voice block. */
#define DRON_VERSION 8

static void ser_drone(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_DRON, DRON_VERSION);
    size_t hb = tlv_begin_block(w);
    tlv_put_u8(w, drone_get_enabled() ? 1 : 0);
    tlv_put_u8(w, (uint8_t)drone_get_chord());
    tlv_put_u8(w, drone_get_root_note());
    tlv_put_u16(w, drone_get_patch());
    tlv_put_f32(w, drone_get_amp_peak());
    tlv_put_f32(w, drone_get_amp_duck());
    tlv_put_u8(w, (uint8_t)drone_get_rate());
    tlv_put_u8(w, drone_get_sub_enabled() ? 1 : 0);
    tlv_put_i8(w, drone_get_sub_interval());
    tlv_put_f32(w, drone_get_gate_len());
    tlv_put_u8(w, drone_get_swing());
    tlv_put_f32(w, drone_get_blip());
    tlv_put_u8(w, (uint8_t)drone_get_pattern());
    tlv_put_u8(w, (uint8_t)drone_get_follow());
    voice_unison_t u; drone_get_unison(&u); ser_unison(w, &u);
    tlv_end_block(w, hb);
    voice_params_t vp; drone_get_voice_params(&vp); ser_vp(w, &vp);
    tlv_end_section(w, h);
}

/* Seed: fallback default (drone_params_defaults); a field appended to either
 * record takes its value from this seed. */
static bool parse_drone(tlv_reader_t *r, staged_drone_t *d)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    drone_params_defaults(d);
    uint8_t v;
    tlv_get_bool(&b, &d->enabled);
    v = (uint8_t)d->chord;
    tlv_get_u8(&b, &v);
    d->chord = (v >= CHORD_TYPE_COUNT) ? CHORD_MAJ : (chord_type_t)v;
    tlv_get_u8(&b, &d->root);
    tlv_get_u16(&b, &d->patch);
    d->patch = clamp_patch(d->patch);
    tlv_get_f32(&b, &d->amp_peak);
    d->amp_peak = SEQ_CLAMP_F32(d->amp_peak, 0.0f, 1.0f);
    tlv_get_f32(&b, &d->amp_duck);
    d->amp_duck = SEQ_CLAMP_F32(d->amp_duck, 0.0f, 1.0f);
    v = (uint8_t)d->rate;
    tlv_get_u8(&b, &v);
    d->rate = (v >= NOTE_DIV_STD_COUNT) ? NOTE_DIV_1_4 : (note_div_t)v;
    tlv_get_bool(&b, &d->sub_enabled);
    tlv_get_i8(&b, &d->sub_interval);
    d->sub_interval = (int8_t)SEQ_CLAMP_INT(d->sub_interval, DRONE_SUB_INTERVAL_MIN,
                                            DRONE_SUB_INTERVAL_MAX);
    tlv_get_f32(&b, &d->gate_len);
    d->gate_len = SEQ_CLAMP_F32(d->gate_len, 0.05f, 0.95f);
    tlv_get_u8(&b, &d->swing);
    if (d->swing > 66) d->swing = 66;   /* mirrors the private SEQ/DRONE_SWING_MAX ceiling */
    tlv_get_f32(&b, &d->blip);
    d->blip = SEQ_CLAMP_F32(d->blip, 0.0f, 1.0f);
    v = (uint8_t)d->pattern;
    tlv_get_u8(&b, &v);
    d->pattern = (v >= DRONE_PAT_COUNT) ? DRONE_PAT_FULL : (drone_pattern_t)v;
    v = (uint8_t)d->follow;
    tlv_get_u8(&b, &v);
    d->follow = (v >= DRONE_FOLLOW_COUNT) ? DRONE_FOLLOW_OFF : (drone_follow_t)v;
    if (!de_unison(&b, &d->unison)) return false;
    return de_vp(r, &d->vp);
}

/* enabled LAST; patch is skipped (leaving the live patch) when the value
 * falls outside the stutter drone's domain. */
static void apply_drone(const staged_drone_t *d)
{
    drone_set_chord(d->chord);
    drone_set_root_note(d->root);
    if (!drone_stutter_patch_excluded(d->patch)) drone_set_patch(d->patch);
    drone_set_amp_peak(d->amp_peak);
    drone_set_amp_duck(d->amp_duck);
    drone_set_rate(d->rate);
    drone_set_sub_interval(d->sub_interval);
    drone_set_sub_enabled(d->sub_enabled);
    drone_set_gate_len(d->gate_len);
    drone_set_swing(d->swing);
    drone_set_blip(d->blip);
    drone_set_pattern(d->pattern);
    drone_set_follow(d->follow);
    drone_set_unison(&d->unison);
    drone_set_voice_params(&d->vp);
    drone_set_enabled(d->enabled);
}

/* ── DSTD section (normal drone) ──────────────────────────────────────────── */

typedef drone_std_params_t staged_drone_std_t;   /* drone_std_core.h */

/* Records as DRON: the scalars, then the voice block. */
#define DSTD_VERSION 5

static void ser_drone_std(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_DSTD, DSTD_VERSION);
    size_t hb = tlv_begin_block(w);
    tlv_put_u8(w, drone_std_get_enabled() ? 1 : 0);
    tlv_put_u8(w, (uint8_t)drone_std_get_chord());
    tlv_put_u8(w, drone_std_get_root_note());
    tlv_put_u8(w, (uint8_t)drone_std_get_follow());
    tlv_put_f32(w, drone_std_get_level());
    tlv_put_u16(w, drone_std_get_patch());
    tlv_put_u8(w, drone_std_get_sub_enabled() ? 1 : 0);
    tlv_put_i8(w, drone_std_get_sub_interval());
    voice_unison_t u; drone_std_get_unison(&u); ser_unison(w, &u);
    tlv_end_block(w, hb);
    voice_params_t vp; drone_std_get_voice_params(&vp); ser_vp(w, &vp);
    tlv_end_section(w, h);
}

/* Seed: fallback default (drone_std_params_defaults); a field appended to
 * either record takes its value from this seed. */
static bool parse_drone_std(tlv_reader_t *r, staged_drone_std_t *d)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    drone_std_params_defaults(d);
    uint8_t v;
    tlv_get_bool(&b, &d->enabled);
    v = (uint8_t)d->chord;
    tlv_get_u8(&b, &v);
    d->chord = (v >= CHORD_TYPE_COUNT) ? CHORD_MAJ : (chord_type_t)v;
    tlv_get_u8(&b, &d->root);
    v = (uint8_t)d->follow;
    tlv_get_u8(&b, &v);
    d->follow = (v >= DRONE_FOLLOW_COUNT) ? DRONE_FOLLOW_OFF : (drone_follow_t)v;
    tlv_get_f32(&b, &d->level);
    d->level = SEQ_CLAMP_F32(d->level, 0.0f, 1.0f);
    tlv_get_u16(&b, &d->patch);
    d->patch = clamp_patch(d->patch);
    tlv_get_bool(&b, &d->sub_enabled);
    tlv_get_i8(&b, &d->sub_interval);
    d->sub_interval = (int8_t)SEQ_CLAMP_INT(d->sub_interval, DRONE_SUB_INTERVAL_MIN,
                                            DRONE_SUB_INTERVAL_MAX);
    if (!de_unison(&b, &d->unison)) return false;
    return de_vp(r, &d->vp);
}

/* As apply_drone(): enabled LAST, an unplayable patch leaves the live one. */
static void apply_drone_std(const staged_drone_std_t *d)
{
    drone_std_set_chord(d->chord);
    drone_std_set_root_note(d->root);
    drone_std_set_follow(d->follow);
    drone_std_set_level(d->level);
    if (!drone_patch_excluded(d->patch)) drone_std_set_patch(d->patch);
    drone_std_set_sub_interval(d->sub_interval);
    drone_std_set_sub_enabled(d->sub_enabled);
    drone_std_set_unison(&d->unison);
    drone_std_set_voice_params(&d->vp);
    drone_std_set_enabled(d->enabled);
}

/* ── CLIP section ─────────────────────────────────────────────────────────── */

/* The bounce shape and each clip slot's settings. Clip audio is not
 * persisted (a clip is lost on power cycle); a load shows the slots empty
 * but configured. */
typedef struct {
    clip_bounce_shape_t  shape;                  /* clip_bounce.h */
    clip_player_params_t slot[CLIP_SLOT_COUNT];  /* clip_player.h */
} staged_clip_t;

/* Fallback default of the CLIP section. */
static void clip_section_defaults(staged_clip_t *c)
{
    clip_bounce_params_defaults(&c->shape);
    for (uint8_t s = 0; s < CLIP_SLOT_COUNT; s++) clip_player_params_defaults(&c->slot[s]);
}

/* One record: the shape, one record per clip slot nested, then after. */
#define CLIP_VERSION 3

static void ser_clip(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_CLIP, CLIP_VERSION);
    size_t hb = tlv_begin_block(w);
    clip_bounce_shape_t sh;
    clip_bounce_get_shape(&sh);
    tlv_put_u8(w, sh.max_bars);
    tlv_put_u8(w, sh.stereo ? 1 : 0);
    tlv_put_u8(w, (uint8_t)sh.tail);
    tlv_put_u8(w, CLIP_SLOT_COUNT);
    for (uint8_t s = 0; s < CLIP_SLOT_COUNT; s++) {
        size_t hs = tlv_begin_block(w);
        tlv_put_u8(w, clip_player_get_level(s));
        tlv_put_u8(w, (uint8_t)clip_player_get_tempo_mode(s));
        tlv_end_block(w, hs);
    }
    tlv_put_u8(w, (uint8_t)sh.after);
    tlv_end_block(w, hb);
    tlv_end_section(w, h);
}

/* Seed: fallback default (clip_section_defaults); a field appended to either
 * record takes its value from this seed. Slots beyond this build's count are
 * consumed but not stored. */
static bool parse_clip(tlv_reader_t *r, staged_clip_t *c)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    clip_section_defaults(c);
    uint8_t nslots = CLIP_SLOT_COUNT, v;
    tlv_get_u8(&b, &c->shape.max_bars);
    tlv_get_bool(&b, &c->shape.stereo);
    v = (uint8_t)c->shape.tail;
    tlv_get_u8(&b, &v);
    c->shape.tail = (v > CLIP_TAIL_BAR) ? CLIP_TAIL_BAR : (clip_tail_t)v;
    tlv_get_u8(&b, &nslots);
    for (uint8_t s = 0; s < nslots; s++) {
        tlv_reader_t sb;
        tlv_get_block(&b, &sb);
        if (s >= CLIP_SLOT_COUNT) continue;
        clip_player_params_t *cp = &c->slot[s];
        tlv_get_u8(&sb, &cp->level);
        if (cp->level > 100) cp->level = 100;
        v = (uint8_t)cp->mode;
        tlv_get_u8(&sb, &v);
        cp->mode = (v > CLIP_TEMPO_VARI) ? CLIP_TEMPO_STRETCH : (clip_tempo_mode_t)v;
    }
    v = (uint8_t)c->shape.after;
    tlv_get_u8(&b, &v);
    c->shape.after = (v > CLIP_AFTER_CLEAR) ? CLIP_AFTER_MUTE : (clip_after_t)v;
    return true;
}

static void apply_clip(const staged_clip_t *c)
{
    clip_bounce_set_shape(&c->shape);   /* clamps bars to the 1/2/4/8/16 list */
    for (uint8_t s = 0; s < CLIP_SLOT_COUNT; s++) {
        clip_player_set_level(s, c->slot[s].level);
        clip_player_set_tempo_mode(s, c->slot[s].mode);
    }
}

/* ── MIXR section ─────────────────────────────────────────────────────────── */

/* Mixer positions, soft mutes, the FADE length and link groups (mixer.h).
 * Layer arrays are index-aligned with the LAYR sections; master is GLOB's
 * master volume, not stored here (its link group is). A fade in progress is
 * not stored. One record, the track_pos row of each layer a record nested
 * in it; the link groups are flat u8s at its tail. */
#define MIXR_VERSION 3

typedef mixer_snapshot_t staged_mixr_t;

static void ser_mixr(tlv_writer_t *w)
{
    mixer_snapshot_t m;
    mixer_export(&m);
    size_t h = tlv_begin_section(w, TAG_MIXR, MIXR_VERSION);
    size_t hb = tlv_begin_block(w);
    for (uint8_t L = 0; L < MAX_LAYERS; L++) tlv_put_u8(w, m.layer_pos[L]);
    tlv_put_u8(w, m.layer_mute);
    for (uint8_t L = 0; L < MAX_LAYERS; L++) {
        size_t hr = tlv_begin_block(w);
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) tlv_put_u8(w, m.track_pos[L][t]);
        tlv_end_block(w, hr);
    }
    for (uint8_t L = 0; L < MAX_LAYERS; L++) tlv_put_u8(w, m.track_mute[L]);
    for (uint8_t k = 0; k < MIX_MISC_COUNT; k++) tlv_put_u8(w, m.misc_pos[k]);
    tlv_put_u8(w, m.misc_mute);
    tlv_put_u8(w, m.fade_bars_idx);
    tlv_put_u8(w, m.master_link);
    for (uint8_t L = 0; L < MAX_LAYERS; L++) tlv_put_u8(w, m.layer_link[L]);
    for (uint8_t L = 0; L < MAX_LAYERS; L++) {
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) tlv_put_u8(w, m.track_link[L][t]);
    }
    for (uint8_t k = 0; k < MIX_MISC_COUNT; k++) tlv_put_u8(w, m.misc_link[k]);
    tlv_end_block(w, hb);
    tlv_end_section(w, h);
}

static void parse_mixr_pos(tlv_reader_t *b, uint8_t *out)
{
    tlv_get_u8(b, out);
    if (*out > MIXER_POS_MAX) *out = MIXER_POS_MAX;
}

static void parse_mixr_link(tlv_reader_t *b, uint8_t *out)
{
    tlv_get_u8(b, out);
    if (*out > MIXER_LINK_COUNT) *out = MIXER_LINK_COUNT;
}

/* Seed: fallback default (mixer_params_defaults); a field appended to either
 * record takes its value from this seed. */
static bool parse_mixr(tlv_reader_t *r, staged_mixr_t *m)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    mixer_params_defaults(m);
    for (uint8_t L = 0; L < MAX_LAYERS; L++) parse_mixr_pos(&b, &m->layer_pos[L]);
    tlv_get_u8(&b, &m->layer_mute);
    m->layer_mute &= (uint8_t)((1u << MAX_LAYERS) - 1u);
    for (uint8_t L = 0; L < MAX_LAYERS; L++) {
        tlv_reader_t rb;
        tlv_get_block(&b, &rb);
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) parse_mixr_pos(&rb, &m->track_pos[L][t]);
    }
    for (uint8_t L = 0; L < MAX_LAYERS; L++) {
        tlv_get_u8(&b, &m->track_mute[L]);
        m->track_mute[L] &= (uint8_t)((1u << SEQ_TRACKS) - 1u);
    }
    for (uint8_t k = 0; k < MIX_MISC_COUNT; k++) parse_mixr_pos(&b, &m->misc_pos[k]);
    tlv_get_u8(&b, &m->misc_mute);
    m->misc_mute &= (uint8_t)((1u << MIX_MISC_COUNT) - 1u);
    tlv_get_u8(&b, &m->fade_bars_idx);
    if (m->fade_bars_idx > MIXER_FADE_BARS_COUNT - 1) m->fade_bars_idx = MIXER_FADE_BARS_COUNT - 1;
    parse_mixr_link(&b, &m->master_link);
    for (uint8_t L = 0; L < MAX_LAYERS; L++) parse_mixr_link(&b, &m->layer_link[L]);
    for (uint8_t L = 0; L < MAX_LAYERS; L++) {
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) parse_mixr_link(&b, &m->track_link[L][t]);
    }
    for (uint8_t k = 0; k < MIX_MISC_COUNT; k++) parse_mixr_link(&b, &m->misc_link[k]);
    return true;
}

/* After the layer imports: the importer resets layers past the live count. */
static void apply_mixr(const staged_mixr_t *m)
{
    mixer_import(m);
}

/* ── PROG section ─────────────────────────────────────────────────────────── */

/* Entries beyond the progression's capacity
 * (sequencer_core_progression_get_max()) are consumed from the stream but
 * not stored. */
typedef seq_prog_params_t staged_prog_t;   /* sequencer_core.h */

/* Records: enabled and the entry count, then one record per entry. */
#define PROG_VERSION 2

static void ser_prog(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_PROG, PROG_VERSION);
    size_t hb = tlv_begin_block(w);
    tlv_put_u8(w, sequencer_core_progression_get_enabled() ? 1 : 0);
    uint8_t count = sequencer_core_progression_get_count();
    tlv_put_u8(w, count);
    tlv_end_block(w, hb);
    for (uint8_t i = 0; i < count; i++) {
        uint8_t root, bars; chord_type_t ct;
        sequencer_core_progression_get_entry(i, &root, &ct, &bars);
        hb = tlv_begin_block(w);
        tlv_put_u8(w, root);
        tlv_put_u8(w, (uint8_t)ct);
        tlv_put_u8(w, bars);
        tlv_end_block(w, hb);
    }
    tlv_end_section(w, h);
}

/* Seed: fallback default (seq_prog_params_defaults); a field appended to
 * either record takes its value from this seed. */
static bool parse_prog(tlv_reader_t *r, staged_prog_t *p)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    seq_prog_params_defaults(p);
    uint8_t raw_count = p->count;
    tlv_get_bool(&b, &p->enabled);
    tlv_get_u8(&b, &raw_count);

    uint8_t max_entries = sequencer_core_progression_get_max();
    if (max_entries > PROG_GEN_MAX_ENTRIES) max_entries = PROG_GEN_MAX_ENTRIES;
    p->count = (raw_count > max_entries) ? max_entries : raw_count;

    for (uint8_t i = 0; i < raw_count; i++) {
        tlv_reader_t eb;
        tlv_get_block(r, &eb);
        if (i >= p->count) continue;
        seq_prog_entry_t *e = &p->entries[i];
        uint8_t ctraw = (uint8_t)e->chord_type;
        tlv_get_u8(&eb, &e->root);
        tlv_get_u8(&eb, &ctraw);
        tlv_get_u8(&eb, &e->duration_bars);
        e->chord_type = (ctraw >= CHORD_TYPE_COUNT) ? CHORD_MAJ : (chord_type_t)ctraw;
    }
    return true;
}

static void apply_prog(const staged_prog_t *p)
{
    sequencer_core_progression_set_count(p->count);
    for (uint8_t i = 0; i < p->count; i++) {
        const seq_prog_entry_t *e = &p->entries[i];
        sequencer_core_progression_set_entry(i, e->root, e->chord_type, e->duration_bars);
    }
    sequencer_core_progression_set_enabled(p->enabled);
}

/* ── PGEN section ─────────────────────────────────────────────────────────── */

/* The progression generator's settings. The chords they produced are the PROG
 * section above; this is what the Prog Gen page shows for them. */
typedef struct {
    prog_gen_params_t params;
} staged_pgen_t;

#define PGEN_VERSION 2

static void ser_pgen(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_PGEN, PGEN_VERSION);
    size_t hb = tlv_begin_block(w);
    prog_gen_params_t p;
    sequencer_core_progression_gen_params_get(&p);
    tlv_put_u8(w, p.style);
    tlv_put_u8(w, p.len);
    tlv_put_u8(w, p.bars);
    tlv_put_u8(w, p.ext);
    tlv_put_u8(w, p.var);
    tlv_put_u16(w, p.seed);
    tlv_end_block(w, hb);
    tlv_end_section(w, h);
}

/* Seed: fallback default (prog_gen_params_defaults); a field appended here
 * takes its value from this seed. No clamping here: the generator clamps
 * every field on use. */
static bool parse_pgen(tlv_reader_t *r, staged_pgen_t *g)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    prog_gen_params_defaults(&g->params);
    tlv_get_u8(&b, &g->params.style);
    tlv_get_u8(&b, &g->params.len);
    tlv_get_u8(&b, &g->params.bars);
    tlv_get_u8(&b, &g->params.ext);
    tlv_get_u8(&b, &g->params.var);
    tlv_get_u16(&b, &g->params.seed);
    return true;
}

static void apply_pgen(const staged_pgen_t *g)
{
    sequencer_core_progression_gen_params_set(&g->params);
}

/* ── WTCU section (custom wavetable, CONFIG_SYNTH_CUSTOM_WT) ───────────────
 * One record: the builder's sixteen parameter bytes in wt_params_t order
 * (shape, width, bright, sync, peak per keyframe A, M, B, then range); the
 * table itself is never stored, the load rebuilds it. Written only when the
 * builder is compiled in; a build without it skips the section as unknown. */
#if CONFIG_SYNTH_CUSTOM_WT
#define WTCU_VERSION 3

static void ser_wtcu(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_WTCU, WTCU_VERSION);
    size_t hb = tlv_begin_block(w);
    wt_params_t p;
    wt_builder_get_params(&p);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_put_u8(w, p.shape[k]);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_put_u8(w, p.width[k]);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_put_u8(w, p.bright[k]);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_put_u8(w, p.sync[k]);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_put_u8(w, p.peak[k]);
    tlv_put_u8(w, p.range);
    tlv_end_block(w, hb);
    tlv_end_section(w, h);
}

/* Seed: fallback default (wt_params_default); a field appended here takes
 * its value from this seed. Clamped here (crash-safety for foreign data); the builder
 * clamps again. */
static bool parse_wtcu(tlv_reader_t *r, wt_params_t *p)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    wt_params_default(p);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_get_u8(&b, &p->shape[k]);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_get_u8(&b, &p->width[k]);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_get_u8(&b, &p->bright[k]);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_get_u8(&b, &p->sync[k]);
    for (uint8_t k = 0; k < WT_KEYS; k++) tlv_get_u8(&b, &p->peak[k]);
    tlv_get_u8(&b, &p->range);
    wt_params_clamp(p);
    return true;
}
#endif

/* ── FMCU section (custom FM voice, CONFIG_SYNTH_CUSTOM_FM) ────────────────
 * One record: the fm_voice_t of patch 276 (fm_voice.h): algorithm and fb_op,
 * then one record per operator (target mask, ratio, fixed Hz, level and its
 * envelope record R1..R4, L1..L4, eg_type), then feedback and the op_fixed
 * mask.
 * op_mute is audition state and stays out. Written only when the FM voice is
 * compiled in; a build without it skips the section as unknown. */
#if CONFIG_SYNTH_CUSTOM_FM
#define FMCU_VERSION 2

static void ser_fm_op_env(tlv_writer_t *w, const fm_op_env_t *e)
{
    size_t h = tlv_begin_block(w);
    for (uint8_t s = 0; s < 4; s++) tlv_put_u8(w, e->rate[s]);
    for (uint8_t s = 0; s < 4; s++) tlv_put_u8(w, e->level[s]);
    tlv_put_u8(w, e->eg_type);
    tlv_end_block(w, h);
}

/* Seed: *e as parse_fmcu left it (fm_voice_default); fm_voice_clamp clamps
 * the whole voice afterwards. */
static bool de_fm_op_env(tlv_reader_t *r, fm_op_env_t *e)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    for (uint8_t s = 0; s < 4; s++) tlv_get_u8(&b, &e->rate[s]);
    for (uint8_t s = 0; s < 4; s++) tlv_get_u8(&b, &e->level[s]);
    tlv_get_u8(&b, &e->eg_type);
    return true;
}

static void ser_fmcu(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_FMCU, FMCU_VERSION);
    size_t hb = tlv_begin_block(w);
    const fm_voice_t *v = &s_fm_voice;
    tlv_put_u8(w, v->algorithm);
    tlv_put_u8(w, v->fb_op);
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        size_t ho = tlv_begin_block(w);
        tlv_put_u8(w, v->op_targets[i]);
        tlv_put_f32(w, v->op_ratio[i]);
        tlv_put_f32(w, v->op_fixed_hz[i]);
        tlv_put_f32(w, v->op_level[i]);
        ser_fm_op_env(w, &v->op_env[i]);
        tlv_end_block(w, ho);
    }
    tlv_put_f32(w, v->feedback);
    tlv_put_u8(w, v->op_fixed);
    tlv_end_block(w, hb);
    tlv_end_section(w, h);
}

/* Seed: fallback default (fm_voice_default); a field appended to either
 * record takes its value from this seed. Clamped here (crash-safety for foreign data);
 * fm_voice_clamp also reseeds a custom routing that would not compile, so
 * the voice always pushes. */
static bool parse_fmcu(tlv_reader_t *r, fm_voice_t *v)
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    fm_voice_default(v);
    tlv_get_u8(&b, &v->algorithm);
    tlv_get_u8(&b, &v->fb_op);
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        tlv_reader_t ob;
        tlv_get_block(&b, &ob);
        tlv_get_u8(&ob, &v->op_targets[i]);
        tlv_get_f32(&ob, &v->op_ratio[i]);
        tlv_get_f32(&ob, &v->op_fixed_hz[i]);
        tlv_get_f32(&ob, &v->op_level[i]);
        if (!de_fm_op_env(&ob, &v->op_env[i])) return false;
    }
    tlv_get_f32(&b, &v->feedback);
    tlv_get_u8(&b, &v->op_fixed);
    fm_voice_clamp(v);
    return true;
}
#endif

/* ── CHRD section (chord presets, seq_chords.h) ──────────────────────────
 * Records: the u8 slot count, then one record per slot: u8 tone count +
 * SEQ_CHORD_MAX_NOTES note bytes (fixed width; a wider voicing bumps the
 * version). Firmware without CHRD skips the unknown section: chords vanish
 * there and any chord sentinel in that file's LAYR notes range-clamps to a
 * top-of-range note, no crash. */

#define CHRD_VERSION 2

static void ser_chrd(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_CHRD, CHRD_VERSION);
    size_t hb = tlv_begin_block(w);
    tlv_put_u8(w, SEQ_CHORD_SLOTS);
    tlv_end_block(w, hb);
    for (uint8_t s = 0; s < SEQ_CHORD_SLOTS; s++) {
        seq_chord_t c;
        if (!seq_chords_get(s, &c)) memset(&c, 0, sizeof c);
        hb = tlv_begin_block(w);
        tlv_put_u8(w, c.count);
        for (uint8_t i = 0; i < SEQ_CHORD_MAX_NOTES; i++) {
            tlv_put_u8(w, c.notes[i]);
        }
        tlv_end_block(w, hb);
    }
    tlv_end_section(w, h);
}

/* Seed: fallback default (seq_chords_params_defaults); a field appended to
 * either record takes its value from this seed. */
static bool parse_chrd(tlv_reader_t *r, seq_chord_t slots[SEQ_CHORD_SLOTS])
{
    tlv_reader_t b;
    tlv_get_block(r, &b);
    seq_chords_params_defaults(slots);
    uint8_t nslots = 0;
    tlv_get_u8(&b, &nslots);
    for (uint8_t s = 0; s < nslots; s++) {
        tlv_reader_t sb;
        tlv_get_block(r, &sb);
        if (s >= SEQ_CHORD_SLOTS) continue;
        /* count is advisory: seq_chords_import re-derives it from the
         * non-zero tones, which also normalizes hand-edited files. */
        tlv_get_u8(&sb, &slots[s].count);
        for (uint8_t i = 0; i < SEQ_CHORD_MAX_NOTES; i++) {
            tlv_get_u8(&sb, &slots[s].notes[i]);
        }
        if (slots[s].count > SEQ_CHORD_MAX_NOTES) slots[s].count = SEQ_CHORD_MAX_NOTES;
    }
    return true;
}

/* ── Section dispatch + partial-load report ───────────────────────────────── */

static const char *const k_sect_name[PROJECT_SECT_COUNT] = {
    [PROJECT_SECT_GLOB] = "GLOB", [PROJECT_SECT_LAYR] = "LAYR", [PROJECT_SECT_ARP]  = "ARP",
    [PROJECT_SECT_DROP] = "DROP", [PROJECT_SECT_DRON] = "DRON", [PROJECT_SECT_DSTD] = "DSTD",
    [PROJECT_SECT_PROG] = "PROG", [PROJECT_SECT_CHRD] = "CHRD", [PROJECT_SECT_CLIP] = "CLIP",
    [PROJECT_SECT_MIXR] = "MIXR", [PROJECT_SECT_PGEN] = "PGEN", [PROJECT_SECT_WTCU] = "WTCU",
    [PROJECT_SECT_FMCU] = "FMCU",
};

/* This build's version of each section; 0 for one compiled out (its tag is
 * then unknown to the dispatcher). */
static const uint8_t k_sect_ver[PROJECT_SECT_COUNT] = {
    [PROJECT_SECT_GLOB] = GLOB_VERSION, [PROJECT_SECT_LAYR] = LAYR_VERSION,
    [PROJECT_SECT_ARP]  = ARP_VERSION,  [PROJECT_SECT_DROP] = DROP_VERSION,
    [PROJECT_SECT_DRON] = DRON_VERSION, [PROJECT_SECT_DSTD] = DSTD_VERSION,
    [PROJECT_SECT_PROG] = PROG_VERSION, [PROJECT_SECT_CHRD] = CHRD_VERSION,
    [PROJECT_SECT_CLIP] = CLIP_VERSION, [PROJECT_SECT_MIXR] = MIXR_VERSION,
    [PROJECT_SECT_PGEN] = PGEN_VERSION,
#if CONFIG_SYNTH_CUSTOM_WT
    [PROJECT_SECT_WTCU] = WTCU_VERSION,
#endif
#if CONFIG_SYNTH_CUSTOM_FM
    [PROJECT_SECT_FMCU] = FMCU_VERSION,
#endif
};

_Static_assert(PROJECT_SECT_COUNT <= 16, "skipped-section mask is a uint16_t");

/* The last successful load's skipped sections (project_snapshot.h); synth_ui
 * task only, like the load. */
static uint16_t s_skipped;
static uint8_t  s_skipped_ver[PROJECT_SECT_COUNT];

typedef enum { SECT_PARSE = 0, SECT_SKIP, SECT_REFUSE } sect_action_t;

typedef struct {
    uint16_t seen, skipped;                 /* bit 1u << project_sect_t */
    uint8_t  file_ver[PROJECT_SECT_COUNT];  /* of the skipped ones */
} sect_track_t;

/* What the loader does with a once-only section at file version ver
 * (file header rules): a duplicate refuses the file, as does GLOB at another
 * version; any other section at another version is skipped and recorded. */
static sect_action_t sect_check(sect_track_t *st, project_sect_t s, uint8_t ver)
{
    uint16_t bit = (uint16_t)(1u << s);
    if (st->seen & bit) return SECT_REFUSE;
    st->seen |= bit;
    if (ver == k_sect_ver[s]) return SECT_PARSE;
    if (s == PROJECT_SECT_GLOB) return SECT_REFUSE;
    st->skipped |= bit;
    st->file_ver[s] = ver;
    return SECT_SKIP;
}

uint16_t project_snapshot_skipped(void)
{
    return s_skipped;
}

bool project_snapshot_skipped_version(project_sect_t s, uint8_t *file_ver,
                                      uint8_t *build_ver)
{
    if ((unsigned)s >= PROJECT_SECT_COUNT || !(s_skipped & (1u << s))) return false;
    if (file_ver)  *file_ver  = s_skipped_ver[s];
    if (build_ver) *build_ver = k_sect_ver[s];
    return true;
}

const char *project_snapshot_sect_name(project_sect_t s)
{
    return ((unsigned)s < PROJECT_SECT_COUNT) ? k_sect_name[s] : "?";
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
        if (sequencer_core_export_layer_authored(i, scratch)) ser_layer(&w, scratch);
    }
    ser_arp(&w);
    ser_drop(&w);
    ser_drone(&w);
    ser_drone_std(&w);
    ser_prog(&w);
    ser_chrd(&w);
    ser_clip(&w);
    ser_mixr(&w);
    ser_pgen(&w);
#if CONFIG_SYNTH_CUSTOM_WT
    ser_wtcu(&w);
#endif
#if CONFIG_SYNTH_CUSTOM_FM
    ser_fmcu(&w);
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
    s_skipped = 0;
    if (!payload) return false;
    if (!name) name = "";

    /* Heap, not stack: the drones' voice_params_t blocks would put this frame
     * past the 1 KB -Wstack-usage limit. */
    typedef struct {
        staged_drone_t     drone;
        staged_drone_std_t dstd;
        staged_drop_t      drop;
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

    /* Every optional section starts at its fallback default: an absent or
     * skipped one is applied as it stands (file header rules); a parsed one
     * re-seeds it and overwrites what the file carries. */
    staged_glob_t  staged_glob;  glob_defaults(&staged_glob);
    staged_arp_t   staged_arp;   arp_section_defaults(&staged_arp);
    staged_prog_t  staged_prog;  seq_prog_params_defaults(&staged_prog);
    staged_clip_t  staged_clip;  clip_section_defaults(&staged_clip);
    staged_mixr_t  staged_mixr;  mixer_params_defaults(&staged_mixr);
    staged_pgen_t  staged_pgen;  prog_gen_params_defaults(&staged_pgen.params);
    seq_chord_t    staged_chords[SEQ_CHORD_SLOTS];
    seq_chords_params_defaults(staged_chords);
    drop_section_defaults(&sd->drop);
    drone_params_defaults(&sd->drone);
    drone_std_params_defaults(&sd->dstd);
    uint8_t staged_layer_count = 0;
    bool got_glob = false;
#if CONFIG_SYNTH_CUSTOM_WT
    wt_params_t staged_wtcu;
    wt_params_default(&staged_wtcu);
#endif
#if CONFIG_SYNTH_CUSTOM_FM
    fm_voice_t staged_fmcu;
    fm_voice_default(&staged_fmcu);
#endif

    tlv_reader_t r;
    tlv_reader_init(&r, payload, len);
    bool ok = true;
    sect_track_t st;
    memset(&st, 0, sizeof st);
    uint32_t tag; uint8_t ver; tlv_reader_t body;
    while (ok && tlv_next_section(&r, &tag, &ver, &body)) {
        sect_action_t act = SECT_PARSE;
        switch (tag) {
        case TAG_GLOB:
            act = sect_check(&st, PROJECT_SECT_GLOB, ver);
            if (act == SECT_PARSE) { ok = parse_glob(&body, &staged_glob); got_glob = ok; }
            break;
        case TAG_LAYR:
            /* Repeats once per layer; the ceiling must track the writer or
             * the firmware refuses its own files. */
            if (ver != LAYR_VERSION || staged_layer_count >= MAX_LAYERS) { ok = false; break; }
            ok = parse_layer(&body, &staged_layers[staged_layer_count]);
            if (ok) staged_layer_count++;
            break;
        case TAG_ARP:
            act = sect_check(&st, PROJECT_SECT_ARP, ver);
            if (act == SECT_PARSE) ok = parse_arp(&body, &staged_arp);
            break;
        case TAG_DROP:
            act = sect_check(&st, PROJECT_SECT_DROP, ver);
            if (act == SECT_PARSE) ok = parse_drop(&body, &sd->drop);
            break;
        case TAG_DRON:
            act = sect_check(&st, PROJECT_SECT_DRON, ver);
            if (act == SECT_PARSE) ok = parse_drone(&body, &sd->drone);
            break;
        case TAG_DSTD:
            act = sect_check(&st, PROJECT_SECT_DSTD, ver);
            if (act == SECT_PARSE) ok = parse_drone_std(&body, &sd->dstd);
            break;
        case TAG_PROG:
            act = sect_check(&st, PROJECT_SECT_PROG, ver);
            if (act == SECT_PARSE) ok = parse_prog(&body, &staged_prog);
            break;
        case TAG_CHRD:
            act = sect_check(&st, PROJECT_SECT_CHRD, ver);
            if (act == SECT_PARSE) ok = parse_chrd(&body, staged_chords);
            break;
        case TAG_CLIP:
            act = sect_check(&st, PROJECT_SECT_CLIP, ver);
            if (act == SECT_PARSE) ok = parse_clip(&body, &staged_clip);
            break;
        case TAG_MIXR:
            act = sect_check(&st, PROJECT_SECT_MIXR, ver);
            if (act == SECT_PARSE) ok = parse_mixr(&body, &staged_mixr);
            break;
        case TAG_PGEN:
            act = sect_check(&st, PROJECT_SECT_PGEN, ver);
            if (act == SECT_PARSE) ok = parse_pgen(&body, &staged_pgen);
            break;
#if CONFIG_SYNTH_CUSTOM_WT
        case TAG_WTCU:
            act = sect_check(&st, PROJECT_SECT_WTCU, ver);
            if (act == SECT_PARSE) ok = parse_wtcu(&body, &staged_wtcu);
            break;
#endif
#if CONFIG_SYNTH_CUSTOM_FM
        case TAG_FMCU:
            act = sect_check(&st, PROJECT_SECT_FMCU, ver);
            if (act == SECT_PARSE) ok = parse_fmcu(&body, &staged_fmcu);
            break;
#endif
        default:
            break;   /* unknown section: ignore (forward-compat) */
        }
        if (act == SECT_REFUSE) ok = false;
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
    transport_stop_reset();
    sequencer_core_set_playing(false);
    arp_core_clear_all();
    drop_core_clear_all();

    /* Chord table BEFORE the layer imports: import sizes each row's voice
     * count from the chord a loaded sentinel references (seq_track_num_voices),
     * so the table must already hold the file's voicings. */
    seq_chords_import(staged_chords);

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

    /* The project is the whole persisted state: every section applies, an
     * absent or skipped one at its fallback default. */
    apply_glob(&staged_glob);
    apply_arp(&staged_arp);
    apply_drop(&sd->drop);
    apply_drone(&sd->drone);
    apply_drone_std(&sd->dstd);
    apply_prog(&staged_prog);
    apply_pgen(&staged_pgen);
    apply_clip(&staged_clip);
    apply_mixr(&staged_mixr);
#if CONFIG_SYNTH_CUSTOM_WT
    wt_builder_set_params(&staged_wtcu);
#endif
#if CONFIG_SYNTH_CUSTOM_FM
    /* The layers imported above configured their FM rows from the session's
     * voice; the push re-sends the loaded one to them, the arp and the
     * drones. */
    s_fm_voice = staged_fmcu;
    sequencer_core_fm_voice_changed(FM_PUSH_ALL);
#endif

    /* The layer import writes solo[] wholesale rather than through the setter,
     * so nothing has applied the loaded solo state yet. Do it after the arp and
     * drone applies, or their duck would be undone by the enable that follows. */
    sequencer_core_notify_solo_changed();

    synth_ui_reload_mirror_from_core();

    free(staged_layers);
    free(sd);
    s_skipped = st.skipped;
    memcpy(s_skipped_ver, st.file_ver, sizeof s_skipped_ver);
    ESP_LOGI(TAG, "load '%s' OK: %u layer(s)", name, staged_layer_count);
    return true;
}

bool project_snapshot_load(uint8_t slot)
{
    uint8_t *payload = NULL;
    size_t   len = 0;
    char     name[PROJECT_NAME_LEN];
    s_skipped = 0;
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
