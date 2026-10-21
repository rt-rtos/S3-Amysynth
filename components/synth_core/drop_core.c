#include "drop_core.h"
#include "sequencer_core.h"
#include "sequencer.h"     /* sequencer_ticks (AMY) */
#include "amy_helpers.h"
#include "voice_config.h"
#include "lfo_bank.h"
#include "lfo_targets.h"
#include "seq_core_config.h"
#include "synth_slots.h"
#include "quantizer.h"
#include "seq_clamp.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "diag_heap.h"
#include <string.h>
#include <stdio.h>

#ifndef CONFIG_SEQ_DROP_DEFAULT_ENABLED
#define CONFIG_SEQ_DROP_DEFAULT_ENABLED 0
#endif
#ifndef CONFIG_SEQ_DROP_DEFAULT_PATCH
#define CONFIG_SEQ_DROP_DEFAULT_PATCH SEQ_PATCH_PULSE
#endif
#ifndef CONFIG_SEQ_DROP_SUB_DEFAULT_PATCH
#define CONFIG_SEQ_DROP_SUB_DEFAULT_PATCH SEQ_PATCH_SINE
#endif
#ifndef CONFIG_SEQ_DROP_DEFAULT_ROOT_NOTE
#define CONFIG_SEQ_DROP_DEFAULT_ROOT_NOTE 48
#endif

static const char *TAG = "drop_core";

_Static_assert(NOTE_DIV_PPQ == AMY_SEQUENCER_PPQ, "note_div ticks are AMY sequencer ticks");
_Static_assert(DROP_FIELD_W <= 128 && DROP_FIELD_H + 16 <= 64, "field fits under the header");

/* ── Physics constants (DROP_Q fixed point, pixels and ticks) ─────────────
 * Gravity makes a free fall over the whole field take one beat
 * (AMY_SEQUENCER_PPQ ticks) at any tempo: g = 2 h / t^2. The reference
 * speed for velocity is what that fall reaches. */
#define PX(n)            ((int32_t)(n) << DROP_Q)
#define DROP_GRAVITY     ((2 * PX(DROP_FIELD_H)) / (AMY_SEQUENCER_PPQ * AMY_SEQUENCER_PPQ))
#define DROP_REF_SPEED   (DROP_GRAVITY * AMY_SEQUENCER_PPQ)
#define DROP_DAMP_RESTITUTION 51    /* 0.2 in Q8 */
#define DROP_MIN_SPEED   (PX(1) / 4) /* below this after a bounce the ball ends */
#define DROP_SEG_HALF    PX(DROP_CELL_W / 2)   /* half a segment's length */
#define DROP_EPS         (PX(1) / 16)          /* push-off after a bounce */

/* Unit vectors of the eight angles, 22.5 degrees apart, in DROP_Q. */
static const int16_t s_dir[DROP_SEG_ANGLES][2] = {
    { 4096,    0 }, { 3784, 1567 }, { 2896, 2896 }, { 1567, 3784 },
    {    0, 4096 }, {-1567, 3784 }, {-2896, 2896 }, {-3784, 1567 },
};

void drop_seg_dir(uint8_t angle, int32_t *dx, int32_t *dy)
{
    if (angle >= DROP_SEG_ANGLES) angle = 0;
    if (dx) *dx = s_dir[angle][0];
    if (dy) *dy = s_dir[angle][1];
}

/* ── State ───────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t       patch;
    voice_params_t vp;
    uint16_t       portamento_ms;
    voice_unison_t unison;        /* count 0 = never set */
    uint8_t        gate_pct;
    bool           muted;
    bool           solo_muted;
} drop_voice_t;

typedef struct {
    bool       enabled;
    uint8_t    focus;             /* drop_which_t the screen addresses */
    note_div_t rate;              /* spawn interval */
    uint8_t    loop_bars;
    uint8_t    octaves;
    uint8_t    bounce;            /* PLAIN/SPLIT restitution, Q8 */
    uint32_t   seed;
    bool       seed_frozen;
    uint8_t    scale_index;
    uint8_t    root_note;
    uint8_t    sub_root_note;
    arp_quant_mode_t quant_mode;
    drop_seg_t segs[DROP_SEG_MAX];
    uint8_t    seg_count;
    uint8_t    jackpot[DROP_JACKPOT_BYTES];
    drop_voice_t voice[DROP_VOICE_COUNT];
} drop_state_t;

static drop_state_t s_drop;
static volatile bool s_drop_dirty = false;
/* Impact voice count (DEV menu, volatile). The sub slot is fixed at
 * SEQ_DROP_SUB_VOICES. */
static uint8_t s_drop_voices = SEQ_DROP_VOICES;
_Static_assert(SEQ_DROP_VOICES >= 1 && SEQ_DROP_VOICES <= DROP_VOICES_MAX,
               "boot voice count outside the settable range");
/* Bumped by every edit that changes the loop; the view sim restarts on a
 * change so it never continues from a field that is gone. */
static uint32_t s_drop_gen = 1;
/* Notes the last refresh emitted on each synth's tag pair. */
static uint16_t s_last_impacts = 0, s_last_jackpots = 0;

/* A synth's tag pair in the drop window: its note-offs, then its note-ons. */
static inline uint32_t drop_tag_pair(drop_which_t w)
{
    return sequencer_core_drop_tag_base() + (w == DROP_VOICE_SUB ? 2u : 0u);
}

static inline void drop_mark_dirty(void)
{
    s_drop_gen++;
    s_drop_dirty = true;
}

static inline uint8_t slot_of(drop_which_t w)
{
    return (w == DROP_VOICE_SUB) ? DROP_SUB_SYNTH : DROP_SYNTH;
}

static inline bool which_ok(drop_which_t w)
{
    return (unsigned)w < DROP_VOICE_COUNT;
}

static inline uint8_t voices_of(drop_which_t w)
{
    return (w == DROP_VOICE_SUB) ? SEQ_DROP_SUB_VOICES : s_drop_voices;
}

/* ── Simulation ──────────────────────────────────────────────────────────
 * Balls never meet, so each one is simulated alone: the spawner drops ball i
 * at tick i x interval of every pass of the loop, with its own PRNG seeded
 * from (seed, i), and a ball lives at most one loop. Its impacts and
 * crossings are folded modulo the loop length, so the loop plays the field
 * as it sounds after running forever: the balls of one pass land on into the
 * start of the next. */

typedef struct {
    int32_t  x, y, vx, vy;
    uint32_t rng;
    uint16_t age;          /* ticks since the spawn */
    bool     alive;
    bool     in_jackpot;
} drop_ball_t;

/* What one tick of a ball did. */
typedef struct {
    bool    impact, jackpot;
    uint8_t row;           /* impact: the segment's row */
    uint8_t seg;           /* impact: the segment's index in s_drop.segs */
    int32_t speed;         /* impact: normal speed */
    int32_t jp_speed;      /* crossing: vertical speed */
} drop_ball_ev_t;

typedef struct {
    uint16_t tick;         /* loop tick, folded */
    uint16_t gate;         /* ticks to the ball's next impact or end */
    uint16_t speed;        /* normal speed at impact, DROP_Q */
    uint8_t  row;
    uint8_t  seg;          /* index in s_drop.segs at the refresh */
} drop_impact_t;
_Static_assert(sizeof(drop_impact_t) == 8, "the segment index fills the record's padding");

typedef struct {
    uint16_t tick;
    uint16_t speed;
} drop_jp_hit_t;

/* What a loop run records, sorted by tick. Over a cap, the softest events
 * give way (cull_key); `soft_*` is the index of the softest kept once an
 * array is full. */
typedef struct {
    uint16_t      n_impacts;
    uint16_t      n_jackpots;
    uint16_t      soft_impact;
    uint16_t      soft_jackpot;
    drop_impact_t impacts[SEQ_DROP_IMPACT_MAX];
    drop_jp_hit_t jackpots[SEQ_DROP_JACKPOT_MAX];
} drop_rec_t;

/* Segment geometry the balls test against, derived from s_drop.segs by
 * geom_build(): cell centre, direction and normal in DROP_Q. */
typedef struct {
    int32_t cx, cy, dx, dy, nx, ny;
    uint8_t row, type, ratio;
} drop_seg_geom_t;

static drop_seg_geom_t s_geom[DROP_SEG_MAX];
static uint8_t         s_geom_count;
static bool            s_geom_jackpot;   /* any jackpot cell set */

/* The refresh's record, static: it is 1800 bytes, which the UI task's frame
 * cannot carry. */
static drop_rec_t s_rec_loop;

