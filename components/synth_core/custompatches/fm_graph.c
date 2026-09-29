#include "custompatches/fm_graph.h"
#include <string.h>

/* AMY algorithms.c FmOperatorFlags (mirrored: the enum is file-private
 * upstream; amy.h documents the values next to amy_set_custom_algorithm). */
#define OUT_BUS_ONE 0x01
#define OUT_BUS_TWO 0x02
#define OUT_BUS_ADD 0x04
#define IN_BUS_ONE  0x10
#define IN_BUS_TWO  0x20
#define FB_IN       0x40
#define FB_OUT      0x80

enum { BUS_NONE = 0, BUS_ONE = 1, BUS_TWO = 2, BUS_OUT = 3 };

void fm_graph_decode(const uint8_t ops[FM_GRAPH_OPS], fm_graph_view_t *out)
{
    uint8_t writers[3] = { 0, 0, 0 };   /* bitmask of ops currently on bus 1/2 */
    memset(out, 0, sizeof(*out));
    out->fb_op = FM_OP_NONE;
    for (uint8_t s = 0; s < FM_GRAPH_OPS; s++) {
        uint8_t b = ops[s];
        uint8_t in = (b & IN_BUS_ONE) ? writers[BUS_ONE]
                   : (b & IN_BUS_TWO) ? writers[BUS_TWO] : 0;
        for (uint8_t w = 0; w < FM_GRAPH_OPS; w++) {
            if (in & (1u << w)) out->out_mask[w] |= (uint8_t)(1u << s);
        }
        if (b & FB_IN) out->fb_op = s;
        uint8_t bus = (b & OUT_BUS_ONE) ? BUS_ONE : (b & OUT_BUS_TWO) ? BUS_TWO : BUS_NONE;
        if (bus == BUS_NONE) {
            out->out_mask[s] |= FM_OUT_BIT;
        } else if (b & OUT_BUS_ADD) {
            writers[bus] |= (uint8_t)(1u << s);
        } else {
            writers[bus] = (uint8_t)(1u << s);
        }
    }
}

#define ALL_OPS ((uint8_t)((1u << FM_GRAPH_OPS) - 1u))

#ifdef FM_GRAPH_STATS
uint32_t fm_graph_stats_nodes;
#endif

static bool mask_valid(uint8_t op, uint8_t m)
{
    if (m == FM_OUT_BIT) return true;
    return m != 0u && (m & (uint8_t)~ALL_OPS) == 0u && (m & (1u << op)) == 0u;
}

/* in[t] = the operators whose mask has bit t, i.e. t's modulators. */
static void graph_inputs(const uint8_t targets[FM_GRAPH_OPS], uint8_t in[FM_GRAPH_OPS])
{
    for (uint8_t t = 0; t < FM_GRAPH_OPS; t++) {
        in[t] = 0;
        for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
            if (targets[i] & (1u << t)) in[t] |= (uint8_t)(1u << i);
        }
    }
}

bool fm_graph_is_acyclic(const uint8_t targets[FM_GRAPH_OPS])
{
    uint8_t in[FM_GRAPH_OPS], placed = 0;
    graph_inputs(targets, in);
    /* Place every op whose modulators are all placed; ops on a cycle never
     * become ready. Six sweeps cover the longest chain. */
    for (uint8_t pass = 0; pass < FM_GRAPH_OPS; pass++) {
        for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
            if (!(placed & (1u << i)) && (in[i] & (uint8_t)~placed) == 0u) {
                placed |= (uint8_t)(1u << i);
            }
        }
    }
    return placed == ALL_OPS;
}

/* ── compiler ─────────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *targets;
    uint8_t in[FM_GRAPH_OPS];
    uint8_t fb_op;
    fm_program_t *prog;
    uint32_t nodes;
} compile_ctx_t;

/* Output choices of a modulator, in search order. */
static const struct { uint8_t bus; bool add; } s_mod_outs[] = {
    { BUS_ONE, false }, { BUS_TWO, false }, { BUS_ONE, true }, { BUS_TWO, true },
};

/* Bus contents only grow by adding newly placed ops or get overwritten, so an
 * unplaced op whose modulators are partly placed needs a bus holding exactly
 * that placed part right now, or it can never read its input. */
