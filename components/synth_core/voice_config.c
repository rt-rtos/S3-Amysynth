#include "voice_config.h"
#include "amy.h"            /* wave constants, COEF_* indices */
#include "amy_helpers.h"    /* shared scratch-event begin/send */
#include "sequencer_core.h" /* sequencer_core_ks_feedback_from_q */
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

/* ── Lazy LFO-sibling materialization state ──────────────────────────────
 * Per synth: has anything ever sent an event to the LFO sibling oscs
 * (osc1/osc2) since the pool was last provably reset? AMY allocates an osc's
 * ~532 B struct the first time an event addresses it (ensure_osc_allocd), so
 * the disabled-path "park" events below would themselves BE the allocation.
 * Skipping them is safe exactly when this bit is clear: the oscs are then NULL
 * or AMY-reset, both silent and free of mod coupling.
 *
 * Clear the bit ONLY on provable freshness (a known pool-shape change in
 * voice_build_wave); anything ambiguous keeps it SET. A wrongly-set bit costs
 * only memory; a wrongly-clear bit skips parking a live carrier (the
 * stale-COEF_MOD DC-rail class). The shape cache exists solely for that proof:
 * 0 = unknown (never built here, or foreign config since, see
 * voice_lfo_mark_foreign), and unknown never clears the bit.
 *
 * Core-0 / UI-task only, like every entry point in this file. */
#define VOICE_LFO_SYNTH_MAX 128u

static uint16_t s_pool_shape[VOICE_LFO_SYNTH_MAX];        /* (voices<<8)|oscs; 0 = unknown */
static uint8_t  s_lfo_materialized[VOICE_LFO_SYNTH_MAX / 8u];

static inline bool lfo_materialized(uint8_t synth)
{
    if (synth >= VOICE_LFO_SYNTH_MAX) return true;  /* out of range: park always */
    return (s_lfo_materialized[synth >> 3] >> (synth & 7u)) & 1u;
}

static inline void lfo_set_materialized(uint8_t synth, bool on)
{
    if (synth >= VOICE_LFO_SYNTH_MAX) return;
    if (on) s_lfo_materialized[synth >> 3] |=  (uint8_t)(1u << (synth & 7u));
    else    s_lfo_materialized[synth >> 3] &= (uint8_t)~(1u << (synth & 7u));
}

bool voice_lfo_siblings_materialized(uint8_t synth)
{
    return lfo_materialized(synth);
}

void voice_lfo_mark_foreign(uint8_t synth)
{
    if (synth >= VOICE_LFO_SYNTH_MAX) return;
    s_pool_shape[synth] = 0;                 /* shape proof is gone */
    lfo_set_materialized(synth, true);       /* park always until proven fresh */
}

