#include "voice_config.h"
#include "amy.h"            /* wave constants, COEF_* indices */
#include "amy_helpers.h"    /* shared scratch-event begin/send */
#include "sequencer_core.h" /* lfo_rate_to_hz */
#include "lfo_bank.h"       /* LFO_BANK_NONE */
#include "synth_level.h"    /* synth_level_set_lfo_cap */
#include "note_div.h"
#include "seq_clamp.h"
#include <assert.h>
#include <math.h>           /* powf: wobble downward-only center offset */
#include <string.h>

void voice_params_init_defaults(voice_params_t *vp)
{
    if (!vp) return;
    memset(vp, 0, sizeof(*vp));
    vp->amp_trim = 1.0f;   /* unity */
    vp->lfo.rate     = NOTE_DIV_1_8;
    vp->lfo.wob_rate = NOTE_DIV_1_8;
    /* LFO2 starts from the LFO editor's seed, off and with no targets, so
     * opening it never takes a target from LFO1. */
    vp->lfo2 = (seq_lfo_t){
        .mode     = LFO_MODE_FREE,
        .wave     = LFO_WAVE_SINE,
        .rate     = NOTE_DIV_1_1,
        .depth    = 50,
        .wob_rate = NOTE_DIV_1_8,
    };
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
 * The stack deals the n copies over G groups (voice_unison_groups), m = n / G
 * copies each; a remainder is not rendered. Inside a group copy j of m sits
 * at position p in -1..+1 (0 alone for m = 1; the centre copy of an odd m at
 * 0) and at (s + p) D octaves from the group's note, D = detune_cents / 1200,
 * s = (shift - 100) / 100. Blend tapers the outer copies (weight
 * 1 - (1-blend)*|p|), normalized so summed power stays at the single-copy
 * level (detuned copies decorrelate, so power - not amplitude - is what
 * adds). The copies render inside AMY unison-cluster oscs: one osc, or an
 * L/R pair when stereo; each osc deals its own copies into the same G groups
 * through the engine's unison_groups and interval fields, so one grid per
 * osc serves every group. Index map: contract in voice_config.h. */
typedef struct {
    uint8_t groups;
    float   interval[3];   /* octaves of groups 1..3 from the note */
} unison_stack_def_t;

#define ST(semis) ((float)(semis) / 12.0f)
static const unison_stack_def_t UNISON_STACKS[VOICE_UNISON_STACK_COUNT] = {
    [VOICE_UNISON_STACK_OFF]     = { 1u, { 0.0f,   0.0f,   0.0f } },
    [VOICE_UNISON_STACK_OCT]     = { 2u, { 1.0f,   0.0f,   0.0f } },
    [VOICE_UNISON_STACK_SUB]     = { 2u, { -1.0f,  0.0f,   0.0f } },
    [VOICE_UNISON_STACK_OCT_SUB] = { 3u, { -1.0f,  1.0f,   0.0f } },
    [VOICE_UNISON_STACK_FIFTH]   = { 2u, { ST(7),  0.0f,   0.0f } },
    [VOICE_UNISON_STACK_POWER]   = { 3u, { ST(7),  1.0f,   0.0f } },
    [VOICE_UNISON_STACK_OCT2]    = { 3u, { 1.0f,   2.0f,   0.0f } },
    [VOICE_UNISON_STACK_FOURTH]  = { 2u, { ST(5),  0.0f,   0.0f } },
    [VOICE_UNISON_STACK_MAJOR]   = { 4u, { ST(4),  ST(7),  1.0f } },
    [VOICE_UNISON_STACK_MINOR]   = { 4u, { ST(3),  ST(7),  1.0f } },
    /* Just harmonics: log2 of 2, 3 and 4. */
    [VOICE_UNISON_STACK_HARM3]   = { 3u, { 1.0f,   1.5849625f, 0.0f } },
    [VOICE_UNISON_STACK_HARM4]   = { 4u, { 1.0f,   1.5849625f, 2.0f } },
};
#undef ST

voice_unison_t voice_unison_defaults(void)
{
    return (voice_unison_t){
        .count = 3u, .detune_cents = 0u, .spread_pct = 50u, .blend_pct = 100u,
        .stereo = 1u, .bypass = 1u, .shift = 100u, .interval_unused = 0u,
        .stack = VOICE_UNISON_STACK_OCT_SUB,
    };
}

static const unison_stack_def_t *unison_stack(const voice_unison_t *u)
{
    uint8_t s = (u && u->stack < VOICE_UNISON_STACK_COUNT) ? u->stack : 0u;
    return &UNISON_STACKS[s];
}

uint8_t voice_unison_groups(uint8_t stack)
{
    return (stack < VOICE_UNISON_STACK_COUNT) ? UNISON_STACKS[stack].groups : 1u;
}

/* Groups actually dealt under n copies (the stack's, cut to n) and the
 * copies each one takes. */
static uint8_t unison_groups_of(const voice_unison_t *u, uint8_t n)
{
    uint8_t g = unison_stack(u)->groups;
    if (g < 1u) g = 1u;
    return (g > n) ? n : g;
}

static uint8_t unison_group_size(const voice_unison_t *u, uint8_t n)
{
    uint8_t g = unison_groups_of(u, n);
    return (g > 0u) ? (uint8_t)(n / g) : n;
}

static inline bool unison_is_pair(uint8_t n, bool stereo)
{
    return stereo && n >= 2u;
}

/* With stereo on, R carries a copy of a group only when the group has two
 * or more; one copy per group plays centred on osc 0 and osc 1 stays silent. */
static bool unison_pair_split(const voice_unison_t *u, uint8_t n)
{
    return u && u->stereo && n >= 2u && unison_group_size(u, n) >= 2u;
}

uint8_t voice_unison_oscs_per_voice(uint8_t n, bool stereo)
{
    return (uint8_t)((unison_is_pair(n, stereo) ? 2u : 1u) + VOICE_LFO_PAIR_OSCS);
}

uint8_t voice_unison_head_osc(uint8_t group, uint8_t n, bool stereo)
{
    return (unison_is_pair(n, stereo) && group) ? 1u : 0u;
}

uint8_t voice_unison_copy_osc(const voice_unison_t *u, uint8_t i, uint8_t n)
{
    if (!unison_pair_split(u, n)) return 0u;
    uint8_t m = unison_group_size(u, n);
    return voice_unison_head_osc((uint8_t)((i % m) & 1u), n, true);
}

uint8_t voice_unison_copies_mask(uint8_t n, bool stereo)
{
    return unison_is_pair(n, stereo) ? 0x03u : 0x01u;
}

uint8_t voice_unison_heads_mask(uint8_t n, bool stereo)
{
    return (n >= 2u) ? voice_unison_copies_mask(n, stereo) : 0u;
}

static float unison_pos(uint8_t j, uint8_t m)
{
    return (m > 1u) ? (2.0f * (float)j / (float)(m - 1u) - 1.0f) : 0.0f;
}

static float unison_weight(uint8_t j, uint8_t m, float blend)
{
    return 1.0f - (1.0f - blend) * fabsf(unison_pos(j, m));
}

static float unison_amp_norm(uint8_t m, float blend)
{
    float sum = 0.0f;
    for (uint8_t j = 0; j < m; j++) {
        float w = unison_weight(j, m, blend);
        sum += w * w;
    }
    /* blend 0 with no center copy (even m) zeroes every weight; fall back to
     * the equal-weight norm instead of dividing by zero. */
    if (sum < 1e-6f) return 1.0f / sqrtf((float)m);
    return 1.0f / sqrtf(sum);
}

/* Stereo pair: one AMY unison-cluster osc per side. The engine renders copy
 * k of each group at logfreq + offset + k * spacing (octaves), group g
 * transposed by its interval, and power-normalizes the weights over the
 * osc's own copies. Side g takes the even (L) or odd (R) positions of every
 * group's m-copy grid, so spacing = 4D/(m-1) and offset_g = -D + 2gD/(m-1) -
 * together the pitch set of the centred group. An odd m puts (m+1)/2 copies
 * of each group in L (both outermost ones among them) and (m-1)/2 in R. The
 * amp CONST carries the ratio between the all-m normalization and the
 * engine's per-side one (1/sqrt(2) at blend 1, even m), so the pair sums to
 * the level of the centred cluster; every group shares the same split, so
 * the per-group ratio is the voice's. */
static float unison_side_power(uint8_t m, uint8_t side, float blend)
{
    float sum = 0.0f;
    for (uint8_t j = side; j < m; j = (uint8_t)(j + 2u)) {
        float w = unison_weight(j, m, blend);
        sum += w * w;
    }
    return sum;
}

float voice_unison_osc_level(const voice_unison_t *u, uint8_t group, uint8_t n)
{
    if (!u || !u->stereo || n < 2u) return 1.0f;
    if (!unison_pair_split(u, n)) return group ? 0.0f : 1.0f;
    uint8_t m = unison_group_size(u, n);
    float blend = (float)u->blend_pct / 100.0f;
    return unison_amp_norm(m, blend) * sqrtf(unison_side_power(m, group, blend));
}

/* Pair pans. spread is the pair's width: 0 keeps both clusters at the
 * centre, 100 % puts them hard left and right, the docs' stereo-pair recipe.
 * With an odd m the sides differ in power (L has the extra copy); the louder
 * one sits nearer the centre by the power ratio so the image stays centred
 * under AMY's equal-power pan (gains sqrt(1-p), sqrt(p)). */
static float unison_engine_pan(const voice_unison_t *u, uint8_t group, uint8_t n)
{
    uint8_t m = unison_group_size(u, n);
    float spread = (float)u->spread_pct / 100.0f;
    float blend  = (float)u->blend_pct / 100.0f;
    float pl = unison_side_power(m, 0u, blend);
    float pr = unison_side_power(m, 1u, blend);
    if (pl < 1e-6f || pr < 1e-6f) pl = pr = 1.0f;
    float r_l = (pl > pr) ? pr / pl : 1.0f;
    float r_r = (pr > pl) ? pl / pr : 1.0f;
    return group ? (0.5f + 0.5f * spread * r_r) : (0.5f - 0.5f * spread * r_l);
}

/* The unison fields of cluster osc `group` for n >= 2 copies. The engine
 * tapers each copy by its distance from its group's note, reaching its blend
 * at the copy farthest from it, so a shifted group's far side plays quieter.
 * The app's blend is the weight at the farthest point a group can reach,
 * (1 + |s|) D from its note; each osc's blend is rescaled from its own
 * farthest copy to that. Stereo off: osc 0 carries all n copies and reaches
 * that point itself, so blend passes through; pan stays centred and the
 * engine's power normalization is the level. Stereo on: the pair above,
 * whose power split keeps the centred law. The groups and their intervals
 * go to every osc as they are; the engine deals each osc's copies into them
 * in the same order the grid interleaves, so the sides stay aligned. */
static void unison_engine_coefs(amy_event *e, uint8_t group, uint8_t n,
                                const voice_unison_t *u, float base_amp)
{
    const unison_stack_def_t *st = unison_stack(u);
    uint8_t G     = unison_groups_of(u, n);
    uint8_t m     = unison_group_size(u, n);
    float blend = (float)u->blend_pct / 100.0f;
    float d     = (float)u->detune_cents / 1200.0f;
    float s     = ((float)u->shift - 100.0f) / 100.0f;
    float reach = 1.0f + fabsf(s);
    float grid  = (m > 1u) ? 1.0f / (float)(m - 1u) : 0.0f;
    e->unison_groups    = G;
    e->unison_interval1 = (G >= 2u) ? st->interval[0] : 0.0f;
    e->unison_interval2 = (G >= 3u) ? st->interval[1] : 0.0f;
    e->unison_interval3 = (G >= 4u) ? st->interval[2] : 0.0f;
    if (!unison_pair_split(u, n)) {
        if (group) {
            /* Stereo on with one copy per group: nothing for the R osc. */
            e->unison_count   = 1u;
            e->unison_groups  = 1u;
            e->unison_interval1 = 0.0f;
            e->unison_interval2 = 0.0f;
            e->unison_interval3 = 0.0f;
            e->unison_spacing = 0.0f;
            e->unison_offset  = 0.0f;
            e->unison_blend   = 1.0f;
            e->pan_coefs[COEF_CONST] = 0.5f;
            e->amp_coefs[COEF_CONST] = 0.0f;
            return;
        }
        e->unison_count   = (uint8_t)(G * m);
        e->unison_spacing = 2.0f * d * grid;
        e->unison_offset  = d * (s + unison_pos(0u, m));
        e->unison_blend   = blend;
        e->pan_coefs[COEF_CONST] = 0.5f;
        e->amp_coefs[COEF_CONST] = base_amp;
        return;
    }
    /* An odd m leaves R one grid step short of the group's edge. */
    uint8_t last = (uint8_t)(m - 1u - ((m - 1u - group) & 1u));
    float span   = fmaxf(fabsf(s + unison_pos(group, m)), fabsf(s + unison_pos(last, m)));
    e->unison_count   = (uint8_t)(G * ((m + 1u - group) / 2u));
    e->unison_spacing = 4.0f * d * grid;
    e->unison_offset  = -d + s * d + 2.0f * (float)group * d * grid;
    e->unison_blend   = 1.0f - (1.0f - blend) * span / reach;
    e->pan_coefs[COEF_CONST] = unison_engine_pan(u, group, n);
    e->amp_coefs[COEF_CONST] = base_amp * voice_unison_osc_level(u, group, n);
    /* The engine spreads each osc's copies evenly over one cycle from the
     * osc's own phase; R starting at g/n keeps the pair from restarting
     * identical. A set trigger_phase also restarts the copies on each
     * note-on. */
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

    /* Audible oscs: osc 0 alone, or the L/R cluster pair 0, 1 when stereo
     * (any reserved LFO pair sits above them - the caller sized
     * oscs_per_voice). n == 1 emits exactly the single-osc skeleton, with no
     * unison, pan or phase terms, so unison off is bit-for-bit the single-osc
     * build. Each audible osc is a full voice osc: note-following carrier
     * (COEF_NOTE=1) with EG0-gated amplitude, velocity, level, filter, dist
     * and pan, rendering its share of the copies internally. */
    uint8_t n = 1;
    if (cfg->unison && !cfg->unison->bypass && cfg->unison->count > 1u && cfg->wave != KS)
        n = cfg->unison->count;
    if (n > VOICE_UNISON_MAX_COPIES) n = VOICE_UNISON_MAX_COPIES;
    bool stereo = cfg->unison && cfg->unison->stereo;
    uint8_t audible = (uint8_t)(voice_unison_oscs_per_voice(n, stereo) - VOICE_LFO_PAIR_OSCS);

    for (uint8_t i = 0; i < audible; i++) {
        e = amy_helpers_event_begin();
        e->synth = cfg->synth;
        e->osc   = i;
        e->wave  = cfg->wave;
        if (cfg->wt_preset >= 0) e->preset = cfg->wt_preset;
        if (cfg->wave == KS) {
            /* Authored feedback drives KS string decay; the 0 "never set"
             * sentinel falls back to VOICE_KS_FEEDBACK_DEFAULT. */
            float fb = cfg->ks_feedback;
            if (fb > 1.0f) fb = 1.0f;   /* > 1 would make the KS buffer diverge */
            e->feedback = (cfg->ks_feedback_authored && fb > 0.0f) ? fb : VOICE_KS_FEEDBACK_DEFAULT;
            e->duty_coefs[COEF_CONST] = 0.5f + cfg->ks_duty_ofs;
        }
        e->freq_coefs[COEF_NOTE] = 1.0f;
        e->amp_coefs[COEF_CONST] = cfg->osc0_amp_const;
        e->amp_coefs[COEF_VEL]   = cfg->osc0_amp_vel;
        e->amp_coefs[COEF_EG0]   = 1.0f;
        if (n > 1u)
            unison_engine_coefs(e, i, n, cfg->unison, cfg->osc0_amp_const);
        amy_helpers_event_send(e);
    }

    /* Park every osc the caller reserved above the audible layout (the native
     * LFO carrier pair without CONFIG_SEQ_LFO_SHARED; why: voice_wave_cfg_t).
     * Their owner raises them. An empty range when nothing is reserved. */
    voice_park_oscs(cfg->synth, audible, cfg->oscs_per_voice);

    /* A rebuild must not drop the stage, so the owner's block rides every
     * build. cfg->dist == NULL means the caller has none to assert yet. */
    if (cfg->dist) {
        if (n > 1u) {
            for (uint8_t i = 0; i < audible; i++)
                voice_apply_dist_osc(cfg->synth, i, cfg->dist);
        } else {
            voice_apply_dist(cfg->synth, cfg->dist);
        }
    }
}

void voice_push_unison_live(uint8_t synth, const voice_unison_t *u,
                            float base_amp)
{
    if (!u || u->bypass || u->count <= 1u || u->count > VOICE_UNISON_MAX_COPIES) return;
    uint8_t n      = u->count;
    bool    stereo = u->stereo;
    uint8_t oscs   = (uint8_t)(voice_unison_oscs_per_voice(n, stereo) - VOICE_LFO_PAIR_OSCS);
    for (uint8_t g = 0; g < oscs; g++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth = synth;
        e->osc   = voice_unison_head_osc(g, n, stereo);
        unison_engine_coefs(e, g, n, u, base_amp);
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

/* The LFO source pair, shared by the native topology and the LFO bank:
 * contract in voice_config.h. */
void voice_lfo_source_apply(uint8_t synth, uint8_t carrier_osc,
                            uint8_t wobble_osc, const seq_lfo_t *lfo,
                            uint16_t bpm)
{
    amy_event *e;

    if (!lfo || !lfo->enabled) {
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
        return;
    }

    /* Carrier: BPM-synced, no pitch tracking / velocity / envelope. WOBBLE
     * (second-order LFO) chains the wobble osc as the carrier's own
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
    e->freq_coefs[COEF_CONST] = lfo_rate_to_hz((note_div_t)lfo->wob_rate, bpm);
    e->freq_coefs[COEF_NOTE]  = 0.0f;
    e->freq_coefs[COEF_BEND]  = 0.0f;
    e->amp_coefs[COEF_CONST]  = (lfo->wob_depth > 0) ? 1.0f : 0.0f;
    e->amp_coefs[COEF_VEL]    = 0.0f;
    e->amp_coefs[COEF_EG0]    = 0.0f;
    amy_helpers_event_send(e);
}

void voice_lfo_source_set_rate(uint8_t synth, uint8_t carrier_osc,
                               uint8_t wobble_osc, const seq_lfo_t *lfo,
                               uint16_t bpm)
{
    if (!lfo) return;
    amy_event *e = amy_helpers_event_begin();
    e->synth                  = synth;
    e->osc                    = carrier_osc;
    e->freq_coefs[COEF_CONST] = lfo_rate_to_hz(lfo->rate, bpm);
    amy_helpers_event_send(e);

    e = amy_helpers_event_begin();
    e->synth                  = synth;
    e->osc                    = wobble_osc;
    e->freq_coefs[COEF_CONST] = lfo_rate_to_hz((note_div_t)lfo->wob_rate, bpm);
    amy_helpers_event_send(e);
}

/* Shared-LFO wiring: contract in voice_config.h. Same depth laws as the
 * COEF_MOD rails of voice_apply_native_lfo_topo, on the given slot. */
void voice_lfo_wire(uint8_t synth, uint8_t slot, uint16_t voice_mask,
                    uint16_t pitch_mask, uint16_t scan_mask,
                    const seq_lfo_t *lfo, uint8_t lfo_synth)
{
    assert(slot < NUM_MOD_SOURCES);
    const int c    = slot ? COEF_MOD1 : COEF_MOD0;
    uint16_t all   = (uint16_t)(voice_mask | pitch_mask | scan_mask);
    bool     wired = lfo && lfo->enabled && lfo->targets &&
                     lfo_synth != LFO_BANK_NONE;
    float    d     = wired ? (float)lfo->depth / 100.0f : 0.0f;
    for (uint8_t o = 0; (uint16_t)(all >> o) != 0u; o++) {
        uint16_t bit = (uint16_t)(1u << o);
        if (!(all & bit)) continue;
        amy_event *e = amy_helpers_event_begin();
        e->synth           = synth;
        e->osc             = o;
        e->mod_synth[slot] = wired ? lfo_synth : LFO_WIRE_UNSET;
        e->filter_freq_coefs[c] = 0.0f;
        e->amp_coefs[c]         = 0.0f;
        e->freq_coefs[c]        = 0.0f;
        e->duty_coefs[c]        = 0.0f;
        e->pan_coefs[c]         = 0.0f;
        e->dist_drive_coefs[c]  = 0.0f;
        e->dist_mix_coefs[c]    = 0.0f;
        if (wired) {
            if ((pitch_mask & bit) && LFO_HAS_TGT(lfo, LFO_TARGET_PITCH))
                e->freq_coefs[c] = d * VOICE_LFO_DEPTH_PITCH;
            if ((scan_mask & bit) && LFO_HAS_TGT(lfo, LFO_TARGET_SCAN))
                e->duty_coefs[c] = d * VOICE_LFO_DEPTH_SCAN;
            if (voice_mask & bit) {
                if (LFO_HAS_TGT(lfo, LFO_TARGET_FILTER)) e->filter_freq_coefs[c] = voice_lfo_filter_octaves(lfo);
                if (LFO_HAS_TGT(lfo, LFO_TARGET_AMP))    e->amp_coefs[c]         = d * VOICE_LFO_DEPTH_AMP;
                if (LFO_HAS_TGT(lfo, LFO_TARGET_DIST_DRIVE))
                    e->dist_drive_coefs[c] = d * VOICE_LFO_DEPTH_DIST_OCT;
                if (LFO_HAS_TGT(lfo, LFO_TARGET_DIST_MIX))
                    e->dist_mix_coefs[c]   = d * VOICE_LFO_DEPTH_DIST_MIX;
                if (LFO_HAS_TGT(lfo, LFO_TARGET_PAN)) {
                    /* Center baseline in the same event, as the native path. */
                    e->pan_coefs[COEF_CONST] = 0.5f;
                    e->pan_coefs[c]          = d * VOICE_LFO_DEPTH_PAN;
                }
            }
        }
        amy_helpers_event_send(e);
    }

    /* AMP overshoot cap (VOICE_LFO_DEPTH_AMP): the pre-dropped synth level
     * while AMP is wired on this slot, unity on every other call. */
    bool amp = wired && voice_mask != 0u && LFO_HAS_TGT(lfo, LFO_TARGET_AMP);
    synth_level_set_lfo_cap(synth, slot,
                            amp ? powf(10.0f, -3.0f * d * VOICE_LFO_DEPTH_AMP) : 1.0f);
}

#if !CONFIG_SEQ_LFO_SHARED
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
         * per-voice stages ride whatever carries them (osc 0, or the
         * cluster oscs under unison). */
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

        /* AMP overshoot cap, as voice_lfo_wire(). */
        synth_level_set_lfo_cap(synth, 0, (voice_mask != 0u && LFO_HAS_TGT(lfo, LFO_TARGET_AMP))
                                          ? powf(10.0f, -3.0f * d * VOICE_LFO_DEPTH_AMP) : 1.0f);

        voice_lfo_source_apply(synth, carrier_osc, wobble_osc, lfo, bpm);
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
        synth_level_set_lfo_cap(synth, 0, 1.0f);

        voice_lfo_source_apply(synth, carrier_osc, wobble_osc, NULL, bpm);
    }
}

/* The wave-build layout (carrier = osc1, osc0 coupled): the signature every
 * WAVE-mode engine calls. */
void voice_apply_native_lfo(uint8_t synth, const seq_lfo_t *lfo, uint16_t bpm)
{
    voice_apply_native_lfo_topo(synth, lfo, bpm, 1, 0x01, 0x01);
}
#endif /* !CONFIG_SEQ_LFO_SHARED */
