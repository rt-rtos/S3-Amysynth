#include "voice_config.h"
#include "amy.h"            /* wave constants, COEF_* indices */
#include "amy_helpers.h"    /* shared scratch-event begin/send */
#include "sequencer_core.h" /* lfo_rate_to_hz */
#include "seq_clamp.h"
#include <math.h>           /* powf: wobble downward-only center offset */
#include <string.h>

void voice_params_init_defaults(voice_params_t *vp)
{
    if (!vp) return;
    memset(vp, 0, sizeof(*vp));
    vp->amp_trim = 1.0f;   /* unity — the one non-zero default */
    /* Distortion off, but with the secondary parameters already in audible
     * territory: the first TYPE flip should be heard, not land on a no-op. */
    vp->dist = (seq_dist_t){ .type = 0, .drive = 2, .bits = 8, .rate = 8, .mix = 100 };
}

void voice_park_oscs(uint8_t synth, uint8_t first, uint8_t end)
{
    for (uint8_t o = first; o < end; o++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth                 = synth;
        e->osc                   = o;
        e->amp_coefs[COEF_CONST] = 0.0f;  /* render_osc_wave skips CONST 0 */
        e->amp_coefs[COEF_MOD]   = 0.0f;
        e->freq_coefs[COEF_MOD]  = 0.0f;
        amy_helpers_event_send(e);
    }
}

uint16_t voice_lfo_wave_to_amy(lfo_wave_t wave)
{
    switch (wave) {
        case LFO_WAVE_SINE:     return SINE;
        case LFO_WAVE_TRIANGLE: return TRIANGLE;
        case LFO_WAVE_SAW_UP:   return SAW_UP;
        case LFO_WAVE_SAW_DOWN: return SAW_DOWN;
        case LFO_WAVE_SQUARE:   return PULSE;
        case LFO_WAVE_RANDOM:   return NOISE;  /* native S&H at the carrier rate */
        default:                return SINE;
    }
}

uint8_t voice_wob_depth_to_db(uint8_t wob_depth)
{
    if (wob_depth > 100u) wob_depth = 100u;
    return (uint8_t)(((unsigned)wob_depth * VOICE_WOB_DB_MAX + 50u) / 100u);
}

uint8_t voice_wob_db_to_depth(uint8_t db)
{
    if (db > VOICE_WOB_DB_MAX) db = VOICE_WOB_DB_MAX;
    return (uint8_t)(((unsigned)db * 100u + VOICE_WOB_DB_MAX / 2u) / VOICE_WOB_DB_MAX);
}

/* ── Unison copy math (spec: voice_unison_t, seq_model.h) ───────────────
 * Copy i of n sits at position s in -1..+1 (0 alone for n = 1; the center
 * copy of an odd n at 0). Detune fans log-linearly: a note-tracking offset of
 * c cents is SEQ_LFO_PITCH_BASE_HZ * 2^(c/1200) (voice_config.h).
 * Blend tapers the outer copies (weight 1 - (1-blend)*|s|), normalized so
 * summed power stays at the single-copy level (detuned copies decorrelate,
 * so power - not amplitude - is what adds). */
/* Index map (contract in voice_config.h). Headed n is even and >= 2, so the
 * highest audible index is n + 1 and every mask fits a uint8_t. */
_Static_assert(((VOICE_UNISON_MAX_COPIES & ~1u) + 2u) <= 8u,
               "unison osc masks are uint8_t");

static inline bool unison_is_headed(uint8_t n, uint8_t layout)
{
    return layout == VOICE_UNISON_LAYOUT_HEADED && n >= 2u;
}

static inline bool unison_is_engine(uint8_t n, uint8_t layout)
{
    return layout == VOICE_UNISON_LAYOUT_ENGINE && n >= 2u;
}