/* The view: up to DROP_BALL_MAX of the loop's balls, stepped on from one
 * call to the next. */
static struct {
    drop_ball_t balls[DROP_BALL_MAX];
    uint32_t    pos;       /* loop tick of the next step */
    uint32_t    gen;
} s_view;

static void geom_build(void)
{
    s_geom_count = s_drop.seg_count;
    for (uint8_t i = 0; i < s_geom_count; i++) {
        const drop_seg_t *g = &s_drop.segs[i];
        drop_seg_geom_t *o = &s_geom[i];
        o->cx = PX(g->col * DROP_CELL_W) + PX(DROP_CELL_W) / 2;
        o->cy = PX(g->row * DROP_CELL_H) + PX(DROP_CELL_H) / 2;
        o->dx = s_dir[g->angle][0];
        o->dy = s_dir[g->angle][1];
        o->nx = -o->dy;
        o->ny = o->dx;
        o->row = g->row;
        o->type = g->type;
        o->ratio = g->ratio;
    }
    s_geom_jackpot = drop_jackpot_count() != 0;
}

static inline uint32_t ball_rand(drop_ball_t *b)
{
    uint32_t x = b->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    b->rng = x;
    return x;
}

static inline uint32_t spawn_interval(void)
{
    return note_div_ticks(s_drop.rate);
}

/* Ball `idx` of the loop at the spawner, its PRNG seeded from the loop's
 * seed and its index (the murmur3 finalizer, so neighbouring indices draw
 * unrelated streams). */
static void ball_spawn(drop_ball_t *b, uint32_t idx)
{
    uint32_t h = s_drop.seed + idx * 0x9E3779B9u;
    h ^= h >> 16; h *= 0x85EBCA6Bu;
    h ^= h >> 13; h *= 0xC2B2AE35u;
    h ^= h >> 16;
    memset(b, 0, sizeof(*b));
    b->rng = h ? h : 0x9E3779B9u;
    int32_t half = PX(DROP_CELL_W) / 2;
    int32_t jitter = (int32_t)(ball_rand(b) % (uint32_t)(2 * half + 1)) - half;
    b->x = PX(DROP_SPAWN_COL * DROP_CELL_W) + PX(DROP_CELL_W) / 2 + jitter;
    b->alive = true;
}

/* Test the move p0 -> p1 against segment g. On a crossing within the
 * segment's extent: reflect the velocity, place the ball just off the line on
 * its incoming side, and return the normal speed before the bounce. */
static bool seg_bounce(const drop_seg_geom_t *g, drop_ball_t *b,
                       int32_t x1, int32_t y1, int32_t *speed_out)
{
    int32_t x0 = b->x, y0 = b->y;
    int32_t lo = (x0 < x1) ? x0 : x1, hi = (x0 < x1) ? x1 : x0;
    if (hi < g->cx - DROP_SEG_HALF - DROP_EPS || lo > g->cx + DROP_SEG_HALF + DROP_EPS) return false;
    lo = (y0 < y1) ? y0 : y1; hi = (y0 < y1) ? y1 : y0;
    if (hi < g->cy - DROP_SEG_HALF - DROP_EPS || lo > g->cy + DROP_SEG_HALF + DROP_EPS) return false;

    int64_t s0 = (int64_t)(x0 - g->cx) * g->nx + (int64_t)(y0 - g->cy) * g->ny;
    int64_t s1 = (int64_t)(x1 - g->cx) * g->nx + (int64_t)(y1 - g->cy) * g->ny;
    if (!((s0 > 0 && s1 <= 0) || (s0 < 0 && s1 >= 0))) return false;

    int64_t u  = (s0 << DROP_Q) / (s0 - s1);                 /* 0..1 in DROP_Q */
    int32_t qx = x0 + (int32_t)(((int64_t)(x1 - x0) * u) >> DROP_Q);
    int32_t qy = y0 + (int32_t)(((int64_t)(y1 - y0) * u) >> DROP_Q);
    int64_t t  = ((int64_t)(qx - g->cx) * g->dx + (int64_t)(qy - g->cy) * g->dy) >> DROP_Q;
    if (t < -DROP_SEG_HALF || t > DROP_SEG_HALF) return false;

    int32_t vn = (int32_t)(((int64_t)b->vx * g->nx + (int64_t)b->vy * g->ny) >> DROP_Q);
    int32_t speed = (vn < 0) ? -vn : vn;
    int32_t e = (g->type == DROP_SEG_DAMP) ? DROP_DAMP_RESTITUTION : (int32_t)s_drop.bounce;
    int32_t term = (int32_t)(((int64_t)(256 + e) * vn) >> 8);
    b->vx -= (int32_t)(((int64_t)term * g->nx) >> DROP_Q);
    b->vy -= (int32_t)(((int64_t)term * g->ny) >> DROP_Q);
    int32_t side = (s0 > 0) ? 1 : -1;
    b->x = qx + side * (g->nx / 16);
    b->y = qy + side * (g->ny / 16);

    if (g->type == DROP_SEG_SPLIT) {
        /* A peg: the ball leaves left or right at no less than a quarter of
         * the impact speed, the side drawn from the ball's PRNG. */
        int32_t kick = speed / 4;
        int32_t mag  = (b->vx < 0) ? -b->vx : b->vx;
        if (mag < kick) mag = kick;
        bool left = (ball_rand(b) % 100u) < g->ratio;
        b->vx = left ? -mag : mag;
    }
    *speed_out = speed;
    return true;
}

/* Advance a live ball one tick. It ends after a bounce too slow to go on,
 * on leaving the field, or once it has lived `life` ticks. */
static void ball_step(drop_ball_t *b, uint32_t life, drop_ball_ev_t *ev)
{
    ev->impact = false;
    ev->jackpot = false;
    b->vy += DROP_GRAVITY;
    int32_t x1 = b->x + b->vx, y1 = b->y + b->vy;
    bool hit = false;
    for (uint8_t k = 0; k < s_geom_count && !hit; k++) {
        int32_t speed = 0;
        if (!seg_bounce(&s_geom[k], b, x1, y1, &speed)) continue;
        hit = true;
        ev->impact = true;
        ev->row = s_geom[k].row;
        ev->seg = k;
        ev->speed = speed;
        int32_t ax = (b->vx < 0) ? -b->vx : b->vx;
        int32_t ay = (b->vy < 0) ? -b->vy : b->vy;
        if (ax < DROP_MIN_SPEED && ay < DROP_MIN_SPEED) b->alive = false;
    }
    if (!hit) {
        b->x = x1;
        b->y = y1;
    }
    b->age++;
    if (!b->alive) return;
    if (b->x < 0 || b->x >= PX(DROP_FIELD_W) || b->y >= PX(DROP_FIELD_H) ||
        b->y < -PX(DROP_FIELD_H) || b->age >= life) {
        b->alive = false;
        return;
    }
    if (s_geom_jackpot) {
        uint8_t col = (uint8_t)((b->x >> DROP_Q) / DROP_CELL_W);
        uint8_t row = (uint8_t)((b->y < 0 ? 0 : b->y) >> DROP_Q) / DROP_CELL_H;
        bool inside = drop_jackpot_at(col, row);
        if (inside && !b->in_jackpot) {
            ev->jackpot = true;
            ev->jp_speed = (b->vy < 0) ? -b->vy : b->vy;
        }
        b->in_jackpot = inside;
    }
}

static inline uint16_t clamp_u16(uint32_t v)
{
    return (v > 0xFFFFu) ? 0xFFFFu : (uint16_t)v;
}

/* The order the caps cull in: coarse speed (1/64 px per tick), ties broken
 * by a hash of tick and row, so a cap over many equal impacts (a field of
 * identical balls) keeps a spread over the whole loop. */
static uint32_t cull_key(uint16_t speed, uint16_t tick, uint8_t row)
{
    uint32_t h = ((uint32_t)tick * 31u + row) * 0x9E3779B9u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return ((uint32_t)(speed >> 6) << 16) | (h & 0xFFFFu);
}

static inline uint32_t impact_key(const drop_impact_t *im)
{
    return cull_key(im->speed, im->tick, im->row);
}

static inline uint32_t jackpot_key(const drop_jp_hit_t *jp)
{
    return cull_key(jp->speed, jp->tick, 0);
}

static uint16_t softest_impact(const drop_rec_t *r)
{
    uint16_t m = 0;
    for (uint16_t i = 1; i < r->n_impacts; i++) {
        if (impact_key(&r->impacts[i]) < impact_key(&r->impacts[m])) m = i;
    }
    return m;
}