void voice_lfo_note_pool_shape(uint8_t synth, uint8_t num_voices,
                               uint8_t oscs_per_voice)
{
    if (synth >= VOICE_LFO_SYNTH_MAX) return;
    /* A *known different* shape forces AMY to reallocate the voice oscs
     * (patches.c no-ops only on an identical shape) and RESET_OSC each one, so
     * the carrier pair is provably fresh. An unknown cache proves nothing. */
    uint16_t shape = (uint16_t)(((uint16_t)num_voices << 8) | oscs_per_voice);
    if (s_pool_shape[synth] != 0 && s_pool_shape[synth] != shape)
        lfo_set_materialized(synth, false);
    s_pool_shape[synth] = shape;
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

/* ââ Unison copy math (spec: voice_unison_t, seq_model.h) âââââââââââââââ
 * Copy i of n sits at position s in -1..+1 (0 alone for n = 1; the center
 * copy of an odd n at 0). Detune fans log-linearly: freq CONST is Hz through
 * the event API and logfreq_of_freq() anchors it at 440 Hz, so a
 * note-tracking offset of c cents is SEQ_LFO_PITCH_BASE_HZ * 2^(c/1200).
 * Blend tapers the outer copies (weight 1 - (1-blend)*|s|), normalized so
 * summed power stays at the single-copy level (detuned copies decorrelate,
 * so power - not amplitude - is what adds). */
/* Index map (contract in voice_config.h). Headed n is even and >= 2, so the
 * highest audible index is n + 1 and every mask fits a uint8_t. */
_Static_assert(((VOICE_UNISON_MAX_COPIES & ~1u) + 2u) <= 8u,
               "unison osc masks are uint8_t");

static inline bool unison_is_headed(uint8_t n, bool headed)
{
    return headed && n >= 2u;
}

uint8_t voice_unison_oscs_per_voice(uint8_t n, bool headed)
{
    if (n < 1u) n = 1u;
    return (uint8_t)(n + (unison_is_headed(n, headed) ? 4u : 2u));
}

uint8_t voice_unison_head_osc(uint8_t group, uint8_t n)
{
    return group ? (uint8_t)(n / 2u + 1u) : 0u;
}

uint8_t voice_unison_copy_osc(uint8_t i, uint8_t n, bool headed)
{
    if (!unison_is_headed(n, headed)) return i;
    uint8_t p     = (i < (uint8_t)(n - 1u - i)) ? i : (uint8_t)(n - 1u - i);
    uint8_t group = (uint8_t)((p & 1u) ^ ((i == p) ? 0u : 1u));
    uint8_t rank  = 0;                       /* copies fill their group in i order */
    for (uint8_t j = 0; j < i; j++) {
        uint8_t pj = (j < (uint8_t)(n - 1u - j)) ? j : (uint8_t)(n - 1u - j);
        if ((uint8_t)((pj & 1u) ^ ((j == pj) ? 0u : 1u)) == group) rank++;
    }
    return (uint8_t)(voice_unison_head_osc(group, n) + 1u + rank);
}

uint8_t voice_unison_copies_mask(uint8_t n, bool headed)
{
    uint8_t m = 0;
    for (uint8_t i = 0; i < n; i++)
        m |= (uint8_t)(1u << voice_unison_copy_osc(i, n, headed));
    return m;
}

uint8_t voice_unison_heads_mask(uint8_t n, bool headed)
{
    if (!unison_is_headed(n, headed)) return 0u;
    return (uint8_t)((1u << voice_unison_head_osc(0, n)) |
                     (1u << voice_unison_head_osc(1, n)));
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

void voice_build_wave(const voice_wave_cfg_t *cfg)
{
    if (!cfg) return;

    voice_lfo_note_pool_shape(cfg->synth, cfg->num_voices, cfg->oscs_per_voice);

    /* Pool definition. Re-sending an unchanged shape is a no-op in AMY, so
     * callers may rebuild freely without resetting live voices. */
    amy_event *e = amy_helpers_event_begin();
    e->synth          = cfg->synth;
    e->num_voices     = cfg->num_voices;
    e->oscs_per_voice = cfg->oscs_per_voice;
    amy_helpers_event_send(e);

    /* Audible copies: osc 0 alone, or the unison fan 0..n-1 (any reserved
     * LFO pair sits above it - the caller sized oscs_per_voice). n == 1
     * emits exactly the single-osc skeleton, with no pan/phase/freq CONST
     * terms, so unison off is bit-for-bit the old build. Each copy is a
     * note-following carrier (COEF_NOTE=1) with EG0-gated amplitude. */
    uint8_t n = 1;
    if (cfg->unison && cfg->unison->count > 1u && cfg->wave != KS)
        n = cfg->unison->count;
    if (n > VOICE_UNISON_MAX_COPIES) n = VOICE_UNISON_MAX_COPIES;
    bool headed = cfg->unison && cfg->unison->headed && n > 1u;
    if (headed) n = (uint8_t)(n & ~1u);   /* the headed layout runs even counts */

    /* A fan-N pool and a headed-(N-2) pool have the same shape, and AMY does
     * not reset a re-sent shape, so a layout switch would inherit the previous
     * build's chained_osc, filter_type and MOD rails. Wipe every audible osc
     * first (voice-relative; a no-op on one AMY has not allocated yet). The
     * reserved carrier pair stays untouched - resetting it would be the
     * allocation the lazy reservation exists to avoid. */
    if (n > 1u) {
        uint8_t audible = (uint8_t)(headed ? n + 2u : n);
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
            uint8_t head = voice_unison_head_osc(g, n);
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

    for (uint8_t i = 0; i < n; i++) {
        uint8_t osc = voice_unison_copy_osc(i, n, headed);
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
        }
        e->freq_coefs[COEF_NOTE] = 1.0f;
        if (headed) {
            /* The head owns the envelope, velocity and level for the group, so
             * a copy contributes its blend weight alone; chaining it to the
             * next copy of its group closes the linked list. */
            e->amp_coefs[COEF_VEL] = 0.0f;
            e->amp_coefs[COEF_EG0] = 0.0f;
            unison_copy_coefs(e, i, n, cfg->unison, 1.0f, true);
            uint8_t grp  = (osc > voice_unison_head_osc(1, n)) ? 1u : 0u;
            uint8_t last = (uint8_t)(voice_unison_head_osc(grp, n) + n / 2u);
            if (osc < last) e->chained_osc = (uint8_t)(osc + 1u);
        } else {
            e->amp_coefs[COEF_CONST] = cfg->osc0_amp_const;
            e->amp_coefs[COEF_VEL]   = cfg->osc0_amp_vel;
            e->amp_coefs[COEF_EG0]   = 1.0f;
            if (n > 1u)
                unison_copy_coefs(e, i, n, cfg->unison, cfg->osc0_amp_const, false);
        }
        amy_helpers_event_send(e);
    }

    /* A rebuild must not drop the stage, so the owner's block rides every
     * build. cfg->dist == NULL means the caller has none to assert yet. */
    if (cfg->dist) {
        if (headed) {
            for (uint8_t g = 0; g < 2u; g++)
                voice_apply_dist_osc(cfg->synth, voice_unison_head_osc(g, n),
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
    bool    headed = u->headed != 0u;
    if (headed) n = (uint8_t)(n & ~1u);
    for (uint8_t i = 0; i < n; i++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth = synth;
        e->osc   = voice_unison_copy_osc(i, n, headed);
        unison_copy_coefs(e, i, n, u, headed ? 1.0f : base_amp, headed);
        amy_helpers_event_send(e);
    }
    /* Spread lives on the heads: the copies feed them through the chain. */
    for (uint8_t g = 0; headed && g < 2u; g++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth = synth;
        e->osc   = voice_unison_head_osc(g, n);
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
    /* osc 0 addresses the BASE osc of every voice in the synth (AMY's
     * patches_event_has_voices). For wave voices and for ALGO/FM voices - where
     * osc 0 already carries the summed operator output - that is the whole
     * voice. Multi-osc patch strings distort their base osc only; distorting
     * each osc and summing afterwards is a different, harsher effect, so the
     * reach stops here deliberately. */
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
    /* Drive and mix ride AMY's control-coef rails now: author the CONST term
     * only, leaving the MOD rail for a native LFO. Drive's CONST is linear
     * (AMY maps it onto its log2 drive rail on the way in); mix is linear 0..1. */
    e->dist_drive_coefs[COEF_CONST] = (float)v.drive;
    e->dist_mix_coefs[COEF_CONST]   = (float)v.mix / 100.0f;
#if CONFIG_SEQ_DIST_DRIVE_AMP_EG
    /* WIP PROTOTYPE (Kconfig-gated, default off): route the amp envelope (EG0)
     * into drive on the log2 rail - the unipolar 0..1 env times a signed octave
     * depth, exactly the shape filter_env_amount uses to sweep cutoff. The
     * authored drive above is the release-tail floor; this adds up to DEPTH_Q/4
     * octaves at the envelope peak (COEF0_SPECIAL only remaps CONST, so the EG
     * slot is already in octaves). Shares EG0 with amplitude, so drive tracks
     * loudness. TODO: promote to a per-track amount + EG0/EG1 source choice if
     * it proves itself (see the dist coef rail in AMY-EDITS.md). */
    e->dist_drive_coefs[COEF_EG0] =
        (float)CONFIG_SEQ_DIST_DRIVE_AMP_EG_DEPTH_Q / 4.0f;
#endif
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
                 * pre-gain, matching the old software stepper's law. Mix is a
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

        lfo_set_materialized(synth, true);
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

        /* A never-materialized carrier pair is NULL or AMY-reset: silent, no
         * coupling, nothing to park. Sending the events would allocate the
         * oscs and forfeit the lazy reservation. */
        if (!lfo_materialized(synth))
            return;

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