uint8_t voice_unison_oscs_per_voice(uint8_t n, uint8_t layout)
{
    if (n < 1u) n = 1u;
    if (unison_is_engine(n, layout)) return 4u;   /* L, R clusters + carrier pair */
    return (uint8_t)(n + (unison_is_headed(n, layout) ? 4u : 2u));
}

uint8_t voice_unison_head_osc(uint8_t group, uint8_t n, uint8_t layout)
{
    if (unison_is_engine(n, layout)) return group ? 1u : 0u;
    return group ? (uint8_t)(n / 2u + 1u) : 0u;
}

/* Mirror pair p = min(i, n-1-i); pair p sits in group L when p is even, and
 * copy i is on its pair's side when i == p, the other side otherwise. For
 * even n that is "even i -> L, odd i -> R" (the engine layout's grid law
 * relies on this reading). */
static uint8_t unison_group_of(uint8_t i, uint8_t n)
{
    uint8_t p = (i < (uint8_t)(n - 1u - i)) ? i : (uint8_t)(n - 1u - i);
    return (uint8_t)((p & 1u) ^ ((i == p) ? 0u : 1u));
}

uint8_t voice_unison_copy_osc(uint8_t i, uint8_t n, uint8_t layout)
{
    if (unison_is_engine(n, layout))
        return voice_unison_head_osc(unison_group_of(i, n), n, layout);
    if (!unison_is_headed(n, layout)) return i;
    uint8_t group = unison_group_of(i, n);
    uint8_t rank  = 0;                       /* copies fill their group in i order */
    for (uint8_t j = 0; j < i; j++)
        if (unison_group_of(j, n) == group) rank++;
    return (uint8_t)(voice_unison_head_osc(group, n, layout) + 1u + rank);
}

uint8_t voice_unison_copies_mask(uint8_t n, uint8_t layout)
{
    uint8_t m = 0;
    for (uint8_t i = 0; i < n; i++)
        m |= (uint8_t)(1u << voice_unison_copy_osc(i, n, layout));
    return m;
}

uint8_t voice_unison_heads_mask(uint8_t n, uint8_t layout)
{
    if (!unison_is_headed(n, layout) && !unison_is_engine(n, layout)) return 0u;
    return (uint8_t)((1u << voice_unison_head_osc(0, n, layout)) |
                     (1u << voice_unison_head_osc(1, n, layout)));
}

static float unison_pos(uint8_t i, uint8_t n)
{
    return (n > 1u) ? (2.0f * (float)i / (float)(n - 1u) - 1.0f) : 0.0f;
}

static float unison_weight(uint8_t i, uint8_t n, float blend)
{
    return 1.0f - (1.0f - blend) * fabsf(unison_pos(i, n));
}

static float unison_amp_norm(uint8_t n, float blend)
{
    float sum = 0.0f;
    for (uint8_t i = 0; i < n; i++) {
        float w = unison_weight(i, n, blend);
        sum += w * w;
    }
    /* blend 0 with no center copy (even n) zeroes every weight; fall back to
     * the equal-weight norm instead of dividing by zero. */
    if (sum < 1e-6f) return 1.0f / sqrtf((float)n);
    return 1.0f / sqrtf(sum);
}

/* Per-copy CONST terms, shared by the build and the live push so the two
 * paths cannot drift. Overwrites amp CONST with the blend law. `headed` copies
 * leave pan alone - it is dead on a chained osc, the head mixes the group. */
static void unison_copy_coefs(amy_event *e, uint8_t i, uint8_t n,
                              const voice_unison_t *u, float base_amp,
                              bool headed)
{
    float s     = unison_pos(i, n);
    float blend = (float)u->blend_pct / 100.0f;
    e->freq_coefs[COEF_CONST] = SEQ_LFO_PITCH_BASE_HZ *
        powf(2.0f, (float)u->detune_cents * s / 1200.0f);
    if (!headed)
        e->pan_coefs[COEF_CONST] = 0.5f +
            0.5f * ((float)u->spread_pct / 100.0f) * s;
    e->amp_coefs[COEF_CONST]  = base_amp *
        unison_weight(i, n, blend) * unison_amp_norm(n, blend);
    /* Deterministic, decorrelated start phase per copy: left phase-aligned,
     * the copies comb through the whole attack. */
    e->trigger_phase = (float)i / (float)n;
}