static uint16_t softest_jackpot(const drop_rec_t *r)
{
    uint16_t m = 0;
    for (uint16_t i = 1; i < r->n_jackpots; i++) {
        if (jackpot_key(&r->jackpots[i]) < jackpot_key(&r->jackpots[m])) m = i;
    }
    return m;
}

/* Keep an impact, or drop the softest one kept when the record is full (the
 * new one when it is the softest). */
static void rec_impact(drop_rec_t *r, const drop_impact_t *im)
{
    if (r->n_impacts < SEQ_DROP_IMPACT_MAX) {
        r->impacts[r->n_impacts++] = *im;
        if (r->n_impacts == SEQ_DROP_IMPACT_MAX) r->soft_impact = softest_impact(r);
        return;
    }
    if (impact_key(im) <= impact_key(&r->impacts[r->soft_impact])) return;
    r->impacts[r->soft_impact] = *im;
    r->soft_impact = softest_impact(r);
}

static void rec_jackpot(drop_rec_t *r, uint16_t tick, uint16_t speed)
{
    drop_jp_hit_t jp = { .tick = tick, .speed = speed };
    if (r->n_jackpots < SEQ_DROP_JACKPOT_MAX) {
        r->jackpots[r->n_jackpots++] = jp;
        if (r->n_jackpots == SEQ_DROP_JACKPOT_MAX) r->soft_jackpot = softest_jackpot(r);
        return;
    }
    if (jackpot_key(&jp) <= jackpot_key(&r->jackpots[r->soft_jackpot])) return;
    r->jackpots[r->soft_jackpot] = jp;
    r->soft_jackpot = softest_jackpot(r);
}

/* Insertion sorts by tick, stable: the record is at most a few hundred
 * events and mostly in order already. */
static void rec_sort(drop_rec_t *r)
{
    for (uint16_t i = 1; i < r->n_impacts; i++) {
        drop_impact_t v = r->impacts[i];
        uint16_t j = i;
        while (j > 0 && r->impacts[j - 1].tick > v.tick) { r->impacts[j] = r->impacts[j - 1]; j--; }
        r->impacts[j] = v;
    }
    for (uint16_t i = 1; i < r->n_jackpots; i++) {
        drop_jp_hit_t v = r->jackpots[i];
        uint16_t j = i;
        while (j > 0 && r->jackpots[j - 1].tick > v.tick) { r->jackpots[j] = r->jackpots[j - 1]; j--; }
        r->jackpots[j] = v;
    }
}

/* Run every ball of the loop from the seed: impacts with their gates and
 * crossings, folded onto the loop and sorted by tick. */
static void sim_run_loop(drop_rec_t *rec)
{
    uint32_t loop = drop_loop_ticks();
    uint32_t iv   = spawn_interval();
    memset(rec, 0, sizeof(*rec));
    geom_build();
    for (uint32_t i = 0; i * iv < loop; i++) {
        uint32_t t0 = i * iv;
        drop_ball_t b;
        ball_spawn(&b, i);
        drop_impact_t im = { 0 };
        uint32_t im_at = 0;          /* unfolded tick of the pending impact */
        bool pending = false;
        while (b.alive) {
            uint32_t t = t0 + b.age;
            drop_ball_ev_t ev;
            ball_step(&b, loop, &ev);
            if (ev.impact) {
                if (pending) {
                    im.gate = clamp_u16(t - im_at);
                    rec_impact(rec, &im);
                }
                im.tick  = (uint16_t)(t % loop);
                im.speed = clamp_u16((uint32_t)ev.speed);
                im.row   = ev.row;
                im.seg   = ev.seg;
                im_at    = t;
                pending  = true;
            }
            if (ev.jackpot) rec_jackpot(rec, (uint16_t)(t % loop), clamp_u16((uint32_t)ev.jp_speed));
        }
        if (pending) {
            im.gate = clamp_u16(t0 + b.age - im_at);
            rec_impact(rec, &im);
        }
    }
    rec_sort(rec);
}

uint32_t drop_loop_ticks(void)
{
    return (uint32_t)s_drop.loop_bars * SEQ_TICKS_PER_BAR;
}

/* One view tick: spawn into a free slot (a full view leaves the ball
 * undrawn; it still plays), then step every ball. */
static void view_step(uint32_t loop)
{
    uint32_t iv = spawn_interval();
    if (s_view.pos % iv == 0) {
        for (uint8_t i = 0; i < DROP_BALL_MAX; i++) {
            if (s_view.balls[i].alive) continue;
            ball_spawn(&s_view.balls[i], s_view.pos / iv);
            break;
        }
    }
    for (uint8_t i = 0; i < DROP_BALL_MAX; i++) {
        drop_ball_t *b = &s_view.balls[i];
        if (!b->alive) continue;
        drop_ball_ev_t ev;
        ball_step(b, loop, &ev);
    }
    s_view.pos = (s_view.pos + 1u) % loop;
}

uint8_t drop_view_balls(uint32_t loop_tick, drop_ball_pos_t out[DROP_BALL_MAX])
{
    uint32_t loop = drop_loop_ticks();
    if (loop_tick >= loop) loop_tick = loop - 1;
    if (s_view.gen != s_drop_gen) {
        /* One pass from an empty field leaves it as it stands at the seam:
         * no ball lives longer than the loop. */
        memset(&s_view, 0, sizeof(s_view));
        s_view.gen = s_drop_gen;
        geom_build();
        for (uint32_t i = 0; i < loop; i++) view_step(loop);
    }
    /* Step forward to loop_tick; a playhead that went back wrapped. */
    uint32_t last = (s_view.pos + loop - 1u) % loop;
    for (uint32_t n = (loop_tick + loop - last) % loop; n > 0; n--) view_step(loop);
    uint8_t n = 0;
    for (uint8_t i = 0; i < DROP_BALL_MAX; i++) {
        const drop_ball_t *b = &s_view.balls[i];
        if (!b->alive) continue;
        int32_t x = b->x >> DROP_Q, y = b->y >> DROP_Q;
        if (x < 0 || x >= DROP_FIELD_W || y < 0 || y >= DROP_FIELD_H) continue;
        out[n].x = (uint8_t)x;
        out[n].y = (uint8_t)y;
        n++;
    }
    return n;
}

uint16_t drop_last_impact_count(void)  { return s_last_impacts; }
uint16_t drop_last_jackpot_count(void) { return s_last_jackpots; }

/* ── Pitch ───────────────────────────────────────────────────────────── */

/* The root and scale the rows walk, by the arp's three sources
 * (arp_snap): OWN is the drop's own; CHORD the progression chord, in the
 * drop root's octave, falling through to GLOBAL with no chord applied; GLOBAL
 * the global quantizer, chromatic while it is disabled. */
static void drop_scale_source(uint8_t *root, const musical_scale_t **scale)
{
    uint8_t r = s_drop.root_note;
    uint8_t scale_idx = s_drop.scale_index;
    if (s_drop.quant_mode != ARP_QUANT_OWN) {
        uint8_t pc;
        if (s_drop.quant_mode == ARP_QUANT_CHORD &&
            sequencer_core_progression_arp_chord(&pc, &scale_idx)) {
            int32_t base = (int32_t)r - (int32_t)(r % 12u) + (int32_t)pc;
            if (base > (int32_t)r + 6) base -= 12;
            r = (uint8_t)SEQ_CLAMP_INT(base, 0, 127);
        } else if (!sequencer_core_get_quantizer_enabled()) {
            scale_idx = 0;   /* chromatic: one semitone per row */
        } else {
            uint8_t qr = sequencer_core_get_quantizer_root_note();
            int32_t base = (int32_t)r - (int32_t)(r % 12u) + (int32_t)(qr % 12u);
            if (base > (int32_t)r + 6) base -= 12;
            r = (uint8_t)SEQ_CLAMP_INT(base, 0, 127);
            scale_idx = sequencer_core_get_quantizer_scale();
        }
    }
    *root  = r;
    *scale = quantizer_get_scale(scale_idx);
}

/* The pitch of a segment row: the bottom row plays the root, each row up is
 * `octaves` scale degrees higher. */
static uint8_t drop_snap(uint8_t row)
{
    uint8_t root;
    const musical_scale_t *scale;
    drop_scale_source(&root, &scale);
    uint8_t size = (scale && scale->size) ? scale->size : 12u;
    uint32_t degree = (uint32_t)(DROP_ROWS - 1 - row) * s_drop.octaves;
    int32_t note = (int32_t)root + 12 * (int32_t)(degree / size);
    note += scale ? (int32_t)scale->intervals[degree % size] : (int32_t)(degree % size);
    return sequencer_core_clamp_melodic_note(note);
}

