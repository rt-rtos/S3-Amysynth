#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "custompatches/fm_graph.h"  /* fm_graph_view_t, FM_OUT_BIT, FM_OP_NONE */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Generic 6-operator DX7-style FM/ALGO voice engine ───────────────────────
 * Wraps AMY's ALGO wave type as one melodic voice: an ALGO control osc
 * (osc 0) plus six SINE operators (osc 1..6), so oscs_per_voice is always 7.
 *
 * Operator index i (0..5) is osc i+1 of the voice, always. In table mode it is
 * also AMY algo_source slot i; in custom mode the compiled program reorders
 * the slots (fm_graph.h) and algo_source[] carries the mapping. AMY's table
 * rows are authored DX7 op6 first, so the UI labels index i as OP(6-i): index
 * 5 is OP1, the leftmost carrier of every DX7 chart.
 *
 * Routing comes from one of two sources:
 *   algorithm < FM_ALGO_CUSTOM : AMY algorithms[] row (DX7 numbering, row 0
 *                                aliases row 1); op_targets/fb_op are ignored
 *                                but kept.
 *   algorithm == FM_ALGO_CUSTOM: the authored op_targets[] + fb_op,
 *                                compiled onto AMY's two buses at push time.
 * op_targets[i] is operator i's target set in the fm_graph_view_t.out_mask
 * encoding (fm_graph.h): FM_OUT_BIT alone (carrier), or one or more other
 * operators (fan-out). A fan-out connection has no depth of its own: the
 * modulator's level and envelope drive every target equally.
 * `feedback` is the amount on whichever operator the routing marks FB.
 *
 * Per-operator "level" is that operator's amp_coefs[COEF_CONST]: for a
 * modulator it IS the modulation index/brightness, for a carrier it scales
 * that carrier. Level 0 silences the operator. Each operator has its own EG0
 * (op_env); the ALGO control osc keeps the row's per-track ADSR as a VCA over
 * the carriers, so both shape the note.
 *
 * Frequency: an operator either tracks the note at op_ratio (bit clear in
 * op_fixed) or sounds op_fixed_hz regardless of the note (bit set).
 *
 * op_mute is audition state: a muted operator is pushed at level 0 while its
 * op_level is kept. Presets and fm_voice_default() leave it 0, and it is never
 * serialized when the FM voice gets a snapshot section. */

#define FM_NUM_OPS      6
#define FM_ALGO_CUSTOM  0xFF   /* algorithm: use op_targets[]/fb_op */

/* DX7-style 4-rate, 4-level operator envelope. From L4 the note-on moves to
 * L1 at R1, then to L2 at R2, then to L3 at R3, and holds L3 while the key is
 * down; the release returns to L4 at R4. Levels are DX7 0..99 (0.75 dB per
 * step, 99 = full scale, 0 = silence). Rates are DX7 0..99, higher is faster.
 * A rate is a slope, so how long a segment takes depends on its rate and on
 * the two levels it joins: fm_voice_env_times_ms(). */
typedef struct {
    uint8_t  rate[4];      /* R1..R4: DX7 rates 0..99; R4 is the release */
    uint8_t  level[4];     /* L1..L4: DX7 levels 0..99; L4 = start and release target */
    uint8_t  eg_type;      /* ENVELOPE_DX7 by default; no UI row */
} fm_op_env_t;

/* The four segment times of an envelope in ms (out_ms[0..3] = T1..T4), as
 * pushed to AMY. The one home of the rate law, which is AMY's fm.py
 * calc_loglin_eg_breakpoints (the conversion behind the built-in DX7 patches),
 * with levels in DX7 units:
 *   falling: (level drop) / (0.5 + 0.5 * 2^(R/6)) seconds, i.e. 1 level/s at
 *            R0 and 99 levels in about 2 ms at R99;
 *   rising:  the DX7 attack curve L(t) = 109 - 75 * exp(-t / tc) entered at
 *            the start level (34 if lower), tc = 8 ms * 2^((65 - R)/6);
 *   equal levels: 0, except the release, which is timed as a 60-level drop.
 * Then held to what AMY is sent: T1 at least VOICE_ENV_ATTACK_MIN_MS, T4 at
 * least VOICE_ENV_RELEASE_MIN_MS, every time at most VOICE_ENV_TIME_MAX_MS
 * (voice_config.h). Rates and levels above 99 are read as 99. Pure. */