/* The whole stereo fan collapses onto the two heads in the headed layout: each
 * head sits at the outermost position of its side. */
static float unison_head_pan(const voice_unison_t *u, uint8_t group)
{
    float half = 0.5f * ((float)u->spread_pct / 100.0f);
    return group ? (0.5f + half) : (0.5f - half);
}

/* Engine layout: one AMY unison-cluster osc per group. The engine renders
 * copy k at logfreq + offset + k * spacing (octaves) and power-normalizes the
 * weights over its own n/2 copies. Group g takes the even (L) or odd (R)
 * positions of the n-copy grid, so spacing = 4D/(n-1) and offset_g =
 * -D + 2gD/(n-1) with D the outermost detune in octaves - the same pitch set
 * as the headed layout. The amp CONST carries the ratio between the app's
 * all-n normalization and the engine's per-group one (1/sqrt(2) at blend 1),
 * so the group lands at the level its copies have under the other layouts. */
static float unison_group_gain(uint8_t n, uint8_t group, float blend)
{
    float sum = 0.0f;
    for (uint8_t i = group; i < n; i = (uint8_t)(i + 2u)) {
        float w = unison_weight(i, n, blend);
        sum += w * w;
    }
    return unison_amp_norm(n, blend) * sqrtf(sum);
}

static void unison_engine_coefs(amy_event *e, uint8_t group, uint8_t n,
                                const voice_unison_t *u, float base_amp)
{
    float blend = (float)u->blend_pct / 100.0f;
    float d     = (float)u->detune_cents / 1200.0f;
    float grid  = (n > 1u) ? 1.0f / (float)(n - 1u) : 0.0f;
    e->unison_count   = (uint8_t)(n / 2u);
    e->unison_spacing = 4.0f * d * grid;
    e->unison_offset  = -d + 2.0f * (float)group * d * grid;
    e->unison_blend   = blend;
    e->pan_coefs[COEF_CONST] = unison_head_pan(u, group);
    e->amp_coefs[COEF_CONST] = base_amp * unison_group_gain(n, group, blend);
    /* Start phase i/n per grid copy, as unison_copy_coefs: the engine
     * respreads copy k of a group from its copy 0 by k/(n/2), so copy 0 at
     * g/n puts grid copy 2k+g at (2k+g)/n. A set trigger_phase also restarts
     * every copy on each note-on. */
    e->trigger_phase = (float)group / (float)n;
}