/* ── Emit ────────────────────────────────────────────────────────────── */

void drop_core_clear_all(void)
{
    sequencer_core_drop_clear_note(drop_tag_pair(DROP_VOICE_MAIN));
    sequencer_core_drop_clear_note(drop_tag_pair(DROP_VOICE_SUB));
    s_last_impacts  = 0;
    s_last_jackpots = 0;
}

/* All-notes-off on one slot. Must follow every clear that can run while
 * notes sound: the clear deleted the note-offs that would silence them. */
static void drop_kill_voices(uint8_t synth)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth    = synth;
    e->velocity = 0.0f;
    amy_helpers_event_send(e);
}

static void drop_kill_all(void)
{
    drop_kill_voices(DROP_SYNTH);
    drop_kill_voices(DROP_SUB_SYNTH);
}

static float drop_velocity(uint16_t speed, float amp_trim)
{
    float ratio = (float)speed / (float)DROP_REF_SPEED;
    if (ratio > 1.0f) ratio = 1.0f;
    float v = ratio * 0.9f * amp_trim;
    return (v > 1.0f) ? 1.0f : v;
}

/* An impact's pitch: its row's note plus the segment's offset. An index past
 * the field counts as no offset: the dump reads the record after segments may
 * have been removed. */
static uint8_t drop_impact_note(const drop_impact_t *im)
{
    int32_t ofs = (im->seg < s_drop.seg_count) ? s_drop.segs[im->seg].pitch_ofs : 0;
    return sequencer_core_clamp_melodic_note((int32_t)drop_snap(im->row) + ofs);
}

/* An impact's velocity: the speed's plus the segment's adjustment. 0 is a
 * silent hit that still plays its note pair, as on a sequencer step. */
static float drop_impact_velocity(const drop_impact_t *im, float amp_trim)
{
    int32_t adj = (im->seg < s_drop.seg_count) ? s_drop.segs[im->seg].vel_adj : 0;
    float v = drop_velocity(im->speed, amp_trim) + (float)adj * 0.01f;
    return SEQ_CLAMP_F32(v, 0.0f, 1.0f);
}

/* The gate of impact i, shortened to end before the next impact at the same
 * pitch (the loop wrapping around counts), clamped to 1..period-1. */
static uint32_t gate_before_repeat(uint32_t gate, uint32_t tick, uint16_t i,
                                   const drop_rec_t *r, const uint8_t *notes,
                                   uint32_t period)
{
    uint32_t next = period + tick;   /* itself, one loop later */
    for (uint16_t j = 0; j < r->n_impacts; j++) {
        if (j == i || notes[j] != notes[i]) continue;
        uint32_t t = r->impacts[j].tick;
        if (t <= tick) t += period;
        if (t < next) next = t;
    }
    if (gate + 1u > next - tick) gate = next - tick - 1u;
    if (gate < 1) gate = 1;
    if (gate > period - 1) gate = period - 1;
    return gate;
}

void drop_core_refresh(void)
{
    drop_core_clear_all();
    drop_kill_all();

    if (!s_drop.enabled || !sequencer_core_get_playing()) return;
    if (s_drop.seg_count == 0 && drop_jackpot_count() == 0) return;
    const drop_voice_t *vm = &s_drop.voice[DROP_VOICE_MAIN];
    const drop_voice_t *vs = &s_drop.voice[DROP_VOICE_SUB];
    bool main_on = !vm->muted && !vm->solo_muted;
    bool sub_on  = !vs->muted && !vs->solo_muted;
    if (!main_on && !sub_on) return;

    drop_rec_t *s = &s_rec_loop;
    sim_run_loop(s);

    uint32_t period = drop_loop_ticks();
    uint32_t base   = drop_tag_pair(DROP_VOICE_MAIN);
    uint32_t sub_base = drop_tag_pair(DROP_VOICE_SUB);
    float    trim   = s_drop.voice[DROP_VOICE_MAIN].vp.amp_trim;
    uint8_t  notes[SEQ_DROP_IMPACT_MAX];
    for (uint16_t i = 0; i < s->n_impacts; i++) notes[i] = drop_impact_note(&s->impacts[i]);
    for (uint16_t i = 0; main_on && i < s->n_impacts; i++) {
        const drop_impact_t *im = &s->impacts[i];
        /* Two balls landing on one pitch on one tick play one note. */
        bool twin = false;
        for (uint16_t k = i; k > 0 && s->impacts[k - 1].tick == im->tick && !twin; k--) {
            twin = notes[k - 1] == notes[i];
        }
        if (twin) continue;
        uint32_t gate = ((uint32_t)im->gate * vm->gate_pct) / 100u;
        /* AMY's instrument keeps one voice per sounding pitch, so a note
         * must end before the next impact at the same pitch, in this loop
         * or the next. */
        gate = gate_before_repeat(gate, im->tick, i, s, notes, period);
        sequencer_core_drop_emit_note(DROP_SYNTH, base,
                                      notes[i], drop_impact_velocity(im, trim),
                                      1u + im->tick, gate, period);
    }
    float sub_trim = s_drop.voice[DROP_VOICE_SUB].vp.amp_trim;
    uint8_t sub_note = sequencer_core_clamp_melodic_note((int32_t)s_drop.sub_root_note);
    for (uint16_t j = 0; sub_on && j < s->n_jackpots; j++) {
        const drop_jp_hit_t *jp = &s->jackpots[j];
        uint32_t next = (j + 1u < s->n_jackpots) ? s->jackpots[j + 1].tick
                                                 : period + s->jackpots[0].tick;
        if (next == jp->tick) continue;   /* the crossing on the same tick plays */
        uint32_t gate = ((uint32_t)spawn_interval() * vs->gate_pct) / 100u;
        if (gate + 1u > next - jp->tick) gate = next - jp->tick - 1u;
        if (gate < 1) gate = 1;
        if (gate > period - 1) gate = period - 1;
        sequencer_core_drop_emit_note(DROP_SUB_SYNTH, sub_base,
                                      sub_note, drop_velocity(jp->speed, sub_trim),
                                      1u + jp->tick, gate, period);
    }
    s_last_impacts  = main_on ? s->n_impacts : 0;
    s_last_jackpots = sub_on ? s->n_jackpots : 0;
}

unsigned drop_core_dump(void)
{
    unsigned n = 0;
    printf("DROP en=%u mute=%u/%u rate=%u bars=%u oct=%u bounce=%u gate=%u/%u seed=%08X%s root=%u"
           " sub=%u q=%u patch=%u/%u segs=%u jpcells=%u loop=%u\n",
           (unsigned)s_drop.enabled, (unsigned)s_drop.voice[0].muted,
           (unsigned)s_drop.voice[1].muted, (unsigned)s_drop.rate,
           (unsigned)s_drop.loop_bars, (unsigned)s_drop.octaves, (unsigned)s_drop.bounce,
           (unsigned)s_drop.voice[0].gate_pct, (unsigned)s_drop.voice[1].gate_pct,
           (unsigned)s_drop.seed, s_drop.seed_frozen ? "*" : "",
           (unsigned)s_drop.root_note, (unsigned)s_drop.sub_root_note,
           (unsigned)s_drop.quant_mode, (unsigned)s_drop.voice[0].patch,
           (unsigned)s_drop.voice[1].patch, (unsigned)s_drop.seg_count,
           (unsigned)drop_jackpot_count(), (unsigned)drop_loop_ticks());
    n++;
    for (uint8_t r = 0; r < DROP_ROWS; r++) {
        for (uint8_t c = 0; c < DROP_COLS; c++) {
            if (drop_jackpot_at(c, r)) { printf("  JPC c=%u r=%u\n", c, r); n++; }
        }
    }
    for (uint8_t i = 0; i < s_drop.seg_count; i++) {
        const drop_seg_t *g = &s_drop.segs[i];
        printf("  SEG%u c=%u r=%u a=%u t=%u ratio=%u pofs=%d vadj=%d\n", i, g->col, g->row,
               g->angle, g->type, g->ratio, (int)g->pitch_ofs, (int)g->vel_adj);
        n++;
    }
    const drop_rec_t *s = &s_rec_loop;
    float trim = s_drop.voice[DROP_VOICE_MAIN].vp.amp_trim;
    for (uint16_t i = 0; i < s_last_impacts; i++) {
        const drop_impact_t *im = &s->impacts[i];
        printf("  IMP%u tick=%u row=%u note=%u vel=%.2f air=%u\n", i, im->tick, im->row,
               (unsigned)drop_impact_note(im), (double)drop_impact_velocity(im, trim),
               (unsigned)im->gate);
        n++;
    }
    for (uint16_t j = 0; j < s_last_jackpots; j++) {
        printf("  JP%u tick=%u vel=%.2f\n", j, s->jackpots[j].tick,
               (double)drop_velocity(s->jackpots[j].speed,
                                     s_drop.voice[DROP_VOICE_SUB].vp.amp_trim));
        n++;
    }
    return n;
}

