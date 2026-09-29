#include "custompatches/fm_voice.h"
#include "amy.h"           /* ALGO, SINE, COEF_*, ENVELOPE_*, MAX_ALGO_OPS, custom rows */
#include "amy_helpers.h"   /* amy_helpers_event_begin/send */
#include "voice_config.h"  /* VOICE_ENV_* clamps */
#include "seq_clamp.h"
#include <string.h>

/* FM_NUM_OPS must track amy.h's MAX_ALGO_OPS (algo_source[] size): an AMY
 * update that changes it would silently truncate the wiring below. */
_Static_assert(FM_NUM_OPS == MAX_ALGO_OPS, "FM_NUM_OPS must equal AMY's MAX_ALGO_OPS");
_Static_assert(FM_NUM_OPS == FM_GRAPH_OPS, "fm_graph and fm_voice disagree on operator count");

/* The live-editable "custom" FM voice; ownership convention in fm_voice.h
 * (mirrors amy_fx.h's s_fx). */
fm_voice_t s_fm_voice;

/* Default operator envelope: short and percussive so a fresh voice decays
 * before any operator ADSR is authored. */
#define FM_OP_ATTACK_MS  4u
#define FM_OP_DECAY_MS   300u
#define FM_OP_SUSTAIN    60u
#define FM_OP_RELEASE_MS 200u

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
    if (!fm_graph_compile(v->op_to, v->fb_op, p)) {
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

void fm_voice_default(fm_voice_t *v)
{
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->algorithm = 1;
    v->feedback  = 0.0f;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        v->op_ratio[i] = 1.0f;
        v->op_level[i] = 0.0f;
        v->op_env[i] = (seq_env_t) {
            .attack_ms   = FM_OP_ATTACK_MS,
            .decay_ms    = FM_OP_DECAY_MS,
            .sustain_pct = FM_OP_SUSTAIN,
            .release_ms  = FM_OP_RELEASE_MS,
            .eg_type     = ENVELOPE_DX7,       /* the DX7 attack curve: what makes modulator envelopes sound right */
        };
    }
    /* Algorithm 1's second chain: index 4 (OP2) modulates index 5 (OP1).
     * Enabling only this pair makes a fresh voice audible as a 2-op tone. */
    v->op_ratio[4] = 1.0f;  v->op_level[4] = 0.5f;
    v->op_ratio[5] = 1.0f;  v->op_level[5] = 1.0f;
    /* Seed the custom fields from the same row so entering custom mode (or a
     * topology edit) starts from what is heard. */
    fm_graph_view_t g;
    fm_voice_graph(v, &g);
    fm_graph_to_forest(&g, v->op_to);
    v->fb_op = g.fb_op;
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

/* One operator osc: ratio, level, own EG0. Sent after the routing event so
 * the eg_type re-assert lands after AMY's ALGO_SOURCE naming (which forces
 * ENVELOPE_DX7 on the operator). */
static void fm_voice_send_op(uint8_t synth_id, const fm_voice_t *voice, uint8_t op)
{
    const seq_env_t *env = &voice->op_env[op];
    amy_event *e = amy_helpers_event_begin();
    e->synth                 = synth_id;
    e->osc                   = (uint16_t)(op + 1);
    e->wave                  = SINE;
    e->ratio                 = voice->op_ratio[op];
    e->freq_coefs[COEF_NOTE] = 1.0f;
    e->amp_coefs[COEF_CONST] = voice->op_level[op];
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
    e->eg0_times[0]  = SEQ_CLAMP_U32(env->attack_ms, VOICE_ENV_ATTACK_MIN_MS,
                                     VOICE_ENV_TIME_MAX_MS);
    e->eg0_values[0] = 1.0f;
    e->eg0_times[1]  = SEQ_CLAMP_U32(env->decay_ms, 0u, VOICE_ENV_TIME_MAX_MS);
    e->eg0_values[1] = (float)SEQ_CLAMP_U8(env->sustain_pct, 0u, 100u) / 100.0f;
    e->eg0_times[2]  = SEQ_CLAMP_U32(env->release_ms, VOICE_ENV_RELEASE_MIN_MS,
                                     VOICE_ENV_TIME_MAX_MS);
    e->eg0_values[2] = 0.0f;
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
    else                               fm_voice_push_live(synth_id, voice);
}

/* ── Routing queries / edits ────────────────────────────────────────────── */

void fm_voice_graph(const fm_voice_t *v, fm_graph_view_t *out)
{
    if (v->algorithm == FM_ALGO_CUSTOM) {
        fm_graph_from_forest(v->op_to, v->fb_op, out);
        return;
    }
    const uint8_t *ops = amy_algorithm_ops(v->algorithm);
    if (!ops) ops = amy_algorithm_ops(1);
    fm_graph_decode(ops, out);
}

void fm_voice_make_custom(fm_voice_t *v)
{
    if (v->algorithm == FM_ALGO_CUSTOM) return;
    fm_graph_view_t g;
    fm_voice_graph(v, &g);
    fm_graph_to_forest(&g, v->op_to);
    v->fb_op     = g.fb_op;
    v->algorithm = FM_ALGO_CUSTOM;
}

bool fm_voice_set_op_target(fm_voice_t *v, uint8_t op, uint8_t target)
{
    if (op >= FM_NUM_OPS) return false;
    fm_voice_t trial = *v;
    fm_voice_make_custom(&trial);
    if (!fm_graph_edge_allowed(trial.op_to, op, target)) return false;
    trial.op_to[op] = target;
    fm_program_t p;
    if (!fm_graph_compile(trial.op_to, trial.fb_op, &p)) return false;
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
        fm_voice_make_custom(v);           /* wrap through the custom slot */
        return v->algorithm;
    }
    v->algorithm = (uint8_t)a;
    return v->algorithm;
}