void voice_build_wave(const voice_wave_cfg_t *cfg)
{
    if (!cfg) return;

    /* Pool definition; resets every osc of every existing voice
     * (voice_wave_cfg_t). */
    amy_event *e = amy_helpers_event_begin();
    e->synth          = cfg->synth;
    e->num_voices     = cfg->num_voices;
    e->oscs_per_voice = cfg->oscs_per_voice;
    amy_helpers_event_send(e);

    /* Audible copies: osc 0 alone, or the unison fan 0..n-1 (any reserved
     * LFO pair sits above it - the caller sized oscs_per_voice). n == 1
     * emits exactly the single-osc skeleton, with no pan/phase/freq CONST
     * terms, so unison off is bit-for-bit the single-osc build. Each copy is a
     * note-following carrier (COEF_NOTE=1) with EG0-gated amplitude. */
    uint8_t n = 1;
    if (cfg->unison && cfg->unison->count > 1u && cfg->wave != KS)
        n = cfg->unison->count;
    if (n > VOICE_UNISON_MAX_COPIES) n = VOICE_UNISON_MAX_COPIES;
    uint8_t layout = (cfg->unison && n > 1u) ? cfg->unison->layout
                                             : (uint8_t)VOICE_UNISON_LAYOUT_FAN;
    bool headed = unison_is_headed(n, layout);
    bool engine = unison_is_engine(n, layout);
    if (headed || engine) n = (uint8_t)(n & ~1u);   /* two equal groups */

    /* A fan-N pool and a headed-(N-2) pool have the same shape (so do a fan-2
     * and an engine pool). Wipe every audible osc explicitly so the build
     * never depends on what the previous layout left in chained_osc,
     * filter_type, unison count or MOD rails; the pool definition's own reset
     * (patches_load_patch) covers the same state. Voice-relative; a no-op on
     * an osc AMY has not allocated yet. The reserved oscs above the audible
     * layout are parked below instead. */
    uint8_t audible = (uint8_t)(headed ? n + 2u : engine ? 2u : n);
    if (n > 1u) {
        for (uint8_t i = 0; i < audible; i++) {
            e = amy_helpers_event_begin();
            e->synth     = cfg->synth;
            e->reset_osc = i;
            amy_helpers_event_send(e);
        }
    }

    if (headed) {
        /* Two SILENT heads, each chaining half the copies: the head renders
         * after its chain and applies the envelope, dist, filter and pan to the
         * group sum once (amy.c render_osc_wave). */
        for (uint8_t g = 0; g < 2u; g++) {
            uint8_t head = voice_unison_head_osc(g, n, layout);
            e = amy_helpers_event_begin();
            e->synth = cfg->synth;
            e->osc   = head;
            e->wave  = SILENT;
            e->amp_coefs[COEF_CONST] = cfg->osc0_amp_const;
            e->amp_coefs[COEF_VEL]   = cfg->osc0_amp_vel;
            e->amp_coefs[COEF_EG0]   = 1.0f;
            e->pan_coefs[COEF_CONST] = unison_head_pan(cfg->unison, g);
            e->chained_osc           = (uint8_t)(head + 1u);
            amy_helpers_event_send(e);
        }
    }

    /* Engine: the group IS the osc - one full voice osc per side (envelope,
     * velocity, level, filter, dist and pan as osc 0 carries them today) that
     * renders its n/2 copies internally. */
    uint8_t audible_oscs = engine ? 2u : n;
    for (uint8_t i = 0; i < audible_oscs; i++) {
        uint8_t osc = engine ? i : voice_unison_copy_osc(i, n, layout);
        e = amy_helpers_event_begin();
        e->synth = cfg->synth;
        e->osc   = osc;
        e->wave  = cfg->wave;
        if (cfg->wt_preset >= 0) e->preset = cfg->wt_preset;
        if (cfg->wave == KS) {
            /* Authored feedback drives KS string decay; the 0 "never set"
             * sentinel falls back to the fixed default. */
            float fb = cfg->ks_feedback;
            if (fb > 1.0f) fb = 1.0f;   /* > 1 would make the KS buffer diverge */
            e->feedback = (cfg->ks_feedback_authored && fb > 0.0f) ? fb : 0.9f;
            e->duty_coefs[COEF_CONST] = 0.5f + cfg->ks_duty_ofs;
        }
        e->freq_coefs[COEF_NOTE] = 1.0f;
        if (headed) {
            /* The head owns the envelope, velocity and level for the group, so
             * a copy contributes its blend weight alone; chaining it to the
             * next copy of its group closes the linked list. */
            e->amp_coefs[COEF_VEL] = 0.0f;
            e->amp_coefs[COEF_EG0] = 0.0f;
            unison_copy_coefs(e, i, n, cfg->unison, 1.0f, true);
            uint8_t grp  = (osc > voice_unison_head_osc(1, n, layout)) ? 1u : 0u;
            uint8_t last = (uint8_t)(voice_unison_head_osc(grp, n, layout) + n / 2u);
            if (osc < last) e->chained_osc = (uint8_t)(osc + 1u);
        } else {
            e->amp_coefs[COEF_CONST] = cfg->osc0_amp_const;
            e->amp_coefs[COEF_VEL]   = cfg->osc0_amp_vel;
            e->amp_coefs[COEF_EG0]   = 1.0f;
            if (engine)
                unison_engine_coefs(e, i, n, cfg->unison, cfg->osc0_amp_const);
            else if (n > 1u)
                unison_copy_coefs(e, i, n, cfg->unison, cfg->osc0_amp_const, false);
        }
        amy_helpers_event_send(e);
    }

    /* Park every osc the caller reserved above the audible layout (the native
     * LFO carrier pair; why: voice_wave_cfg_t). The LFO path raises the
     * carrier when one is authored. */
    voice_park_oscs(cfg->synth, audible, cfg->oscs_per_voice);

    /* A rebuild must not drop the stage, so the owner's block rides every
     * build. cfg->dist == NULL means the caller has none to assert yet. */
    if (cfg->dist) {
        if (headed || engine) {
            for (uint8_t g = 0; g < 2u; g++)
                voice_apply_dist_osc(cfg->synth,
                                     voice_unison_head_osc(g, n, layout),
                                     cfg->dist);
        } else {
            voice_apply_dist(cfg->synth, cfg->dist);
        }
    }
}