void drop_core_service(void)
{
    if (!s_drop_dirty) return;
    s_drop_dirty = false;
    drop_core_refresh();
}

void drop_core_mark_dirty(void)
{
    s_drop_dirty = true;
}

/* ── Voice setup (one slot at a time; mirrors the arp's) ─────────────── */

static bool drop_unison_stereo(drop_which_t w)
{
    voice_unison_t u;
    drop_get_unison(w, &u);
    return u.stereo != 0u;
}

static uint8_t drop_unison_copies(drop_which_t w)
{
    const drop_voice_t *v = &s_drop.voice[w];
    return sequencer_core_unison_copies_for(v->patch,
                                            v->unison.bypass ? 1u : v->unison.count,
                                            voices_of(w), drop_unison_stereo(w));
}

static voice_unison_t drop_unison_eff(drop_which_t w)
{
    voice_unison_t eff;
    drop_get_unison(w, &eff);
    eff.count = drop_unison_copies(w);
    return eff;
}

static bool drop_voice_layout(drop_which_t w, seq_voice_layout_t *vl)
{
    return sequencer_core_voice_layout(s_drop.voice[w].patch, drop_unison_copies(w),
                                       drop_unison_stereo(w), vl);
}

static void drop_push_dist(drop_which_t w, const seq_dist_t *d)
{
    uint8_t synth = slot_of(w);
    seq_voice_layout_t vl;
    uint8_t heads = drop_voice_layout(w, &vl) ? vl.heads_mask : 0u;
    if (!heads) {
        voice_apply_dist(synth, d);
        return;
    }
    for (uint8_t o = 0; (uint8_t)(heads >> o) != 0u; o++)
        if (heads & (uint8_t)(1u << o))
            voice_apply_dist_osc(synth, o, d);
}

static void drop_apply_filter(drop_which_t w, const seq_filter_t *f)
{
    const drop_voice_t *v = &s_drop.voice[w];
    uint8_t synth = slot_of(w);
    amy_event *e = amy_helpers_event_begin();
    e->synth = synth;
    if (f->enabled) {
        e->filter_type = f->filter_type;
        e->filter_freq_coefs[COEF_CONST] = sequencer_core_filter_const_hz(f);
        e->resonance = f->resonance;
    } else {
        e->filter_type = FILTER_NONE;
    }
    if (v->patch == SEQ_PATCH_KS) {
        if (f->feedback > 0.0f) e->feedback = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
        e->duty_coefs[COEF_CONST] = 0.5f + f->ks_duty_ofs;
    }
    amy_helpers_event_send(e);

    sequencer_core_push_eg_depths(synth, sequencer_core_patch_voice_osc(v->patch), f,
                                  v->vp.filter_authored);
    if (sequencer_core_is_wavetable_patch(v->patch)) {
        seq_voice_layout_t vl;
        uint8_t mask = drop_voice_layout(w, &vl) ? vl.pitch_mask : 0x01u;
        sequencer_core_push_eg_scan(synth, mask, f, v->vp.filter_authored);
    }
    if (seq_filter_eg1_live(f)) {
        sequencer_core_push_envelope_eg1(synth, 0, &v->vp.env1);
    }
}

#if CONFIG_SEQ_LFO_SHARED
static uint8_t s_lfo_ref[DROP_VOICE_COUNT]  = { LFO_BANK_NONE, LFO_BANK_NONE };
static uint8_t s_lfo2_ref[DROP_VOICE_COUNT] = { LFO_BANK_NONE, LFO_BANK_NONE };

static bool drop_lfo_target_map(drop_which_t w, lfo_target_map_t *map)
{
    seq_voice_layout_t vl;
    if (drop_voice_layout(w, &vl)) {
        *map = (lfo_target_map_t){ .voice_mask = vl.voice_mask,
                                   .pitch_mask = vl.pitch_mask,
                                   .scan_mask  = vl.pitch_mask };
        return true;
    }
    return lfo_targets_of_patch(s_drop.voice[w].patch, map);
}
#endif

/* `lfo` (NULL = none) on a shared LFO synth over the patch's target map; an
 * unmapped patch, a full bank or no shared bank leaves the voice without
 * one (the drop has no software stepper). */
static void drop_apply_lfo(drop_which_t w, const seq_lfo_t *lfo)
{
#if CONFIG_SEQ_LFO_SHARED
    lfo_target_map_t map;
    if (drop_lfo_target_map(w, &map))
        (void)lfo_bank_bind(slot_of(w), VOICE_LFO_SLOT_LFO1, &map, lfo,
                            sequencer_core_get_bpm(), &s_lfo_ref[w]);
    else
        lfo_bank_unbind(&s_lfo_ref[w]);
#else
    (void)w; (void)lfo;
#endif
}

/* `lfo` (NULL = unwire) on the slot's mod slot 0 where the patch has a target
 * map; drop LFO2's reference otherwise. */
static void drop_wire_lfo2(drop_which_t w, const seq_lfo_t *lfo)
{
#if CONFIG_SEQ_LFO_SHARED
    lfo_target_map_t map;
    if (drop_lfo_target_map(w, &map))
        (void)lfo_bank_bind(slot_of(w), VOICE_LFO_SLOT_LFO2, &map, lfo,
                            sequencer_core_get_bpm(), &s_lfo2_ref[w]);
    else
        lfo_bank_unbind(&s_lfo2_ref[w]);
#else
    (void)w; (void)lfo;
#endif
}

/* LFO2 by the slot-0 rule (arp_apply_lfo2): wired while open, else no send
 * so a Juno/DX7 patch keeps its own LFO. */
static void drop_apply_lfo2(drop_which_t w)
{
    if (s_drop.voice[w].vp.lfo2_authored) {
        drop_wire_lfo2(w, &s_drop.voice[w].vp.lfo2);
        return;
    }
#if CONFIG_SEQ_LFO_SHARED
    lfo_bank_unbind(&s_lfo2_ref[w]);
#endif
}

static void drop_apply_lfos(drop_which_t w)
{
    const drop_voice_t *v = &s_drop.voice[w];
    bool lfo_on = v->vp.lfo_authored && v->vp.lfo.enabled;
    drop_apply_lfo(w, lfo_on ? &v->vp.lfo : NULL);
    drop_apply_lfo2(w);
}

static void drop_push_portamento(drop_which_t w)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth         = slot_of(w);
    e->portamento_ms = s_drop.voice[w].portamento_ms;
    amy_helpers_event_send(e);
}

/* (Re)build one slot for its patch and params, then re-impose the authored
 * voice block; mirrors arp_rebuild(). The loop is cleared first and re-emitted
 * on the next service frame. */
static void drop_rebuild(drop_which_t w)
{
    drop_core_clear_all();
    drop_kill_all();

    drop_voice_t *v = &s_drop.voice[w];
    uint8_t synth = slot_of(w);
    voice_unison_t eff = drop_unison_eff(w);
    sequencer_core_drop_configure(synth, v->patch, voices_of(w),
                                  v->vp.filter_authored, v->vp.filter.feedback,
                                  v->vp.filter.ks_duty_ofs,
                                  (eff.count > 1u) ? &eff : NULL);
    if (sequencer_core_is_wave_patch(v->patch) || v->vp.env_authored) {
        sequencer_core_push_envelope_osc(synth, sequencer_core_patch_voice_osc(v->patch),
                                         &v->vp.env);
    }
    drop_apply_lfos(w);
    if (v->vp.filter_authored) drop_apply_filter(w, &v->vp.filter);
    if (v->vp.env1_authored) sequencer_core_push_envelope_eg1(synth, 0, &v->vp.env1);
    if (v->vp.dist_authored) drop_push_dist(w, &v->vp.dist);
    drop_push_portamento(w);

    drop_core_mark_dirty();
}

/* ── Public API ──────────────────────────────────────────────────────── */

