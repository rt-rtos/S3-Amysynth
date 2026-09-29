#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "seq_model.h"               /* seq_env_t */
#include "custompatches/fm_graph.h"  /* fm_graph_view_t, FM_TO_OUT, FM_OP_NONE */

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
 *                                aliases row 1); op_to/fb_op are ignored.
 *   algorithm == FM_ALGO_CUSTOM: the authored forest op_to[] + fb_op,
 *                                compiled onto AMY's two buses at push time.
 * `feedback` is the amount on whichever operator the routing marks FB.
 *
 * Per-operator "level" is that operator's amp_coefs[COEF_CONST]: for a
 * modulator it IS the modulation index/brightness, for a carrier it scales
 * that carrier. Level 0 silences the operator. Each operator has its own EG0
 * (op_env); the ALGO control osc keeps the row's per-track ADSR as a VCA over
 * the carriers, so both shape the note. */

#define FM_NUM_OPS      6
#define FM_ALGO_CUSTOM  0xFF   /* algorithm: use op_to[]/fb_op */

typedef struct {
    uint8_t   algorithm;
    uint8_t   fb_op;                 /* custom mode: FM_OP_NONE or 0..5    */
    uint8_t   op_to[FM_NUM_OPS];     /* custom mode: FM_TO_OUT or 0..5     */
    float     op_ratio[FM_NUM_OPS];  /* per-operator frequency ratio       */
    float     op_level[FM_NUM_OPS];  /* per-operator output level, 0..1    */
    seq_env_t op_env[FM_NUM_OPS];    /* per-operator EG0                   */
    float     feedback;              /* 0..~1.2                            */
} fm_voice_t;

/* The single live-editable "custom" FM voice (SEQ_PATCH_FM_CUSTOM), owned by
 * this module. The FM UI screen mutates it (through the setters below for
 * anything topological) then calls sequencer_core_fm_voice_changed() /
 * sequencer_core_fm_voice_op_changed() to push to AMY. UI task only. */
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
 * FM_PUSH_ALL everything. Keeps encoder-rate edits to one event per synth. */
#define FM_PUSH_ALL      0xF0
#define FM_PUSH_ROUTING  0xF1
void fm_voice_push(uint8_t synth_id, const fm_voice_t *voice, uint8_t what);

/* ── Routing queries / edits (no AMY traffic; the caller pushes) ────────── */

/* The routing the voice is playing, decoded from the table row in table mode
 * or built from op_to/fb_op in custom mode. */
void fm_voice_graph(const fm_voice_t *v, fm_graph_view_t *out);

/* Switch to custom mode seeded from the current routing (fan-out rows keep
 * each operator's first target). No-op when already custom. */
void fm_voice_make_custom(fm_voice_t *v);

/* Custom-mode edits: enter custom mode if needed, apply, and keep the change
 * only if the result compiles onto AMY's buses (false = rejected, voice
 * unchanged). target is FM_TO_OUT or an operator index. */
bool fm_voice_set_op_target(fm_voice_t *v, uint8_t op, uint8_t target);
bool fm_voice_set_fb_op(fm_voice_t *v, uint8_t fb_op);

/* Step the algorithm through table rows 1..N-1 then FM_ALGO_CUSTOM (wrapping).
 * Entering custom seeds op_to/fb_op from the row being left. Returns the new
 * `algorithm` value. */
uint8_t fm_voice_step_algorithm(fm_voice_t *v, int dir);

#ifdef __cplusplus
}
#endif