void voice_push_unison_live(uint8_t synth, const voice_unison_t *u,
                            float base_amp)
{
    if (!u || u->count <= 1u || u->count > VOICE_UNISON_MAX_COPIES) return;
    uint8_t n      = u->count;
    uint8_t layout = u->layout;
    bool    headed = unison_is_headed(n, layout);
    bool    engine = unison_is_engine(n, layout);
    if (headed || engine) n = (uint8_t)(n & ~1u);
    if (engine) {
        /* Everything live lives on the two cluster oscs. */
        for (uint8_t g = 0; g < 2u; g++) {
            amy_event *e = amy_helpers_event_begin();
            e->synth = synth;
            e->osc   = voice_unison_head_osc(g, n, layout);
            unison_engine_coefs(e, g, n, u, base_amp);
            amy_helpers_event_send(e);
        }
        return;
    }
    for (uint8_t i = 0; i < n; i++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth = synth;
        e->osc   = voice_unison_copy_osc(i, n, layout);
        unison_copy_coefs(e, i, n, u, headed ? 1.0f : base_amp, headed);
        amy_helpers_event_send(e);
    }
    /* Spread lives on the heads: the copies feed them through the chain. */
    for (uint8_t g = 0; headed && g < 2u; g++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth = synth;
        e->osc   = voice_unison_head_osc(g, n, layout);
        e->pan_coefs[COEF_CONST] = unison_head_pan(u, g);
        amy_helpers_event_send(e);
    }
}

/* ── Per-voice distortion ────────────────────────────────────────────────── */

void voice_dist_clamp(seq_dist_t *d)
{
    if (!d) return;
    d->type  = (uint8_t)(d->type & 7u);  /* stage mask; stray bits dropped */
    d->drive = SEQ_CLAMP_U8(d->drive, 1u, 16u);
    d->bits  = SEQ_CLAMP_U8(d->bits, 1u, 24u);
    d->rate  = SEQ_CLAMP_U8(d->rate, 1u, 64u);
    d->mix   = SEQ_CLAMP_U8(d->mix, 0u, 100u);
}

void voice_apply_dist(uint8_t synth, const seq_dist_t *d)
{
    /* Reach note: voice_config.h. Multi-osc patch strings distort their base
     * osc only; distorting each osc and summing afterwards is a different,
     * harsher effect, so the reach stops here deliberately. */
    voice_apply_dist_osc(synth, 0, d);
}