void fm_voice_env_times_ms(const fm_op_env_t *env, uint32_t out_ms[4]);

/* DX7 level (0..99) to linear amplitude: 0.75 dB per step below 99, the law of
 * AMY's fm.py dx7level_to_linear, except that L0 is exact silence. The one
 * home of this law: the envelope push and the page-2 plot both use it. */
float fm_voice_level_to_amp(uint8_t level);

typedef struct {
    uint8_t     algorithm;
    uint8_t     fb_op;                   /* custom mode: FM_OP_NONE or 0..5    */
    uint8_t     op_targets[FM_NUM_OPS];  /* custom mode: target mask, see above */
    float       op_ratio[FM_NUM_OPS];    /* per-operator frequency ratio       */
    float       op_fixed_hz[FM_NUM_OPS]; /* fixed-mode frequency, Hz           */
    float       op_level[FM_NUM_OPS];    /* per-operator output level, 0..1    */
    fm_op_env_t op_env[FM_NUM_OPS];      /* per-operator EG0                   */
    float       feedback;                /* 0..~1.2                            */
    uint8_t     op_fixed;                /* bit i: operator i is fixed-frequency */
    uint8_t     op_mute;                 /* bit i: operator i muted (audition)  */
} fm_voice_t;

/* The single live-editable "custom" FM voice (SEQ_PATCH_FM_CUSTOM), owned by
 * this module. The FM UI screen mutates it (through the setters below for
 * anything topological) then calls sequencer_core_fm_voice_changed() to push
 * to AMY. Written from the Core-0 input tasks (encoder_task, button_task),
 * not only the UI task. */
extern fm_voice_t s_fm_voice;

/* Safe, audible default: DX7 algorithm 1 with only the OP2->OP1 pair (indices
 * 4, 5) at nonzero level, short operator envelopes. */
void fm_voice_default(fm_voice_t *v);

/* Full (re)configure of one synth slot as a 7-osc FM/ALGO voice. Call after
 * allocating/reallocating the synth (patch switch, layer creation). */
void fm_voice_configure_track(uint8_t synth_id, uint16_t num_voices,
                              const fm_voice_t *voice);

/* Live update of an already-configured FM voice without reallocating the osc
 * pool: routing + every operator (7 events per synth). Use after a topology
 * or algorithm change; for one operator's ratio/level/env use fm_voice_push()
 * with that operator's index. */
void fm_voice_push_live(uint8_t synth_id, const fm_voice_t *voice);

/* Push scope for the "voice changed" fan-out (sequencer_core_fm_voice_changed,
 * arp_core_fm_voice_changed): an operator index pushes that operator only,
 * FM_PUSH_ROUTING the osc-0 routing event only (algorithm/topology/feedback),
 * FM_PUSH_ALL everything. Keeps encoder-rate edits to one event per synth.
 * FM_PUSH_OP_RESET(op) resets that operator's osc before pushing it: AMY
 * cannot clear a set ratio by event, so a ratio -> fixed switch needs it
 * (fixed -> ratio does not; a set ratio overrides the osc's own pitch). */
#define FM_PUSH_ALL      0xF0
#define FM_PUSH_ROUTING  0xF1
#define FM_PUSH_OP_RESET(op)  ((uint8_t)(0xE0u + (op)))
void fm_voice_push(uint8_t synth_id, const fm_voice_t *voice, uint8_t what);

/* ── Routing queries / edits (no AMY traffic; the caller pushes) ────────── */

/* The routing the voice is playing, decoded from the table row in table mode
 * or copied from op_targets/fb_op in custom mode. */
void fm_voice_graph(const fm_voice_t *v, fm_graph_view_t *out);

/* Switch to custom mode seeded from the current routing: the row's decoded
 * target masks, fan-out included, and its feedback operator. No-op when
 * already custom. */
void fm_voice_make_custom(fm_voice_t *v);

/* Why fm_voice_set_op_targets() refused a change. */
typedef enum { FM_ROUTE_OK, FM_ROUTE_INVALID, FM_ROUTE_LOOP, FM_ROUTE_NO_BUS } fm_route_err_t;

