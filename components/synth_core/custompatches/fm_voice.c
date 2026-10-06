#include "custompatches/fm_voice.h"
#include "amy.h"           /* ALGO, SINE, COEF_*, ENVELOPE_*, MAX_ALGO_OPS, custom rows */
#include "amy_helpers.h"   /* amy_helpers_event_begin/send */
#include "voice_config.h"  /* VOICE_ENV_* clamps */
#include "seq_clamp.h"
#include <math.h>
#include <string.h>

/* FM_NUM_OPS must track amy.h's MAX_ALGO_OPS (algo_source[] size): an AMY
 * update that changes it would silently truncate the wiring below. */
_Static_assert(FM_NUM_OPS == MAX_ALGO_OPS, "FM_NUM_OPS must equal AMY's MAX_ALGO_OPS");
_Static_assert(FM_NUM_OPS == FM_GRAPH_OPS, "fm_graph and fm_voice disagree on operator count");

/* The live-editable "custom" FM voice; ownership convention in fm_voice.h
 * (mirrors amy_fx.h's s_fx). */
fm_voice_t s_fm_voice;

/* Default operator envelope: short and percussive so a fresh voice decays
 * before any operator envelope is authored. L93 is ~0.59 linear. */
static const fm_op_env_t s_fm_op_env_default = {
    .time_ms = { 4u, 300u, 0u, 200u },
    .level   = { 99u, 93u, 93u, 0u },
    .eg_type = ENVELOPE_DX7,       /* the DX7 attack curve: what makes modulator envelopes sound right */
};

#define FM_FIXED_HZ_DEFAULT 440.0f

float fm_voice_level_to_amp(uint8_t level)
{
    if (level == 0u) return 0.0f;
    if (level > 99u) level = 99u;
    return exp2f(((float)level - 99.0f) / 8.0f);
}

/* ── Custom program double-buffer ─────────────────────────────────────────
 * The compiled program lives in one of AMY's AMY_NUM_CUSTOM_ALGORITHMS RAM
 * rows. A changed program is written to the row NOT currently referenced,
 * then the algorithm/algo_source event switches every synth to it in one
 * apply under amy_queue_lock, so the render body never reads a half-written
 * row and the FIFO ordering of edits is preserved. */
static fm_program_t s_prog;             /* what the referenced row holds */
static uint8_t      s_prog_slot = 0;
static bool         s_prog_valid = false;

static void fm_program_identity(fm_program_t *p, uint8_t algorithm)
{
    const uint8_t *ops = amy_algorithm_ops(algorithm);
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        p->ops[i]     = ops ? ops[i] : 0;
        p->slot_op[i] = i;
    }
}

/* Resolve the voice's routing to an AMY algorithm index + slot order,
 * publishing a custom program to a RAM row when it changed. */
static uint8_t fm_voice_resolve_program(const fm_voice_t *v, fm_program_t *p)
{
    if (v->algorithm != FM_ALGO_CUSTOM) {
        uint8_t a = (v->algorithm < amy_num_algorithms) ? v->algorithm : 1;
        fm_program_identity(p, a);
        return a;
    }
    if (!fm_graph_compile(v->op_targets, v->fb_op, p)) {
        /* Setters never store an uncompilable graph; a corrupt voice falls
         * back to row 1 rather than pushing garbage routing. */
        fm_program_identity(p, 1);
        return 1;
    }
    if (!s_prog_valid || memcmp(&s_prog, p, sizeof(*p)) != 0) {
        s_prog_slot  = (uint8_t)((s_prog_slot + 1u) % AMY_NUM_CUSTOM_ALGORITHMS);
        s_prog       = *p;
        s_prog_valid = true;
        amy_set_custom_algorithm(s_prog_slot, p->ops);
    }
    return (uint8_t)(amy_num_algorithms + s_prog_slot);
}

/* Copy the table routing into the custom fields. A decoded mask of 0 (a bus
 * write nothing reads; no DX7 row has one) becomes a carrier. */
static void fm_voice_seed_custom(fm_voice_t *v)
{
    fm_graph_view_t g;
    fm_voice_graph(v, &g);
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        v->op_targets[i] = g.out_mask[i] ? g.out_mask[i] : FM_OUT_BIT;
    }
    v->fb_op = g.fb_op;
}

