#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── FM operator graph <-> AMY operator program ──────────────────────────
 * Pure functions (no AMY, no RTOS): host-testable. An operator program is
 * AMY's per-slot routing byte array (algorithms.c FmOperatorFlags); a graph
 * is who-modulates-whom over the 6 operators.
 *
 * Operator indexing: fm_voice.h. */

#define FM_GRAPH_OPS   6
#define FM_TO_OUT      0xFF   /* op_to[] value: carrier (final output)       */
#define FM_OP_NONE     0xFF   /* fb_op: no feedback operator                 */
#define FM_OUT_BIT     0x40   /* out_mask[] bit: routes to the final output  */

/* Display-side graph: one target bitmask per operator (bit t = modulates op
 * t, FM_OUT_BIT = carrier). Multi-bit masks come from decoded table rows
 * (fan-out); authored graphs are forests (one bit each). */
typedef struct {
    uint8_t out_mask[FM_GRAPH_OPS];
    uint8_t fb_op;              /* FM_OP_NONE or the self-feedback operator */
} fm_graph_view_t;

/* Compiled program: ops[s] is the routing byte rendered at slot s and
 * slot_op[s] the operator it renders (algo_source[s] = slot_op[s] + 1). */
typedef struct {
    uint8_t ops[FM_GRAPH_OPS];
    uint8_t slot_op[FM_GRAPH_OPS];
} fm_program_t;

/* Walk a 6-byte table row and recover its routing. Robust to any bytes:
 * unknown bus reads see whatever was last written there. */
void fm_graph_decode(const uint8_t ops[FM_GRAPH_OPS], fm_graph_view_t *out);

/* Forest view of an authored topology (each op one target). */
void fm_graph_from_forest(const uint8_t op_to[FM_GRAPH_OPS], uint8_t fb_op,
                          fm_graph_view_t *out);

/* Reduce a (possibly fan-out) view to a forest: each op keeps its lowest
 * target bit; unreachable/cyclic ops become carriers. */
void fm_graph_to_forest(const fm_graph_view_t *g, uint8_t op_to[FM_GRAPH_OPS]);

/* True when making `op` modulate `target` (FM_TO_OUT allowed) keeps the
 * forest acyclic, i.e. `target` is not `op` or one of its modulators. */
bool fm_graph_edge_allowed(const uint8_t op_to[FM_GRAPH_OPS], uint8_t op,
                           uint8_t target);

/* Compile a forest onto AMY's two modulation buses. Returns false when the
 * graph has a cycle or needs more than two live buses at some point; *out is
 * then unspecified. Operators are emitted carriers-in-index-order, subtree
 * before parent, so every modulator renders before what it modulates. */
bool fm_graph_compile(const uint8_t op_to[FM_GRAPH_OPS], uint8_t fb_op,
                      fm_program_t *out);

#ifdef __cplusplus
}
#endif