/* Set operator op's target set to mask, entering custom mode if needed. The
 * change is kept only if the mask is shape-valid (fm_graph.h; else
 * FM_ROUTE_INVALID), the graph stays acyclic (else FM_ROUTE_LOOP) and it
 * compiles onto AMY's buses (else FM_ROUTE_NO_BUS). On failure *v is
 * unchanged, including its algorithm. *err gets the outcome (FM_ROUTE_OK on
 * success); err may be NULL. UI task only. */
bool fm_voice_set_op_targets(fm_voice_t *v, uint8_t op, uint8_t mask, fm_route_err_t *err);

/* Custom-mode feedback operator (FM_OP_NONE or an operator index), entering
 * custom mode if needed. Feedback never changes bus needs, so any valid value
 * is kept (false = out of range, voice unchanged). */
bool fm_voice_set_fb_op(fm_voice_t *v, uint8_t fb_op);

/* Step the algorithm through table rows 1..N-1 then FM_ALGO_CUSTOM (wrapping).
 * op_targets/fb_op are not touched: stepping onto custom plays the topology
 * last authored (or last seeded by a setter above), however many table rows
 * were visited in between. Returns the new `algorithm` value. */
uint8_t fm_voice_step_algorithm(fm_voice_t *v, int dir);

/* ── Value ranges and the edit grid (no AMY traffic; the caller pushes) ────
 * The range each value is held to and the step a control moves it in. Every
 * editor of a voice goes through these, so a value one of them shows is one
 * the others can reach. Any task; *v is the caller's to guard. */
#define FM_RATIO_MIN     0.25f
#define FM_RATIO_MAX     20.0f
#define FM_FIXED_HZ_MIN  1.0f
#define FM_FIXED_HZ_MAX  9772.0f
#define FM_A4_HZ         440.0f     /* reference note of the fine step and of a ratio <-> fixed switch */
#define FM_FINE_HZ       0.1f       /* fine step */
#define FM_LEVEL_STEP    0.05f      /* op_level and feedback step */
#define FM_FEEDBACK_MAX  1.2f
#define FM_EG_LEVEL_MAX  99
#define FM_EG_RATE_MAX   99

typedef enum {
    FM_FIELD_COARSE,    /* ratio mode: the next curated ratio (0.5 .. 16), keeping the
                         * offset from the nearest one as a factor; fixed mode: a semitone */
    FM_FIELD_FINE,      /* FM_FINE_HZ, measured at A4 in ratio mode */
    FM_FIELD_LEVEL,     /* FM_LEVEL_STEP, 0..1 */
    FM_FIELD_R1, FM_FIELD_R2, FM_FIELD_R3, FM_FIELD_R4,  /* 1, 0..FM_EG_RATE_MAX */
    FM_FIELD_L1, FM_FIELD_L2, FM_FIELD_L3, FM_FIELD_L4,  /* 1, 0..FM_EG_LEVEL_MAX */
    FM_FIELD_FEEDBACK,  /* FM_LEVEL_STEP, 0..FM_FEEDBACK_MAX; voice-level, op ignored */
    FM_FIELD_COUNT
} fm_field_t;

/* Move one field by `delta` steps, clamped to its range. An operator index
 * or field out of range leaves *v unchanged. */
void fm_voice_step(fm_voice_t *v, uint8_t op, fm_field_t field, int delta);

/* Switch operator op between ratio and fixed mode, seeding the new mode from
 * what the operator sounds at A4. Returns true when the operator left ratio
 * mode: its push then needs FM_PUSH_OP_RESET(op). op out of range: false,
 * *v unchanged. */
bool fm_voice_toggle_fixed(fm_voice_t *v, uint8_t op);

/* Force a voice from outside (a file, a host tool) into the ranges above;
 * values already in range are kept. A non-finite float takes the default
 * voice's value. An algorithm past the table becomes row 1. Custom fields
 * that are not shape-valid, acyclic and compilable are reseeded from the
 * table row (row 1 when the voice was on CUST), so the voice always pushes. */
void fm_voice_clamp(fm_voice_t *v);

#ifdef __cplusplus
}
#endif