static void drop_voice_defaults(drop_voice_params_t *v, uint16_t patch)
{
    memset(v, 0, sizeof(*v));
    voice_params_init_defaults(&v->vp);
    v->patch = patch;
    v->gate_pct = 75;
    v->unison.stereo = 1u;   /* as seq_layer_defaults */
    v->unison.shift  = 100u;
    /* As the arp's defaults: an envelope with a short decay and a slower
     * EG1 tail, both unauthored until an editor commits them. */
    v->vp.env.attack_ms   = 4;
    v->vp.env.decay_ms    = 250;
    v->vp.env.sustain_pct = 30;
    v->vp.env.release_ms  = 200;
    v->vp.env1.attack_ms   = 15;
    v->vp.env1.decay_ms    = 400;
    v->vp.env1.sustain_pct = 20;
    v->vp.env1.release_ms  = 300;
    v->vp.filter.cutoff_hz = 800.0f;
    v->vp.filter.resonance = 1.0f;
    v->vp.lfo.wave    = LFO_WAVE_SINE;
    v->vp.lfo.rate    = NOTE_DIV_1_1;
    v->vp.lfo.depth   = 50;
    v->vp.lfo.targets = LFO_TGT_BIT(LFO_TARGET_FILTER);
}

void drop_params_defaults(drop_params_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled     = false;
    out->rate        = NOTE_DIV_1_4;
    out->loop_bars   = 2;
    out->octaves     = 1;
    out->seed        = 0x1D0B5EEDu;
    out->scale       = 0;
    out->root        = 48;
    out->sub_root    = 36;   /* an octave below root */
    out->quant_mode  = ARP_QUANT_CHORD;
    out->bounce      = DROP_BOUNCE_DEFAULT;
    /* The boot field: a plain floor under the spawner, so every ball lands
     * and bounces out its ritardando on the root, and a jackpot cell on the
     * way down, which the first fall and the first bounce cross. */
    out->segs[0] = (drop_seg_t){ .col = DROP_SPAWN_COL, .row = DROP_ROWS - 1,
                                 .angle = 0, .type = DROP_SEG_PLAIN, .ratio = 50 };
    out->seg_count = 1;
    {
        unsigned bit = 3u * DROP_COLS + DROP_SPAWN_COL;
        out->jackpot[bit >> 3] = (uint8_t)(1u << (bit & 7u));
    }
    drop_voice_defaults(&out->voice[DROP_VOICE_MAIN], SEQ_PATCH_PULSE);
    drop_voice_defaults(&out->voice[DROP_VOICE_SUB], SEQ_PATCH_SINE);
}

void drop_core_init(void)
{
    drop_params_t p;
    drop_params_defaults(&p);
    /* Kconfig overlay: the boot drop. */
    p.enabled  = CONFIG_SEQ_DROP_DEFAULT_ENABLED;
    p.root     = SEQ_CLAMP_U8(CONFIG_SEQ_DROP_DEFAULT_ROOT_NOTE, 0, 127);
    p.sub_root = (p.root >= 12u) ? (uint8_t)(p.root - 12u) : p.root;
    p.voice[DROP_VOICE_MAIN].patch = SEQ_CLAMP_U16(CONFIG_SEQ_DROP_DEFAULT_PATCH, 0, SEQ_PATCH_FULL_MAX);
    p.voice[DROP_VOICE_SUB].patch  = SEQ_CLAMP_U16(CONFIG_SEQ_DROP_SUB_DEFAULT_PATCH, 0, SEQ_PATCH_FULL_MAX);

    memset(&s_drop, 0, sizeof(s_drop));
    s_drop.enabled       = p.enabled;
    s_drop.rate          = p.rate;
    s_drop.loop_bars     = p.loop_bars;
    s_drop.octaves       = p.octaves;
    s_drop.bounce        = p.bounce;
    s_drop.seed          = p.seed;
    s_drop.seed_frozen   = p.seed_frozen;
    s_drop.scale_index   = p.scale;
    s_drop.root_note     = p.root;
    s_drop.sub_root_note = p.sub_root;
    s_drop.quant_mode    = (arp_quant_mode_t)p.quant_mode;
    memcpy(s_drop.segs, p.segs, sizeof(s_drop.segs));
    s_drop.seg_count     = p.seg_count;
    memcpy(s_drop.jackpot, p.jackpot, sizeof(s_drop.jackpot));
    for (uint8_t i = 0; i < DROP_VOICE_COUNT; i++) {
        drop_voice_t *v = &s_drop.voice[i];
        const drop_voice_params_t *pv = &p.voice[i];
        v->patch         = pv->patch;
        v->vp            = pv->vp;
        v->portamento_ms = pv->portamento_ms;
        v->unison        = pv->unison;
        v->gate_pct      = pv->gate_pct;
        v->muted         = pv->muted;
    }

    DIAG_HEAP_CHECK("drop_init: before configure");
    drop_rebuild(DROP_VOICE_MAIN);
    drop_rebuild(DROP_VOICE_SUB);
    DIAG_HEAP_CHECK("drop_init: after configure");
    drop_mark_dirty();
    ESP_LOGI(TAG, "drop_core initialized (synths %u, %u)", DROP_SYNTH, DROP_SUB_SYNTH);
}

void drop_set_enabled(bool enabled)
{
    if (s_drop.enabled == enabled) return;
    s_drop.enabled = enabled;
    drop_mark_dirty();
    ESP_LOGI(TAG, "drop %s", enabled ? "enabled" : "disabled");
}
bool drop_get_enabled(void) { return s_drop.enabled; }

void drop_set_muted(drop_which_t w, bool muted)
{
    if (!which_ok(w) || s_drop.voice[w].muted == muted) return;
    s_drop.voice[w].muted = muted;
    if (muted) drop_core_refresh();
    else       drop_mark_dirty();
}
bool drop_get_muted(drop_which_t w) { return which_ok(w) && s_drop.voice[w].muted; }

void drop_set_solo_muted(drop_which_t w, bool muted)
{
    if (!which_ok(w) || s_drop.voice[w].solo_muted == muted) return;
    s_drop.voice[w].solo_muted = muted;
    drop_core_refresh();
}

void drop_set_rate(note_div_t rate)
{
    if (rate >= NOTE_DIV_STD_COUNT) return;
    if (s_drop.rate == rate) return;
    s_drop.rate = rate;
    drop_mark_dirty();
}
note_div_t drop_get_rate(void) { return s_drop.rate; }

void drop_set_loop_bars(uint8_t bars)
{
    bars = SEQ_CLAMP_U8(bars, 1, DROP_LOOP_BARS_MAX);
    if (s_drop.loop_bars == bars) return;
    s_drop.loop_bars = bars;
    drop_mark_dirty();
}
uint8_t drop_get_loop_bars(void) { return s_drop.loop_bars; }

void drop_set_octaves(uint8_t octaves)
{
    octaves = SEQ_CLAMP_U8(octaves, 1, DROP_OCT_MAX);
    if (s_drop.octaves == octaves) return;
    s_drop.octaves = octaves;
    drop_mark_dirty();
}
uint8_t drop_get_octaves(void) { return s_drop.octaves; }

void drop_set_bounce(uint8_t q8)
{
    q8 = SEQ_CLAMP_U8(q8, DROP_BOUNCE_MIN, DROP_BOUNCE_MAX);
    if (s_drop.bounce == q8) return;
    s_drop.bounce = q8;
    drop_mark_dirty();
}
uint8_t drop_get_bounce(void) { return s_drop.bounce; }

void drop_set_gate_pct(drop_which_t w, uint8_t gate_pct)
{
    if (!which_ok(w)) return;
    gate_pct = SEQ_CLAMP_U8(gate_pct, 10, 100);
    if (s_drop.voice[w].gate_pct == gate_pct) return;
    s_drop.voice[w].gate_pct = gate_pct;
    drop_mark_dirty();
}
uint8_t drop_get_gate_pct(drop_which_t w) { return which_ok(w) ? s_drop.voice[w].gate_pct : 75; }

void drop_set_seed(uint32_t seed)
{
    if (s_drop.seed == seed) return;
    s_drop.seed = seed;
    drop_mark_dirty();
}
uint32_t drop_get_seed(void) { return s_drop.seed; }

void drop_reroll_seed(void)
{
    if (s_drop.seed_frozen) return;
    /* The next seed from the current one: a reroll chain is itself a
     * replayable sequence. */
    uint32_t x = s_drop.seed ? s_drop.seed : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    x += (uint32_t)sequencer_ticks() * 0x9E3779B9u;
    if (x == 0) x = 1;
    drop_set_seed(x);
}

void drop_set_seed_frozen(bool frozen) { s_drop.seed_frozen = frozen; }
bool drop_get_seed_frozen(void)        { return s_drop.seed_frozen; }

