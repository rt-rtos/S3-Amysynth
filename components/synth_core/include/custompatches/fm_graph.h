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
#define FM_TO_OUT      0xFF   /* TO row candidate token: OUT (make a carrier) */
#define FM_OP_NONE     0xFF   /* fb_op: no feedback operator                 */
#define FM_OUT_BIT     0x40   /* out_mask[] bit: routes to the final output  */

/* Search-node cap of fm_graph_compile(); reaching it fails the compile. */
#define FM_GRAPH_COMPILE_BUDGET 4096u

/* One target bitmask per operator (bit t = modulates op t, FM_OUT_BIT =
 * carrier). An operator's output goes to exactly one place in AMY, so a
 * shape-valid mask is exactly FM_OUT_BIT, or a nonzero subset of 0x3F without
 * the operator's own bit (fan-out: every target gets the same signal). */
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

/* True when no operator reaches itself through its targets. Precondition:
 * every mask shape-valid. A graph without a carrier is always cyclic. */
bool fm_graph_is_acyclic(const uint8_t targets[FM_GRAPH_OPS]);

/* Compile target masks onto AMY's two modulation buses. Depth-first search
 * over render orders (lowest operator index first) and bus choices, so the
 * result is deterministic for a given input and every modulator renders
 * before what it modulates. Returns false when a mask is not shape-valid,
 * the graph has a cycle, no order fits two buses, or the search reaches
 * FM_GRAPH_COMPILE_BUDGET nodes; *out is then unspecified. On success all
 * six operators are emitted.
 *
 * Besides the byte forms the DX7 table uses, the search may emit 0x26 (read
 * BUS_TWO, add to BUS_TWO): render_algo does not zero BUS_TWO for an adding
 * write, and every FM kernel reads mod[i] before writing buf[i], so the bus
 * ends up holding the operator's input plus its output. That is what compiles
 * the shape A -> B, A -> C, B -> C. The table never uses it. Refused forms:
 * 0x15 (render_algo takes the bus-one scratch path and never zeroes it) and
 * 0x22 (the bus is zeroed before the operator reads it). */
bool fm_graph_compile(const uint8_t targets[FM_GRAPH_OPS], uint8_t fb_op,
                      fm_program_t *out);

#ifdef FM_GRAPH_STATS
/* Host check only: search nodes used by the last fm_graph_compile(). */
extern uint32_t fm_graph_stats_nodes;
#endif

#ifdef __cplusplus
}
#endif