static bool inputs_reachable(const compile_ctx_t *c, uint8_t placed, uint8_t c1, uint8_t c2)
{
    for (uint8_t t = 0; t < FM_GRAPH_OPS; t++) {
        if (placed & (1u << t)) continue;
        uint8_t s = (uint8_t)(c->in[t] & placed);
        if (s != 0u && s != c1 && s != c2) return false;
    }
    return true;
}

/* State: `placed` ops rendered in slots 0..n-1, c1/c2 the ops summed on
 * BUS_ONE/BUS_TWO. 0 means stale data from the last voice (render_algo never
 * clears the buses between calls), so it is neither read nor added onto. */
static bool search(compile_ctx_t *c, uint8_t placed, uint8_t c1, uint8_t c2, uint8_t n)
{
    if (++c->nodes > FM_GRAPH_COMPILE_BUDGET) return false;
    if (placed == ALL_OPS) return true;
    for (uint8_t x = 0; x < FM_GRAPH_OPS; x++) {
        uint8_t bit = (uint8_t)(1u << x);
        uint8_t in = c->in[x];
        if ((placed & bit) || (in & (uint8_t)~placed)) continue;

        uint8_t ins[2], n_in = 0;
        if (in == 0u) {
            ins[n_in++] = BUS_NONE;
        } else {
            if (c1 == in) ins[n_in++] = BUS_ONE;
            if (c2 == in) ins[n_in++] = BUS_TWO;
        }
        bool carrier = (c->targets[x] == FM_OUT_BIT);
        uint8_t n_out = carrier ? 1u : (uint8_t)(sizeof(s_mod_outs) / sizeof(s_mod_outs[0]));

        for (uint8_t a = 0; a < n_in; a++) {
            for (uint8_t o = 0; o < n_out; o++) {
                uint8_t ob  = carrier ? BUS_OUT : s_mod_outs[o].bus;
                bool    add = carrier ? true    : s_mod_outs[o].add;
                uint8_t n1 = c1, n2 = c2;
                if (ob == BUS_ONE) {
                    if (add && (c1 == 0u || ins[a] == BUS_ONE)) continue;   /* stale, 0x15 */
                    n1 = add ? (uint8_t)(c1 | bit) : bit;
                } else if (ob == BUS_TWO) {
                    if (add ? c2 == 0u : ins[a] == BUS_TWO) continue;       /* stale, 0x22 */
                    n2 = add ? (uint8_t)(c2 | bit) : bit;
                }
                uint8_t np = (uint8_t)(placed | bit);
                if (!inputs_reachable(c, np, n1, n2)) continue;

                uint8_t byte = 0;
                if (ins[a] == BUS_ONE) byte |= IN_BUS_ONE;
                if (ins[a] == BUS_TWO) byte |= IN_BUS_TWO;
                if (ob == BUS_OUT) {
                    byte |= OUT_BUS_ADD;             /* carriers always sum into buf */
                } else {
                    byte |= (ob == BUS_ONE) ? OUT_BUS_ONE : OUT_BUS_TWO;
                    if (add) byte |= OUT_BUS_ADD;
                }
                if (x == c->fb_op) byte |= FB_IN | FB_OUT;
                c->prog->ops[n]     = byte;
                c->prog->slot_op[n] = x;
                if (search(c, np, n1, n2, (uint8_t)(n + 1u))) return true;
                if (c->nodes > FM_GRAPH_COMPILE_BUDGET) return false;
            }
        }
    }
    return false;
}

bool fm_graph_compile(const uint8_t targets[FM_GRAPH_OPS], uint8_t fb_op, fm_program_t *out)
{
    compile_ctx_t c = { .targets = targets, .fb_op = fb_op, .prog = out, .nodes = 0 };
    memset(out, 0, sizeof(*out));
    bool ok = true;
    for (uint8_t i = 0; i < FM_GRAPH_OPS; i++) {
        if (!mask_valid(i, targets[i])) ok = false;
    }
    if (ok) ok = fm_graph_is_acyclic(targets);
    if (ok) {
        graph_inputs(targets, c.in);
        ok = search(&c, 0, 0, 0, 0);
    }
#ifdef FM_GRAPH_STATS
    fm_graph_stats_nodes = c.nodes;
#endif
    return ok;
}