void drop_set_scale(uint8_t scale_index)
{
    if (scale_index >= quantizer_scale_count()) scale_index = 0;
    if (s_drop.scale_index == scale_index) return;
    s_drop.scale_index = scale_index;
    drop_mark_dirty();
}
uint8_t drop_get_scale(void) { return s_drop.scale_index; }

void drop_set_root_note(uint8_t root_note)
{
    root_note = SEQ_CLAMP_U8(root_note, 0, 127);
    if (s_drop.root_note == root_note) return;
    s_drop.root_note = root_note;
    drop_mark_dirty();
}
uint8_t drop_get_root_note(void) { return s_drop.root_note; }

void drop_set_sub_root_note(uint8_t root_note)
{
    root_note = SEQ_CLAMP_U8(root_note, 0, 127);
    if (s_drop.sub_root_note == root_note) return;
    s_drop.sub_root_note = root_note;
    drop_mark_dirty();
}
uint8_t drop_get_sub_root_note(void) { return s_drop.sub_root_note; }

void drop_set_quant_mode(arp_quant_mode_t mode)
{
    if ((unsigned)mode >= ARP_QUANT_COUNT) mode = ARP_QUANT_CHORD;
    if (s_drop.quant_mode == mode) return;
    s_drop.quant_mode = mode;
    drop_mark_dirty();
}
arp_quant_mode_t drop_get_quant_mode(void) { return s_drop.quant_mode; }

/* ── Field ── */

uint8_t drop_get_seg_count(void) { return s_drop.seg_count; }

bool drop_get_seg(uint8_t idx, drop_seg_t *out)
{
    if (idx >= s_drop.seg_count || !out) return false;
    *out = s_drop.segs[idx];
    return true;
}

int drop_find_seg(uint8_t col, uint8_t row)
{
    for (uint8_t i = 0; i < s_drop.seg_count; i++) {
        if (s_drop.segs[i].col == col && s_drop.segs[i].row == row) return (int)i;
    }
    return -1;
}

bool drop_set_seg(const drop_seg_t *seg)
{
    if (!seg || seg->col >= DROP_COLS || seg->row >= DROP_ROWS) return false;
    drop_seg_t v = *seg;
    v.angle = SEQ_CLAMP_U8(v.angle, 0, DROP_SEG_ANGLES - 1);
    v.type  = SEQ_CLAMP_U8(v.type, 0, DROP_SEG_TYPE_COUNT - 1);
    v.ratio = SEQ_CLAMP_U8(v.ratio, 0, 100);
    v.pitch_ofs = (int8_t)SEQ_CLAMP_INT(v.pitch_ofs, -SEQ_STEP_PITCH_OFS_MAX, SEQ_STEP_PITCH_OFS_MAX);
    v.vel_adj   = (int8_t)SEQ_CLAMP_INT(v.vel_adj, -SEQ_STEP_VEL_ADJ_MAX, SEQ_STEP_VEL_ADJ_MAX);
    int idx = drop_find_seg(v.col, v.row);
    if (idx < 0) {
        if (s_drop.seg_count >= DROP_SEG_MAX) return false;
        idx = s_drop.seg_count++;
    } else if (memcmp(&s_drop.segs[idx], &v, sizeof(v)) == 0) {
        return true;
    }
    s_drop.segs[idx] = v;
    drop_mark_dirty();
    return true;
}

bool drop_remove_seg(uint8_t col, uint8_t row)
{
    int idx = drop_find_seg(col, row);
    if (idx < 0) return false;
    uint8_t tail = (uint8_t)(s_drop.seg_count - idx - 1);
    if (tail) memmove(&s_drop.segs[idx], &s_drop.segs[idx + 1], tail * sizeof(drop_seg_t));
    s_drop.seg_count--;
    memset(&s_drop.segs[s_drop.seg_count], 0, sizeof(drop_seg_t));
    drop_mark_dirty();
    return true;
}

bool drop_jackpot_at(uint8_t col, uint8_t row)
{
    if (col >= DROP_COLS || row >= DROP_ROWS) return false;
    unsigned bit = (unsigned)row * DROP_COLS + col;
    return (s_drop.jackpot[bit >> 3] >> (bit & 7u)) & 1u;
}

void drop_jackpot_set(uint8_t col, uint8_t row, bool on)
{
    if (col >= DROP_COLS || row >= DROP_ROWS) return;
    if (drop_jackpot_at(col, row) == on) return;
    unsigned bit = (unsigned)row * DROP_COLS + col;
    if (on) s_drop.jackpot[bit >> 3] |=  (uint8_t)(1u << (bit & 7u));
    else    s_drop.jackpot[bit >> 3] &= (uint8_t)~(1u << (bit & 7u));
    drop_mark_dirty();
}

uint8_t drop_jackpot_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < DROP_JACKPOT_BYTES; i++) {
        uint8_t b = s_drop.jackpot[i];
        while (b) { n++; b &= (uint8_t)(b - 1u); }
    }
    return n;
}

void drop_jackpot_get_map(uint8_t out[DROP_JACKPOT_BYTES])
{
    if (out) memcpy(out, s_drop.jackpot, DROP_JACKPOT_BYTES);
}

void drop_jackpot_set_map(const uint8_t map[DROP_JACKPOT_BYTES])
{
    if (!map || memcmp(s_drop.jackpot, map, DROP_JACKPOT_BYTES) == 0) return;
    memcpy(s_drop.jackpot, map, DROP_JACKPOT_BYTES);
    drop_mark_dirty();
}

/* ── Per-synth voice ── */

void drop_set_patch(drop_which_t w, uint16_t patch)
{
    if (!which_ok(w)) return;
    patch = SEQ_CLAMP_U16(patch, 0, SEQ_PATCH_FULL_MAX);
    if (s_drop.voice[w].patch == patch) return;
    s_drop.voice[w].patch = patch;
    drop_rebuild(w);
}

uint16_t drop_get_patch(drop_which_t w)
{
    return which_ok(w) ? s_drop.voice[w].patch : 0u;
}

void drop_set_voices(uint8_t n)
{
    n = SEQ_CLAMP_U8(n, 1u, DROP_VOICES_MAX);
    if (n == s_drop_voices) return;
    s_drop_voices = n;
    drop_rebuild(DROP_VOICE_MAIN);
}

uint8_t drop_get_voices(void)
{
    return s_drop_voices;
}

void drop_set_portamento_ms(drop_which_t w, uint16_t ms)
{
    if (!which_ok(w)) return;
    ms = SEQ_CLAMP_U16(ms, 0, DROP_PORTAMENTO_MAX_MS);
    if (s_drop.voice[w].portamento_ms == ms) return;
    s_drop.voice[w].portamento_ms = ms;
    drop_push_portamento(w);
}

uint16_t drop_get_portamento_ms(drop_which_t w)
{
    return which_ok(w) ? s_drop.voice[w].portamento_ms : 0u;
}

void drop_set_amp_scale(drop_which_t w, float v)
{
    if (!which_ok(w)) return;
    v = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
    s_drop.voice[w].vp.amp_trim = v;
    drop_mark_dirty();
}

float drop_get_amp_scale(drop_which_t w)
{
    return which_ok(w) ? s_drop.voice[w].vp.amp_trim : 1.0f;
}

void drop_get_vp(drop_which_t w, voice_params_t *out)
{
    if (!out) return;
    if (which_ok(w)) *out = s_drop.voice[w].vp;
    else             voice_params_init_defaults(out);
}

void drop_get_unison(drop_which_t w, voice_unison_t *out)
{
    if (!out) return;
    voice_unison_t u = voice_unison_defaults();
    if (which_ok(w) && s_drop.voice[w].unison.count != 0u) u = s_drop.voice[w].unison;
    *out = u;
}

uint8_t drop_unison_max(drop_which_t w)
{
    if (!which_ok(w)) return 0u;
    return sequencer_core_unison_max_for(s_drop.voice[w].patch,
                                         voices_of(w), drop_unison_stereo(w));
}

static voice_unison_t drop_unison_clamp(const voice_unison_t *u)
{
    voice_unison_t v = *u;
    v.count        = SEQ_CLAMP_U8(v.count, 0u, VOICE_UNISON_MAX_COPIES);
    v.detune_cents = SEQ_CLAMP_U8(v.detune_cents, 0u, VOICE_UNISON_MAX_DETUNE);
    v.spread_pct   = SEQ_CLAMP_U8(v.spread_pct, 0u, 100u);
    v.blend_pct    = SEQ_CLAMP_U8(v.blend_pct, 0u, 100u);
    v.stereo       = SEQ_CLAMP_U8(v.stereo, 0u, 1u);
    v.bypass       = SEQ_CLAMP_U8(v.bypass, 0u, 1u);
    v.shift        = SEQ_CLAMP_U8(v.shift, 0u, VOICE_UNISON_MAX_SHIFT);
    v.stack        = SEQ_CLAMP_U8(v.stack, 0u, VOICE_UNISON_MAX_STACK);
    v.phase_random = SEQ_CLAMP_U8(v.phase_random, 0u, 1u);
    return v;
}