void fm_voice_default(fm_voice_t *v)
{
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->algorithm = 1;
    v->feedback  = 0.0f;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        v->op_ratio[i]    = 1.0f;
        v->op_fixed_hz[i] = FM_FIXED_HZ_DEFAULT;
        v->op_level[i]    = 0.0f;
        v->op_env[i]      = s_fm_op_env_default;
    }
    /* Algorithm 1's second chain: index 4 (OP2) modulates index 5 (OP1).
     * Enabling only this pair makes a fresh voice audible as a 2-op tone. */
    v->op_ratio[4] = 1.0f;  v->op_level[4] = 0.5f;
    v->op_ratio[5] = 1.0f;  v->op_level[5] = 1.0f;
    /* Seed the custom fields from the same row so entering custom mode (or a
     * topology edit) starts from what is heard. */
    fm_voice_seed_custom(v);
}

/* osc 0 routing event: algorithm + algo_source order + feedback. */
static void fm_voice_send_routing(uint8_t synth_id, const fm_voice_t *voice)
{
    fm_program_t p;
    uint8_t algo = fm_voice_resolve_program(voice, &p);

    amy_event *e = amy_helpers_event_begin();
    e->synth     = synth_id;
    e->osc       = 0;
    e->algorithm = algo;
    e->feedback  = voice->feedback;
    /* algo_source[s] is voice-relative; AMY offsets it by the voice's base
     * osc internally. Slot s renders operator slot_op[s] = osc slot_op[s]+1. */
    for (uint8_t s = 0; s < FM_NUM_OPS; s++) {
        e->algo_source[s] = (int16_t)(p.slot_op[s] + 1);
    }
    /* The ALGORITHM delta force-switches the osc's eg_type[0] to the DX7
     * curve; this voice authors ENVELOPE_NORMAL, so re-assert it in the same
     * event or the first live edit silently changes the envelope shape. */
    e->eg_type[0] = ENVELOPE_NORMAL;
    amy_helpers_event_send(e);
}

/* One operator osc: frequency, level, own EG0. Sent after the routing event
 * so the eg_type re-assert lands after AMY's ALGO_SOURCE naming (which forces
 * ENVELOPE_DX7 on the operator). */
static void fm_voice_send_op(uint8_t synth_id, const fm_voice_t *voice, uint8_t op)
{
    const fm_op_env_t *env = &voice->op_env[op];
    uint8_t bit = (uint8_t)(1u << op);
    amy_event *e = amy_helpers_event_begin();
    e->synth                 = synth_id;
    e->osc                   = (uint16_t)(op + 1);
    e->wave                  = SINE;
    if (voice->op_fixed & bit) {
        /* No ratio: with one set, render_fm_sine derives the pitch from the
         * ALGO osc and ignores these coefs. */
        e->freq_coefs[COEF_CONST] = voice->op_fixed_hz[op];
        e->freq_coefs[COEF_NOTE]  = 0.0f;
    } else {
        e->ratio                  = voice->op_ratio[op];
        e->freq_coefs[COEF_NOTE]  = 1.0f;
    }
    e->amp_coefs[COEF_CONST] = (voice->op_mute & bit) ? 0.0f : voice->op_level[op];
    /* VEL must be 0 on operators: AMY never delivers velocity to
     * SYNTH_IS_ALGO_SOURCE oscs, so a nonzero VEL coef on their 0 input would
     * contribute -60 dB in the dB combine (voice_config.h) and floor the
     * operator to silence. Velocity comes from the ALGO control osc (VEL=1),
     * whose amp scales the carriers in render_algo. Same convention as the
     * built-in DX7 patch strings. */
    e->amp_coefs[COEF_VEL]   = 0.0f;
    e->amp_coefs[COEF_EG0]   = 1.0f;
    e->bp_is_set[0]          = 1;
    e->eg_type[0]            = env->eg_type;
    /* Five pairs: L4 at t=0, the three note-on segments (AMY sustains on the
     * pair before the last), then the release back to L4. */
    e->eg0_times[0]  = 0u;
    e->eg0_values[0] = fm_voice_level_to_amp(env->level[3]);
    e->eg0_times[1]  = SEQ_CLAMP_U32(env->time_ms[0], VOICE_ENV_ATTACK_MIN_MS,
                                     VOICE_ENV_TIME_MAX_MS);
    e->eg0_values[1] = fm_voice_level_to_amp(env->level[0]);
    e->eg0_times[2]  = SEQ_CLAMP_U32(env->time_ms[1], 0u, VOICE_ENV_TIME_MAX_MS);
    e->eg0_values[2] = fm_voice_level_to_amp(env->level[1]);
    e->eg0_times[3]  = SEQ_CLAMP_U32(env->time_ms[2], 0u, VOICE_ENV_TIME_MAX_MS);
    e->eg0_values[3] = fm_voice_level_to_amp(env->level[2]);
    e->eg0_times[4]  = SEQ_CLAMP_U32(env->time_ms[3], VOICE_ENV_RELEASE_MIN_MS,
                                     VOICE_ENV_TIME_MAX_MS);
    e->eg0_values[4] = fm_voice_level_to_amp(env->level[3]);
    amy_helpers_event_send(e);
}