void voice_apply_dist_osc(uint8_t synth, uint8_t osc, const seq_dist_t *d)
{
    if (!d) return;
    seq_dist_t v = *d;
    voice_dist_clamp(&v);

    amy_event *e = amy_helpers_event_begin();
    e->synth      = synth;
    e->osc        = osc;
    /* v.type is a stage mask; author all three enables explicitly so a
     * mask change turns dropped stages off. */
    e->dist_clip  = !!(v.type & 1u);
    e->dist_fold  = !!(v.type & 2u);
    e->dist_crush = !!(v.type & 4u);
    e->dist_bits  = (uint8_t)v.bits;
    e->dist_rate  = (uint16_t)v.rate;
    /* Drive and mix ride AMY's control-coef rails: author the CONST term
     * only, leaving the MOD rail for a native LFO. Drive's CONST is linear
     * (AMY maps it onto its log2 drive rail on the way in); mix is linear 0..1. */
    e->dist_drive_coefs[COEF_CONST] = (float)v.drive;
    e->dist_mix_coefs[COEF_CONST]   = (float)v.mix / 100.0f;
    amy_helpers_event_send(e);
}

/* PROTOTYPE LFO->distortion tick: contract in voice_config.h. Partial event -
 * EVENT_TO_DELTA emits deltas only for the fields set here, so the committed
 * type/bits/rate are never re-sent, and a stepper tick costs one event. */
void voice_push_dist_lfo(uint8_t synth, const seq_dist_t *base,
                         const seq_lfo_t *lfo, float val)
{
    if (!base || !lfo || base->type == 0u) return;
    float d = (float)lfo->depth / 100.0f;

    amy_event *e = amy_helpers_event_begin();
    e->synth = synth;
    e->osc   = 0;   /* base osc of every voice - same reach as voice_apply_dist */
    if (LFO_HAS_TGT(lfo, LFO_TARGET_DIST_DRIVE)) {
        /* Octave-denominated pre-gain swing around the committed drive,
         * clamped to the documented wire range. */
        float drv = (float)base->drive *
                    powf(2.0f, d * VOICE_LFO_DEPTH_DIST_OCT * val);
        e->dist_drive_coefs[COEF_CONST] = SEQ_CLAMP_F32(drv, 1.0f, 16.0f);
    }
    if (LFO_HAS_TGT(lfo, LFO_TARGET_DIST_MIX)) {
        float mix = (float)base->mix / 100.0f +
                    d * VOICE_LFO_DEPTH_DIST_MIX * val;
        e->dist_mix_coefs[COEF_CONST] = SEQ_CLAMP_F32(mix, 0.0f, 1.0f);
    }
    amy_helpers_event_send(e);
}