void drop_set_unison(drop_which_t w, const voice_unison_t *u)
{
    if (!which_ok(w) || !u) return;
    uint8_t was        = drop_unison_copies(w);
    bool    was_stereo = drop_unison_stereo(w);
    s_drop.voice[w].unison = drop_unison_clamp(u);
    uint8_t now = drop_unison_copies(w);
    if (now != was || (now > 1u && drop_unison_stereo(w) != was_stereo)) {
        drop_rebuild(w);
    } else if (now > 1u) {
        voice_unison_t eff = drop_unison_eff(w);
        voice_push_unison_live(slot_of(w), &eff, 1.0f);
    }
}

/* ── The shared editors, per synth (contracts in drop_core.h) ── */

static void drop_env_clamp(seq_env_t *e)
{
    e->attack_ms  = SEQ_CLAMP_U32(e->attack_ms, VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    e->release_ms = SEQ_CLAMP_U32(e->release_ms, VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
}

void drop_get_envelope(drop_which_t w, seq_env_t *out)
{
    if (out && which_ok(w)) *out = s_drop.voice[w].vp.env;
}

void drop_set_envelope(drop_which_t w, const seq_env_t *env)
{
    if (!which_ok(w) || !env) return;
    drop_voice_t *v = &s_drop.voice[w];
    v->vp.env = *env;
    drop_env_clamp(&v->vp.env);
    v->vp.env_authored = true;
    sequencer_core_push_envelope_osc(slot_of(w), sequencer_core_patch_voice_osc(v->patch),
                                     &v->vp.env);
}

void drop_preview_envelope(drop_which_t w, const seq_env_t *env)
{
    if (!which_ok(w) || !env) return;
    sequencer_core_push_envelope_osc(slot_of(w),
                                     sequencer_core_patch_voice_osc(s_drop.voice[w].patch), env);
}

void drop_get_envelope2(drop_which_t w, seq_env_t *out)
{
    if (out && which_ok(w)) *out = s_drop.voice[w].vp.env1;
}

void drop_set_envelope2(drop_which_t w, const seq_env_t *env)
{
    if (!which_ok(w) || !env) return;
    drop_voice_t *v = &s_drop.voice[w];
    v->vp.env1 = *env;
    drop_env_clamp(&v->vp.env1);
    v->vp.env1_authored = true;
    sequencer_core_push_envelope_eg1(slot_of(w), 0, &v->vp.env1);
}

void drop_preview_envelope2(drop_which_t w, const seq_env_t *env)
{
    if (!which_ok(w) || !env) return;
    sequencer_core_push_envelope_eg1(slot_of(w), 0, env);
}

void drop_get_filter(drop_which_t w, seq_filter_t *out)
{
    if (out && which_ok(w)) *out = s_drop.voice[w].vp.filter;
}

void drop_set_filter(drop_which_t w, const seq_filter_t *f)
{
    if (!which_ok(w) || !f) return;
    s_drop.voice[w].vp.filter = *f;
    s_drop.voice[w].vp.filter_authored = true;
    drop_apply_filter(w, &s_drop.voice[w].vp.filter);
}

void drop_preview_filter(drop_which_t w, const seq_filter_t *f)
{
    if (!which_ok(w) || !f) return;
    drop_apply_filter(w, f);
}

void drop_get_dist(drop_which_t w, seq_dist_t *out)
{
    if (out && which_ok(w)) *out = s_drop.voice[w].vp.dist;
}

void drop_set_dist(drop_which_t w, const seq_dist_t *d)
{
    if (!which_ok(w) || !d) return;
    s_drop.voice[w].vp.dist = *d;
    voice_dist_clamp(&s_drop.voice[w].vp.dist);
    s_drop.voice[w].vp.dist_authored = true;
    drop_push_dist(w, &s_drop.voice[w].vp.dist);
}

void drop_preview_dist(drop_which_t w, const seq_dist_t *d)
{
    if (!which_ok(w) || !d) return;
    drop_push_dist(w, d);
}

void drop_reapply_dist(drop_which_t w)
{
    if (!which_ok(w)) return;
    if (s_drop.voice[w].vp.dist_authored) drop_push_dist(w, &s_drop.voice[w].vp.dist);
}

void drop_get_lfo(drop_which_t w, seq_lfo_t *out)
{
    if (out && which_ok(w)) *out = s_drop.voice[w].vp.lfo;
}

void drop_set_lfo(drop_which_t w, const seq_lfo_t *lfo)
{
    if (!which_ok(w) || !lfo) return;
    drop_voice_t *v = &s_drop.voice[w];
    v->vp.lfo = *lfo;
    v->vp.lfo_authored = true;
    /* One LFO per target (seq_model.h): LFO1 keeps a shared one. */
    uint8_t shared = v->vp.lfo2.targets & v->vp.lfo.targets;
    v->vp.lfo2.targets &= (uint8_t)~shared;
    if (shared) drop_apply_lfo2(w);
    drop_apply_lfo(w, lfo->enabled ? &v->vp.lfo : NULL);
}

bool drop_lfo_bound(drop_which_t w)
{
#if CONFIG_SEQ_LFO_SHARED
    return which_ok(w) && s_lfo_ref[w] != LFO_BANK_NONE;
#else
    (void)w;
    return false;
#endif
}

bool drop_get_lfo2(drop_which_t w, seq_lfo_t *out)
{
    if (!which_ok(w)) return false;
    if (out) *out = s_drop.voice[w].vp.lfo2;
    return s_drop.voice[w].vp.lfo2_authored;
}

void drop_set_lfo2(drop_which_t w, const seq_lfo_t *lfo, bool open)
{
    if (!which_ok(w) || !lfo) return;
    drop_voice_t *v = &s_drop.voice[w];
    bool collapse = v->vp.lfo2_authored && !open;
    v->vp.lfo2 = *lfo;
    v->vp.lfo2.targets &= (uint8_t)~v->vp.lfo.targets;
    if (collapse && lfo_targets_patch_owns_slot0(v->patch)) {
        /* Only a patch re-apply brings the patch's own LFO back. */
        v->vp.lfo2_authored = false;
        drop_rebuild(w);
        return;
    }
    if (collapse) drop_wire_lfo2(w, NULL);
    v->vp.lfo2_authored = open;
    drop_apply_lfo2(w);
}

bool drop_lfo2_bound(drop_which_t w)
{
#if CONFIG_SEQ_LFO_SHARED
    return which_ok(w) && s_lfo2_ref[w] != LFO_BANK_NONE;
#else
    (void)w;
    return false;
#endif
}

void drop_set_voice(drop_which_t w, uint16_t patch, uint16_t portamento_ms,
                    const voice_params_t *vp, const voice_unison_t *uni)
{
    if (!which_ok(w) || !vp || !uni) return;
    drop_voice_t *v = &s_drop.voice[w];
    v->patch = SEQ_CLAMP_U16(patch, 0, SEQ_PATCH_FULL_MAX);
    v->portamento_ms = SEQ_CLAMP_U16(portamento_ms, 0, DROP_PORTAMENTO_MAX_MS);
    v->vp = *vp;
    v->vp.env.attack_ms   = SEQ_CLAMP_U32(v->vp.env.attack_ms,
                                          VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    v->vp.env.release_ms  = SEQ_CLAMP_U32(v->vp.env.release_ms,
                                          VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    v->vp.env1.attack_ms  = SEQ_CLAMP_U32(v->vp.env1.attack_ms,
                                          VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    v->vp.env1.release_ms = SEQ_CLAMP_U32(v->vp.env1.release_ms,
                                          VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    v->vp.amp_trim = SEQ_CLAMP_F32(v->vp.amp_trim, 0.0f, 1.0f);
    v->vp.lfo2.targets &= (uint8_t)~v->vp.lfo.targets;   /* one LFO per target */
    voice_dist_clamp(&v->vp.dist);
    v->unison = drop_unison_clamp(uni);
    drop_rebuild(w);
}

drop_which_t drop_get_focus(void) { return (drop_which_t)s_drop.focus; }

void drop_set_focus(drop_which_t w)
{
    if (which_ok(w)) s_drop.focus = (uint8_t)w;
}