void fm_voice_configure_track(uint8_t synth_id, uint16_t num_voices,
                              const fm_voice_t *voice)
{
    if (!voice) return;

    amy_event *e = amy_helpers_event_begin();
    e->synth          = synth_id;
    e->num_voices     = num_voices;
    e->oscs_per_voice = FM_NUM_OPS + 1;
    amy_helpers_event_send(e);

    /* osc 0: ALGO control osc. Carries the shared voice envelope/velocity and
     * the operator wiring. */
    e = amy_helpers_event_begin();
    e->synth                 = synth_id;
    e->osc                   = 0;
    e->wave                  = ALGO;
    e->freq_coefs[COEF_NOTE] = 1.0f;
    e->amp_coefs[COEF_CONST] = 1.0f;
    e->amp_coefs[COEF_VEL]   = 1.0f;
    e->amp_coefs[COEF_EG0]   = 1.0f;
    e->eg_type[0]            = ENVELOPE_NORMAL;
    e->eg0_times[0]  = 5;    e->eg0_values[0] = 1.0f;
    e->eg0_times[1]  = 250;  e->eg0_values[1] = 0.6f;
    e->eg0_times[2]  = 200;  e->eg0_values[2] = 0.0f;
    amy_helpers_event_send(e);

    fm_voice_push_live(synth_id, voice);
}

void fm_voice_push_live(uint8_t synth_id, const fm_voice_t *voice)
{
    if (!voice) return;
    fm_voice_send_routing(synth_id, voice);
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) fm_voice_send_op(synth_id, voice, i);
}

void fm_voice_push(uint8_t synth_id, const fm_voice_t *voice, uint8_t what)
{
    if (!voice) return;
    if (what < FM_NUM_OPS)             fm_voice_send_op(synth_id, voice, what);
    else if (what == FM_PUSH_ROUTING)  fm_voice_send_routing(synth_id, voice);
    else if (what >= FM_PUSH_OP_RESET(0) && what < FM_PUSH_OP_RESET(FM_NUM_OPS)) {
        uint8_t op = (uint8_t)(what - FM_PUSH_OP_RESET(0));
        amy_event *e = amy_helpers_event_begin();
        e->synth     = synth_id;
        e->reset_osc = (uint32_t)(op + 1);     /* voice-relative with synth set */
        amy_helpers_event_send(e);
        fm_voice_send_op(synth_id, voice, op);
    }
    else                               fm_voice_push_live(synth_id, voice);
}

/* ── Routing queries / edits ────────────────────────────────────────────── */

void fm_voice_graph(const fm_voice_t *v, fm_graph_view_t *out)
{
    if (v->algorithm == FM_ALGO_CUSTOM) {
        memcpy(out->out_mask, v->op_targets, sizeof(out->out_mask));
        out->fb_op = v->fb_op;
        return;
    }
    const uint8_t *ops = amy_algorithm_ops(v->algorithm);
    if (!ops) ops = amy_algorithm_ops(1);
    fm_graph_decode(ops, out);
}

void fm_voice_make_custom(fm_voice_t *v)
{
    if (v->algorithm == FM_ALGO_CUSTOM) return;
    fm_voice_seed_custom(v);
    v->algorithm = FM_ALGO_CUSTOM;
}

/* Shape rule of fm_graph.h: exactly FM_OUT_BIT, or a nonzero set of other
 * operators. */
static bool fm_voice_mask_valid(uint8_t op, uint8_t mask)
{
    if (mask == FM_OUT_BIT) return true;
    uint8_t ops = (uint8_t)((1u << FM_NUM_OPS) - 1u);
    return mask != 0u && (mask & (uint8_t)~ops) == 0u && (mask & (1u << op)) == 0u;
}

bool fm_voice_set_op_targets(fm_voice_t *v, uint8_t op, uint8_t mask, fm_route_err_t *err)
{
    fm_route_err_t e = FM_ROUTE_OK;
    fm_voice_t trial = *v;
    fm_program_t p;
    if (op >= FM_NUM_OPS || !fm_voice_mask_valid(op, mask)) {
        e = FM_ROUTE_INVALID;
    } else {
        fm_voice_make_custom(&trial);
        trial.op_targets[op] = mask;
        if (!fm_graph_is_acyclic(trial.op_targets)) {
            e = FM_ROUTE_LOOP;
        } else if (!fm_graph_compile(trial.op_targets, trial.fb_op, &p)) {
            e = FM_ROUTE_NO_BUS;
        }
    }
    if (err) *err = e;
    if (e != FM_ROUTE_OK) return false;
    *v = trial;
    return true;
}