void voice_apply_native_lfo_topo(uint8_t synth, const seq_lfo_t *lfo,
                                 uint16_t bpm, uint8_t carrier_osc,
                                 uint8_t pitch_mask, uint8_t voice_mask)
{
    amy_event *e;
    uint8_t wobble_osc   = (uint8_t)(carrier_osc + 1u);
    uint8_t coupled_mask = (uint8_t)(pitch_mask | voice_mask);

    if (lfo && lfo->enabled && lfo->targets) {
        /* Coupled (audible) oscs: wire mod_source to the carrier (voice-local -
         * AMY adds the base_osc offset) and set the COEF_MOD depth for every
         * checked target, clearing every sibling rail first. One carrier feeds
         * all targets on all coupled oscs, each scaled by its own normalizing
         * constant. Bass couples both audible oscs so vibrato/tremolo hits the
         * sub; the wave build couples osc0 only. The two masks split which
         * oscs take which rails: pitch/duty ride the sounding oscs, the
         * per-voice stages ride whatever carries them (osc 0, or the SILENT
         * heads under unison). */
        float d = (float)lfo->depth / 100.0f;
        for (uint8_t o = 0; (uint8_t)(coupled_mask >> o) != 0u; o++) {
            if (!(coupled_mask & (uint8_t)(1u << o))) continue;
            e = amy_helpers_event_begin();
            e->synth      = synth;
            e->osc        = o;
            e->mod_source[0] = carrier_osc;
            e->filter_freq_coefs[COEF_MOD] = 0.0f;
            e->amp_coefs[COEF_MOD]         = 0.0f;
            e->freq_coefs[COEF_MOD]        = 0.0f;
            e->duty_coefs[COEF_MOD]        = 0.0f;
            e->pan_coefs[COEF_MOD]         = 0.0f;
            /* Distortion rides the oscs that carry the stage - the same reach
             * as voice_apply_dist - so its COEF_MOD follows voice_mask; clear
             * it on every coupled osc so a stale rail cannot keep modulating.
             * Inert on an osc with no dist stage enabled. */
            e->dist_drive_coefs[COEF_MOD]  = 0.0f;
            e->dist_mix_coefs[COEF_MOD]    = 0.0f;
            if (pitch_mask & (uint8_t)(1u << o)) {
                if (LFO_HAS_TGT(lfo, LFO_TARGET_PITCH)) e->freq_coefs[COEF_MOD] = d * VOICE_LFO_DEPTH_PITCH;
                if (LFO_HAS_TGT(lfo, LFO_TARGET_SCAN))  e->duty_coefs[COEF_MOD] = d * VOICE_LFO_DEPTH_SCAN;
            }
            if (voice_mask & (uint8_t)(1u << o)) {
                if (LFO_HAS_TGT(lfo, LFO_TARGET_FILTER)) e->filter_freq_coefs[COEF_MOD] = voice_lfo_filter_octaves(lfo);
                if (LFO_HAS_TGT(lfo, LFO_TARGET_AMP))    e->amp_coefs[COEF_MOD]         = d * VOICE_LFO_DEPTH_AMP;
                /* Drive rides AMY's log2 rail, so the drive COEF_MOD is in
                 * octaves: the carrier's +/-1 swing is +/-(d*OCT) octaves of
                 * pre-gain, matching the software stepper's law. Mix is a
                 * linear rail. Both stay inert until a dist stage is enabled. */
                if (LFO_HAS_TGT(lfo, LFO_TARGET_DIST_DRIVE))
                    e->dist_drive_coefs[COEF_MOD] = d * VOICE_LFO_DEPTH_DIST_OCT;
                if (LFO_HAS_TGT(lfo, LFO_TARGET_DIST_MIX))
                    e->dist_mix_coefs[COEF_MOD]   = d * VOICE_LFO_DEPTH_DIST_MIX;
                if (LFO_HAS_TGT(lfo, LFO_TARGET_PAN)) {
                    /* Pan is [0,1], not bipolar: set the center baseline and
                     * swing COEF_MOD around it in the same event. */
                    e->pan_coefs[COEF_CONST] = 0.5f;
                    e->pan_coefs[COEF_MOD]   = d * VOICE_LFO_DEPTH_PAN;
                }
            }
            amy_helpers_event_send(e);
        }

        /* Carrier: BPM-synced, no pitch tracking / velocity / envelope. WOBBLE
         * (second-order LFO) chains the next osc as the carrier's own
         * mod_source per AMY's chained-modulator semantics
         * (mod_osc_would_cause_loop / compute_mod_scale); the reach picks
         * which carrier rails it drives. Depth breathing is DOWNWARD-ONLY:
         * amp CONST is pre-dropped by the swing so the breath peaks exactly
         * at the authored LFO depth (an unclamped +swing would multiply the
         * effective depth through the exponential MOD rail - voice_config.h).
         * All coupled coefs are ALWAYS written so a stale wobble cannot keep
         * modulating after it is turned off or its reach changes (the
         * clear-siblings contract). */
        float w = (float)lfo->wob_depth / 100.0f;
        float wcoef = w * VOICE_WOB_DEPTH_AMP;
        bool wob_depth_live = lfo->wob_depth > 0 &&
                              lfo->wob_reach != WOB_REACH_RATE;
        bool wob_rate_live  = lfo->wob_depth > 0 &&
                              lfo->wob_reach != WOB_REACH_DEPTH;
        e = amy_helpers_event_begin();
        e->synth                  = synth;
        e->osc                    = carrier_osc;
        e->wave                   = voice_lfo_wave_to_amy(lfo->wave);
        e->freq_coefs[COEF_CONST] = lfo_rate_to_hz(lfo->rate, bpm);
        e->freq_coefs[COEF_NOTE]  = 0.0f;
        e->freq_coefs[COEF_BEND]  = 0.0f;
        e->amp_coefs[COEF_CONST]  = wob_depth_live
                                  ? powf(10.0f, -3.0f * wcoef) : 1.0f;
        e->amp_coefs[COEF_VEL]    = 0.0f;
        e->amp_coefs[COEF_EG0]    = 0.0f;
        e->mod_source[0]          = wobble_osc;
        e->amp_coefs[COEF_MOD]    = wob_depth_live ? wcoef : 0.0f;
        e->freq_coefs[COEF_MOD]   = wob_rate_live
                                  ? w * VOICE_WOB_DEPTH_RATE : 0.0f;
        amy_helpers_event_send(e);

        /* The wobble modulator itself: fixed TRIANGLE (smooth, no steps on the
         * depth/rate rails), BPM-synced, dormant at 0 %. */
        e = amy_helpers_event_begin();
        e->synth                  = synth;
        e->osc                    = wobble_osc;
        e->wave                   = TRIANGLE;
        e->freq_coefs[COEF_CONST] = lfo_rate_to_hz((lfo_rate_t)lfo->wob_rate, bpm);
        e->freq_coefs[COEF_NOTE]  = 0.0f;
        e->freq_coefs[COEF_BEND]  = 0.0f;
        e->amp_coefs[COEF_CONST]  = (lfo->wob_depth > 0) ? 1.0f : 0.0f;
        e->amp_coefs[COEF_VEL]    = 0.0f;
        e->amp_coefs[COEF_EG0]    = 0.0f;
        amy_helpers_event_send(e);
    } else {
        /* Disabled: clear the mod coupling on every coupled osc, silence the
         * carrier. The coupled-osc events always go out (those oscs exist as
         * part of the patch and cost nothing). */
        for (uint8_t o = 0; (uint8_t)(coupled_mask >> o) != 0u; o++) {
            if (!(coupled_mask & (uint8_t)(1u << o))) continue;
            e = amy_helpers_event_begin();
            e->synth                       = synth;
            e->osc                         = o;
            e->filter_freq_coefs[COEF_MOD] = 0.0f;
            e->amp_coefs[COEF_MOD]         = 0.0f;
            e->freq_coefs[COEF_MOD]        = 0.0f;
            e->duty_coefs[COEF_MOD]        = 0.0f;
            e->pan_coefs[COEF_MOD]         = 0.0f;
            e->dist_drive_coefs[COEF_MOD]  = 0.0f;
            e->dist_mix_coefs[COEF_MOD]    = 0.0f;
            amy_helpers_event_send(e);
        }

        e = amy_helpers_event_begin();
        e->synth                 = synth;
        e->osc                   = carrier_osc;
        e->amp_coefs[COEF_CONST] = 0.0f;  /* dormant */
        e->amp_coefs[COEF_MOD]   = 0.0f;  /* clear wobble depth coupling */
        e->freq_coefs[COEF_MOD]  = 0.0f;  /* clear wobble rate coupling  */
        amy_helpers_event_send(e);

        e = amy_helpers_event_begin();
        e->synth                 = synth;
        e->osc                   = wobble_osc;
        e->amp_coefs[COEF_CONST] = 0.0f;  /* wobble modulator dormant */
        amy_helpers_event_send(e);
    }
}

/* The wave-build layout (carrier = osc1, osc0 coupled): the signature every
 * WAVE-mode engine calls. */
void voice_apply_native_lfo(uint8_t synth, const seq_lfo_t *lfo, uint16_t bpm)
{
    voice_apply_native_lfo_topo(synth, lfo, bpm, 1, 0x01, 0x01);
}