bool fm_voice_set_fb_op(fm_voice_t *v, uint8_t fb_op)
{
    if (fb_op != FM_OP_NONE && fb_op >= FM_NUM_OPS) return false;
    fm_voice_make_custom(v);
    v->fb_op = fb_op;      /* feedback never changes bus needs: always compiles */
    return true;
}

uint8_t fm_voice_step_algorithm(fm_voice_t *v, int dir)
{
    int n = (int)amy_num_algorithms;      /* rows 1..n-1 are the DX7 set */
    int step = (dir > 0) ? 1 : -1;
    if (v->algorithm == FM_ALGO_CUSTOM) {
        v->algorithm = (uint8_t)((step > 0) ? 1 : n - 1);
        return v->algorithm;
    }
    int a = (int)v->algorithm + step;
    if (a <= 0 || a >= n) {
        v->algorithm = FM_ALGO_CUSTOM;     /* wrap through the custom slot */
        return v->algorithm;
    }
    v->algorithm = (uint8_t)a;
    return v->algorithm;
}

/* ── Value ranges and the edit grid ─────────────────────────────────────── */

/* Curated DX7-style harmonic ratios plus a few inharmonic ones, kept short
 * enough to step through. */
static const float s_fm_ratio_steps[] = {
    0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 5.0f,
    6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 14.0f, 16.0f,
};
#define FM_RATIO_STEP_COUNT ((int)(sizeof(s_fm_ratio_steps) / sizeof(s_fm_ratio_steps[0])))

static int fm_ratio_nearest_index(float ratio)
{
    int best = 0;
    float best_d = 1e9f;
    for (int i = 0; i < FM_RATIO_STEP_COUNT; i++) {
        float d = ratio - s_fm_ratio_steps[i];
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static uint32_t fm_time_min(uint8_t seg)
{
    return (seg == 0u) ? VOICE_ENV_ATTACK_MIN_MS
         : (seg == 3u) ? VOICE_ENV_RELEASE_MIN_MS : 0u;
}

static uint16_t fm_step_time(uint16_t t, int delta, uint32_t lo, uint32_t hi)
{
    int32_t v = (int32_t)t;
    int32_t dir = (delta > 0) ? 1 : -1;
    int n = (delta > 0) ? delta : -delta;
    for (int i = 0; i < n; i++) {
        int32_t s = (int32_t)((float)v * 0.08f + 0.5f);
        if (s < 1) s = 1;
        v += dir * s;
        v = SEQ_CLAMP_INT(v, (int32_t)lo, (int32_t)hi);
    }
    return (uint16_t)v;
}

void fm_voice_step(fm_voice_t *v, uint8_t op, fm_field_t field, int delta)
{
    if (field == FM_FIELD_FEEDBACK) {
        float fb = v->feedback + (float)delta * FM_LEVEL_STEP;
        v->feedback = SEQ_CLAMP_F32(fb, 0.0f, FM_FEEDBACK_MAX);
        return;
    }
    if (op >= FM_NUM_OPS) return;
    bool fixed = (v->op_fixed & (1u << op)) != 0u;
    fm_op_env_t *env = &v->op_env[op];
    switch (field) {
        case FM_FIELD_COARSE:
            if (fixed) {
                float hz = v->op_fixed_hz[op] * exp2f((float)delta / 12.0f);
                v->op_fixed_hz[op] = SEQ_CLAMP_F32(hz, FM_FIXED_HZ_MIN, FM_FIXED_HZ_MAX);
            } else {
                float ratio = v->op_ratio[op];
                int cur = fm_ratio_nearest_index(ratio);
                int next = SEQ_CLAMP_INT(cur + delta, 0, FM_RATIO_STEP_COUNT - 1);
                float r = s_fm_ratio_steps[next] * (ratio / s_fm_ratio_steps[cur]);
                v->op_ratio[op] = SEQ_CLAMP_F32(r, FM_RATIO_MIN, FM_RATIO_MAX);
            }
            break;
        case FM_FIELD_FINE:
            if (fixed) {
                float hz = v->op_fixed_hz[op] + FM_FINE_HZ * (float)delta;
                v->op_fixed_hz[op] = SEQ_CLAMP_F32(hz, FM_FIXED_HZ_MIN, FM_FIXED_HZ_MAX);
            } else {
                float r = v->op_ratio[op] + FM_FINE_HZ * (float)delta / FM_A4_HZ;
                v->op_ratio[op] = SEQ_CLAMP_F32(r, FM_RATIO_MIN, FM_RATIO_MAX);
            }
            break;
        case FM_FIELD_LEVEL: {
            float lvl = v->op_level[op] + (float)delta * FM_LEVEL_STEP;
            v->op_level[op] = SEQ_CLAMP_F32(lvl, 0.0f, 1.0f);
            break;
        }
        case FM_FIELD_T1: case FM_FIELD_T2: case FM_FIELD_T3: case FM_FIELD_T4: {
            uint8_t s = (uint8_t)(field - FM_FIELD_T1);
            env->time_ms[s] = fm_step_time(env->time_ms[s], delta, fm_time_min(s), VOICE_ENV_TIME_MAX_MS);
            break;
        }
        case FM_FIELD_L1: case FM_FIELD_L2: case FM_FIELD_L3: case FM_FIELD_L4: {
            uint8_t s = (uint8_t)(field - FM_FIELD_L1);
            env->level[s] = (uint8_t)SEQ_CLAMP_INT((int)env->level[s] + delta, 0, FM_EG_LEVEL_MAX);
            break;
        }
        default:
            break;
    }
}

bool fm_voice_toggle_fixed(fm_voice_t *v, uint8_t op)
{
    if (op >= FM_NUM_OPS) return false;
    uint8_t bit = (uint8_t)(1u << op);
    if (v->op_fixed & bit) {
        float r = v->op_fixed_hz[op] / FM_A4_HZ;
        v->op_ratio[op] = SEQ_CLAMP_F32(r, FM_RATIO_MIN, FM_RATIO_MAX);
        v->op_fixed = (uint8_t)(v->op_fixed & ~bit);
        return false;
    }
    float hz = FM_A4_HZ * v->op_ratio[op];
    v->op_fixed_hz[op] = SEQ_CLAMP_F32(hz, FM_FIXED_HZ_MIN, FM_FIXED_HZ_MAX);
    v->op_fixed = (uint8_t)(v->op_fixed | bit);
    return true;
}

static float fm_clamp_f32(float x, float lo, float hi, float dflt)
{
    if (!isfinite(x)) return dflt;
    return SEQ_CLAMP_F32(x, lo, hi);
}

void fm_voice_clamp(fm_voice_t *v)
{
    if (!v) return;
    const uint8_t all = (uint8_t)((1u << FM_NUM_OPS) - 1u);
    v->op_fixed = (uint8_t)(v->op_fixed & all);
    v->op_mute  = (uint8_t)(v->op_mute & all);
    v->feedback = fm_clamp_f32(v->feedback, 0.0f, FM_FEEDBACK_MAX, 0.0f);
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        fm_op_env_t *env = &v->op_env[i];
        v->op_ratio[i]    = fm_clamp_f32(v->op_ratio[i], FM_RATIO_MIN, FM_RATIO_MAX, 1.0f);
        v->op_fixed_hz[i] = fm_clamp_f32(v->op_fixed_hz[i], FM_FIXED_HZ_MIN, FM_FIXED_HZ_MAX,
                                         FM_FIXED_HZ_DEFAULT);
        v->op_level[i]    = fm_clamp_f32(v->op_level[i], 0.0f, 1.0f, 0.0f);
        for (uint8_t s = 0; s < 4u; s++) {
            env->time_ms[s] = (uint16_t)SEQ_CLAMP_U32(env->time_ms[s], fm_time_min(s),
                                                      VOICE_ENV_TIME_MAX_MS);
            if (env->level[s] > FM_EG_LEVEL_MAX) env->level[s] = FM_EG_LEVEL_MAX;
        }
        if (env->eg_type > ENVELOPE_TRUE_EXPONENTIAL) env->eg_type = ENVELOPE_DX7;
    }
    if (v->fb_op != FM_OP_NONE && v->fb_op >= FM_NUM_OPS) v->fb_op = FM_OP_NONE;
    if (v->algorithm != FM_ALGO_CUSTOM && v->algorithm >= amy_num_algorithms) v->algorithm = 1;
    fm_program_t p;
    if (!fm_graph_compile(v->op_targets, v->fb_op, &p)) {
        if (v->algorithm == FM_ALGO_CUSTOM) v->algorithm = 1;
        fm_voice_seed_custom(v);
    }
}
