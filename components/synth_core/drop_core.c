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
#define CONFIG_SEQ_DROP_DEFAULT_PATCH 138
#endif
#ifndef CONFIG_SEQ_DROP_SUB_DEFAULT_PATCH
#define CONFIG_SEQ_DROP_SUB_DEFAULT_PATCH 142
#endif
#ifndef CONFIG_SEQ_DROP_DEFAULT_ROOT_NOTE
#define CONFIG_SEQ_DROP_DEFAULT_ROOT_NOTE 48
#endif

static const char *TAG = "drop_core";

_Static_assert(NOTE_DIV_PPQ == AMY_SEQUENCER_PPQ, "note_div ticks are AMY sequencer ticks");
_Static_assert(DROP_FIELD_W <= 128 && DROP_FIELD_H + 8 <= 64, "field fits the panel");

/* ── Physics constants (DROP_Q fixed point, pixels and ticks) ─────────────
 * Gravity makes a free fall over the whole field take one beat
 * (AMY_SEQUENCER_PPQ ticks) at any tempo: g = 2 h / t^2. The reference
 * speed for velocity is what that fall reaches. */
#define PX(n)            ((int32_t)(n) << DROP_Q)
#define DROP_GRAVITY     ((2 * PX(DROP_FIELD_H)) / (AMY_SEQUENCER_PPQ * AMY_SEQUENCER_PPQ))
#define DROP_REF_SPEED   (DROP_GRAVITY * AMY_SEQUENCER_PPQ)
#define DROP_RESTITUTION      179   /* 0.7 in Q8 */
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
} drop_voice_t;

typedef struct {
    bool       enabled;
    bool       muted;
    bool       solo_muted;
    uint8_t    focus;             /* drop_which_t the screen addresses */
    note_div_t rate;              /* spawn interval */
    uint8_t    loop_bars;
    uint8_t    octaves;
    uint8_t    gate_pct;
    uint32_t   seed;
    bool       seed_frozen;
    uint8_t    scale_index;
    uint8_t    root_note;
    uint8_t    sub_root_note;
    arp_quant_mode_t quant_mode;
    drop_seg_t segs[DROP_SEG_MAX];
    uint8_t    seg_count;
    drop_jackpot_t jackpot;
    drop_voice_t voice[DROP_VOICE_COUNT];
} drop_state_t;

static drop_state_t s_drop;
static volatile bool s_drop_dirty = false;
/* Bumped by every edit that changes the loop; the view sim restarts on a
 * change so it never continues from a field that is gone. */
static uint32_t s_drop_gen = 1;
static uint8_t s_last_impacts = 0, s_last_jackpots = 0;

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

/* ── Simulation ──────────────────────────────────────────────────────── */

typedef struct {
    int32_t x, y, vx, vy;
    bool    alive;
    bool    in_jackpot;
    int8_t  last_impact;   /* index into impacts awaiting its gate, or -1 */
} drop_ball_t;

typedef struct {
    uint16_t tick;
    uint16_t gate;         /* ticks to the ball's next impact or death */
    uint16_t speed;        /* normal speed at impact, DROP_Q */
    uint8_t  row;
} drop_impact_t;

typedef struct {
    uint16_t tick;
    uint16_t speed;
} drop_jp_hit_t;

typedef struct {
    uint32_t rng;
    uint32_t tick;
    uint8_t  n_impacts;
    uint8_t  n_jackpots;
    drop_ball_t   balls[DROP_BALL_MAX];
    drop_impact_t impacts[SEQ_DROP_IMPACT_MAX];
    drop_jp_hit_t jackpots[SEQ_DROP_JACKPOT_MAX];
} drop_sim_t;

/* Segment geometry the sim tests against, derived from s_drop.segs by
 * sim_init(): cell centre, direction and normal in DROP_Q. */
typedef struct {
    int32_t cx, cy, dx, dy, nx, ny;
    uint8_t row, type, ratio;
} drop_seg_geom_t;

static drop_seg_geom_t s_geom[DROP_SEG_MAX];
static uint8_t         s_geom_count;

/* The refresh's sim and the view's incremental one, static: together they
 * are over a kilobyte, which the UI task's frame cannot carry. */
static drop_sim_t s_sim_loop;
static drop_sim_t s_sim_view;
static uint32_t   s_sim_view_gen = 0;

static inline uint32_t sim_rand(drop_sim_t *s)
{
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return x;
}

static void sim_init(drop_sim_t *s, uint32_t seed)
{
    memset(s, 0, sizeof(*s));
    s->rng = seed ? seed : 0x9E3779B9u;
    for (uint8_t i = 0; i < DROP_BALL_MAX; i++) s->balls[i].last_impact = -1;
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
}

static inline uint32_t spawn_interval(void)
{
    return note_div_ticks(s_drop.rate);
}

/* Close the gate of the ball's pending impact at `tick`. */
static void sim_close_gate(drop_sim_t *s, drop_ball_t *b, uint32_t tick)
{
    if (b->last_impact >= 0) {
        drop_impact_t *im = &s->impacts[b->last_impact];
        im->gate = (uint16_t)(tick - im->tick);
        b->last_impact = -1;
    }
}

static void sim_spawn(drop_sim_t *s)
{
    for (uint8_t i = 0; i < DROP_BALL_MAX; i++) {
        drop_ball_t *b = &s->balls[i];
        if (b->alive) continue;
        int32_t jitter = (int32_t)(sim_rand(s) % (uint32_t)PX(DROP_CELL_W)) - PX(DROP_CELL_W) / 2;
        b->x = PX(DROP_SPAWN_COL * DROP_CELL_W) + PX(DROP_CELL_W) / 2 + jitter;
        b->y = 0;
        b->vx = 0;
        b->vy = 0;
        b->alive = true;
        b->in_jackpot = false;
        b->last_impact = -1;
        return;
    }
    /* Full: the spawner skips this beat. */
}

/* Test the move p0 -> p1 against segment g. On a crossing within the
 * segment's extent: reflect the velocity, place the ball just off the line on
 * its incoming side, and return the normal speed before the bounce. */
static bool seg_bounce(drop_sim_t *s, const drop_seg_geom_t *g, drop_ball_t *b,
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
    int32_t e = (g->type == DROP_SEG_DAMP) ? DROP_DAMP_RESTITUTION : DROP_RESTITUTION;
    int32_t term = (int32_t)(((int64_t)(256 + e) * vn) >> 8);
    b->vx -= (int32_t)(((int64_t)term * g->nx) >> DROP_Q);
    b->vy -= (int32_t)(((int64_t)term * g->ny) >> DROP_Q);
    int32_t side = (s0 > 0) ? 1 : -1;
    b->x = qx + side * (g->nx / 16);
    b->y = qy + side * (g->ny / 16);

    if (g->type == DROP_SEG_SPLIT) {
        /* A peg: the ball leaves left or right at no less than a quarter of
         * the impact speed, the side drawn from the loop's PRNG. */
        int32_t kick = speed / 4;
        int32_t mag  = (b->vx < 0) ? -b->vx : b->vx;
        if (mag < kick) mag = kick;
        bool left = (sim_rand(s) % 100u) < g->ratio;
        b->vx = left ? -mag : mag;
    }
    *speed_out = speed;
    return true;
}

static void sim_step(drop_sim_t *s)
{
    uint32_t tick = s->tick;
    if (tick % spawn_interval() == 0) sim_spawn(s);

    bool jp_on = s_drop.jackpot.enabled;
    for (uint8_t i = 0; i < DROP_BALL_MAX; i++) {
        drop_ball_t *b = &s->balls[i];
        if (!b->alive) continue;

        b->vy += DROP_GRAVITY;
        int32_t x1 = b->x + b->vx, y1 = b->y + b->vy;
        bool hit = false;
        for (uint8_t k = 0; k < s_geom_count && !hit; k++) {
            int32_t speed = 0;
            if (!seg_bounce(s, &s_geom[k], b, x1, y1, &speed)) continue;
            hit = true;
            sim_close_gate(s, b, tick);
            if (s->n_impacts < SEQ_DROP_IMPACT_MAX) {
                drop_impact_t *im = &s->impacts[s->n_impacts];
                im->tick  = (uint16_t)tick;
                im->gate  = 0;
                im->speed = (speed > 0xFFFF) ? 0xFFFFu : (uint16_t)speed;
                im->row   = s_geom[k].row;
                b->last_impact = (int8_t)s->n_impacts;
                s->n_impacts++;
            }
            int32_t ax = (b->vx < 0) ? -b->vx : b->vx;
            int32_t ay = (b->vy < 0) ? -b->vy : b->vy;
            if (ax < DROP_MIN_SPEED && ay < DROP_MIN_SPEED) {
                sim_close_gate(s, b, tick);
                b->alive = false;
            }
        }
        if (!hit) {
            b->x = x1;
            b->y = y1;
        }
        if (!b->alive) continue;
        if (b->x < 0 || b->x >= PX(DROP_FIELD_W) || b->y >= PX(DROP_FIELD_H) ||
            b->y < -PX(DROP_FIELD_H)) {
            sim_close_gate(s, b, tick);
            b->alive = false;
            continue;
        }
        if (jp_on) {
            uint8_t col = (uint8_t)((b->x >> DROP_Q) / DROP_CELL_W);
            uint8_t row = (uint8_t)((b->y < 0 ? 0 : b->y) >> DROP_Q) / DROP_CELL_H;
            bool inside = row == s_drop.jackpot.row &&
                          col >= s_drop.jackpot.col0 && col <= s_drop.jackpot.col1;
            if (inside && !b->in_jackpot && s->n_jackpots < SEQ_DROP_JACKPOT_MAX) {
                int32_t sp = (b->vy < 0) ? -b->vy : b->vy;
                s->jackpots[s->n_jackpots].tick  = (uint16_t)tick;
                s->jackpots[s->n_jackpots].speed = (sp > 0xFFFF) ? 0xFFFFu : (uint16_t)sp;
                s->n_jackpots++;
            }
            b->in_jackpot = inside;
        }
    }
    s->tick++;
}

/* Run the whole loop: impacts, crossings and gates, from the seed. */
static void sim_run_loop(drop_sim_t *s)
{
    uint32_t loop = drop_loop_ticks();
    sim_init(s, s_drop.seed);
    while (s->tick < loop) sim_step(s);
    /* Balls still in the air at loop end: their last note holds until the
     * loop restarts. */
    for (uint8_t i = 0; i < DROP_BALL_MAX; i++) {
        if (s->balls[i].alive) sim_close_gate(s, &s->balls[i], loop);
    }
}

uint32_t drop_loop_ticks(void)
{
    return (uint32_t)s_drop.loop_bars * SEQ_TICKS_PER_BAR;
}

uint8_t drop_view_balls(uint32_t loop_tick, drop_ball_pos_t out[DROP_BALL_MAX])
{
    uint32_t loop = drop_loop_ticks();
    if (loop_tick >= loop) loop_tick = loop - 1;
    if (s_sim_view_gen != s_drop_gen || s_sim_view.tick > loop_tick) {
        sim_init(&s_sim_view, s_drop.seed);
        s_sim_view_gen = s_drop_gen;
    }
    while (s_sim_view.tick <= loop_tick) sim_step(&s_sim_view);
    uint8_t n = 0;
    for (uint8_t i = 0; i < DROP_BALL_MAX; i++) {
        const drop_ball_t *b = &s_sim_view.balls[i];
        if (!b->alive) continue;
        int32_t x = b->x >> DROP_Q, y = b->y >> DROP_Q;
        if (x < 0 || x >= DROP_FIELD_W || y < 0 || y >= DROP_FIELD_H) continue;
        out[n].x = (uint8_t)x;
        out[n].y = (uint8_t)y;
        n++;
    }
    return n;
}

uint8_t drop_last_impact_count(void)  { return s_last_impacts; }
uint8_t drop_last_jackpot_count(void) { return s_last_jackpots; }

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
    uint32_t base = sequencer_core_drop_tag_base();
    for (uint32_t i = 0; i < SEQ_DROP_IMPACT_MAX + SEQ_DROP_JACKPOT_MAX; i++) {
        sequencer_core_drop_clear_note(base + i * 2u);
    }
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

/* The gate of impact i, shortened to end before the next impact at the same
 * pitch (the loop wrapping around counts), clamped to 1..period-1. */
static uint32_t gate_before_repeat(uint32_t gate, uint32_t tick, uint8_t i,
                                   const drop_sim_t *s, const uint8_t *notes,
                                   uint32_t period)
{
    uint32_t next = period + tick;   /* itself, one loop later */
    for (uint8_t j = 0; j < s->n_impacts; j++) {
        if (j == i || notes[j] != notes[i]) continue;
        uint32_t t = s->impacts[j].tick;
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
    s_last_impacts = 0;
    s_last_jackpots = 0;

    if (!s_drop.enabled || s_drop.solo_muted || s_drop.muted) return;
    if (s_drop.seg_count == 0 && !s_drop.jackpot.enabled) return;

    drop_sim_t *s = &s_sim_loop;
    sim_run_loop(s);

    uint32_t period = drop_loop_ticks();
    uint32_t base   = sequencer_core_drop_tag_base();
    float    trim   = s_drop.voice[DROP_VOICE_MAIN].vp.amp_trim;
    uint8_t  notes[SEQ_DROP_IMPACT_MAX];
    for (uint8_t i = 0; i < s->n_impacts; i++) notes[i] = drop_snap(s->impacts[i].row);
    for (uint8_t i = 0; i < s->n_impacts; i++) {
        const drop_impact_t *im = &s->impacts[i];
        uint32_t gate = ((uint32_t)im->gate * s_drop.gate_pct) / 100u;
        /* AMY's instrument keeps one voice per sounding pitch, so a note
         * must end before the next impact at the same pitch, in this loop
         * or the next. */
        gate = gate_before_repeat(gate, im->tick, i, s, notes, period);
        sequencer_core_drop_emit_note(DROP_SYNTH, base + (uint32_t)i * 2u,
                                      notes[i], drop_velocity(im->speed, trim),
                                      1u + im->tick, gate, period);
    }
    float sub_trim = s_drop.voice[DROP_VOICE_SUB].vp.amp_trim;
    uint8_t sub_note = sequencer_core_clamp_melodic_note((int32_t)s_drop.sub_root_note);
    for (uint8_t j = 0; j < s->n_jackpots; j++) {
        const drop_jp_hit_t *jp = &s->jackpots[j];
        uint32_t gate = ((uint32_t)spawn_interval() * s_drop.gate_pct) / 100u;
        uint32_t next = (j + 1u < s->n_jackpots) ? s->jackpots[j + 1].tick
                                                 : period + s->jackpots[0].tick;
        if (gate + 1u > next - jp->tick) gate = next - jp->tick - 1u;
        if (gate < 1) gate = 1;
        if (gate > period - 1) gate = period - 1;
        sequencer_core_drop_emit_note(DROP_SUB_SYNTH,
                                      base + (uint32_t)(SEQ_DROP_IMPACT_MAX + j) * 2u,
                                      sub_note, drop_velocity(jp->speed, sub_trim),
                                      1u + jp->tick, gate, period);
    }
    s_last_impacts  = s->n_impacts;
    s_last_jackpots = s->n_jackpots;
}

unsigned drop_core_dump(void)
{
    unsigned n = 0;
    printf("DROP en=%u mute=%u rate=%u bars=%u oct=%u gate=%u seed=%08X%s root=%u sub=%u q=%u"
           " patch=%u/%u segs=%u jp=%u(%u-%u,%u) loop=%u\n",
           (unsigned)s_drop.enabled, (unsigned)s_drop.muted, (unsigned)s_drop.rate,
           (unsigned)s_drop.loop_bars, (unsigned)s_drop.octaves, (unsigned)s_drop.gate_pct,
           (unsigned)s_drop.seed, s_drop.seed_frozen ? "*" : "",
           (unsigned)s_drop.root_note, (unsigned)s_drop.sub_root_note,
           (unsigned)s_drop.quant_mode, (unsigned)s_drop.voice[0].patch,
           (unsigned)s_drop.voice[1].patch, (unsigned)s_drop.seg_count,
           (unsigned)s_drop.jackpot.enabled, (unsigned)s_drop.jackpot.col0,
           (unsigned)s_drop.jackpot.col1, (unsigned)s_drop.jackpot.row,
           (unsigned)drop_loop_ticks());
    n++;
    for (uint8_t i = 0; i < s_drop.seg_count; i++) {
        const drop_seg_t *g = &s_drop.segs[i];
        printf("  SEG%u c=%u r=%u a=%u t=%u ratio=%u\n", i, g->col, g->row, g->angle,
               g->type, g->ratio);
        n++;
    }
    const drop_sim_t *s = &s_sim_loop;
    float trim = s_drop.voice[DROP_VOICE_MAIN].vp.amp_trim;
    for (uint8_t i = 0; i < s_last_impacts; i++) {
        const drop_impact_t *im = &s->impacts[i];
        printf("  IMP%u tick=%u row=%u note=%u vel=%.2f air=%u\n", i, im->tick, im->row,
               (unsigned)drop_snap(im->row), (double)drop_velocity(im->speed, trim),
               (unsigned)im->gate);
        n++;
    }
    for (uint8_t j = 0; j < s_last_jackpots; j++) {
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

static uint8_t drop_unison_copies(drop_which_t w)
{
    const drop_voice_t *v = &s_drop.voice[w];
    return sequencer_core_unison_copies_for(v->patch, v->unison.count,
                                            sequencer_core_drop_voices(slot_of(w)));
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
    return sequencer_core_voice_layout(s_drop.voice[w].patch, drop_unison_copies(w), vl);
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

/* The authored LFOs on a shared LFO synth over the patch's target map; an
 * unmapped patch, a full bank or no shared bank leaves the voice without
 * one (the drop has no software stepper). */
static void drop_apply_lfos(drop_which_t w)
{
#if CONFIG_SEQ_LFO_SHARED
    const drop_voice_t *v = &s_drop.voice[w];
    uint8_t synth = slot_of(w);
    uint16_t bpm  = sequencer_core_get_bpm();
    lfo_target_map_t map;
    if (!drop_lfo_target_map(w, &map)) {
        lfo_bank_unbind(&s_lfo_ref[w]);
        lfo_bank_unbind(&s_lfo2_ref[w]);
        return;
    }
    bool lfo_on = v->vp.lfo_authored && v->vp.lfo.enabled;
    (void)lfo_bank_bind(synth, VOICE_LFO_SLOT_LFO1, &map, lfo_on ? &v->vp.lfo : NULL,
                        bpm, &s_lfo_ref[w]);
    if (v->vp.lfo2_authored)
        (void)lfo_bank_bind(synth, VOICE_LFO_SLOT_LFO2, &map, &v->vp.lfo2, bpm,
                            &s_lfo2_ref[w]);
    else
        lfo_bank_unbind(&s_lfo2_ref[w]);
#else
    (void)w;
#endif
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
    sequencer_core_drop_configure(synth, v->patch, sequencer_core_drop_voices(synth),
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

static void drop_voice_defaults(drop_voice_t *v, uint16_t patch)
{
    memset(v, 0, sizeof(*v));
    voice_params_init_defaults(&v->vp);
    v->patch = SEQ_CLAMP_U16(patch, 0, SEQ_PATCH_FULL_MAX);
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

void drop_core_init(void)
{
    memset(&s_drop, 0, sizeof(s_drop));
    s_drop.enabled       = CONFIG_SEQ_DROP_DEFAULT_ENABLED;
    s_drop.rate          = NOTE_DIV_1_4;
    s_drop.loop_bars     = 2;
    s_drop.octaves       = 1;
    s_drop.gate_pct      = 75;
    s_drop.seed          = 0x1D0B5EEDu;
    s_drop.scale_index   = 0;
    s_drop.root_note     = SEQ_CLAMP_U8(CONFIG_SEQ_DROP_DEFAULT_ROOT_NOTE, 0, 127);
    s_drop.sub_root_note = (s_drop.root_note >= 12u) ? (uint8_t)(s_drop.root_note - 12u)
                                                     : s_drop.root_note;
    s_drop.quant_mode    = ARP_QUANT_CHORD;
    s_drop.jackpot = (drop_jackpot_t){ .col0 = 7, .col1 = 8, .row = DROP_ROWS - 1, .enabled = false };
    drop_voice_defaults(&s_drop.voice[DROP_VOICE_MAIN], CONFIG_SEQ_DROP_DEFAULT_PATCH);
    drop_voice_defaults(&s_drop.voice[DROP_VOICE_SUB], CONFIG_SEQ_DROP_SUB_DEFAULT_PATCH);

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

void drop_set_muted(bool muted)
{
    if (s_drop.muted == muted) return;
    s_drop.muted = muted;
    if (muted) drop_core_refresh();
    else       drop_mark_dirty();
}
bool drop_get_muted(void) { return s_drop.muted; }

void drop_set_solo_muted(bool muted)
{
    if (s_drop.solo_muted == muted) return;
    s_drop.solo_muted = muted;
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

void drop_set_gate_pct(uint8_t gate_pct)
{
    gate_pct = SEQ_CLAMP_U8(gate_pct, 10, 100);
    if (s_drop.gate_pct == gate_pct) return;
    s_drop.gate_pct = gate_pct;
    drop_mark_dirty();
}
uint8_t drop_get_gate_pct(void) { return s_drop.gate_pct; }

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

void drop_get_jackpot(drop_jackpot_t *out)
{
    if (out) *out = s_drop.jackpot;
}

void drop_set_jackpot(const drop_jackpot_t *jp)
{
    if (!jp) return;
    drop_jackpot_t v = *jp;
    v.col0 = SEQ_CLAMP_U8(v.col0, 0, DROP_COLS - 1);
    v.col1 = SEQ_CLAMP_U8(v.col1, v.col0, DROP_COLS - 1);
    v.row  = SEQ_CLAMP_U8(v.row, 0, DROP_ROWS - 1);
    if (memcmp(&s_drop.jackpot, &v, sizeof(v)) == 0) return;
    s_drop.jackpot = v;
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
    voice_unison_t u = { 1u, 12u, 50u, 100u, 0u };
    if (which_ok(w) && s_drop.voice[w].unison.count != 0u) u = s_drop.voice[w].unison;
    u.layout = sequencer_core_get_unison_layout();
    *out = u;
}

uint8_t drop_unison_max(drop_which_t w)
{
    if (!which_ok(w)) return 0u;
    return sequencer_core_unison_max_for(s_drop.voice[w].patch,
                                         sequencer_core_drop_voices(slot_of(w)));
}

static voice_unison_t drop_unison_clamp(const voice_unison_t *u)
{
    voice_unison_t v = *u;
    v.count        = SEQ_CLAMP_U8(v.count, 0u, VOICE_UNISON_MAX_COPIES);
    v.detune_cents = SEQ_CLAMP_U8(v.detune_cents, 0u, VOICE_UNISON_MAX_DETUNE);
    v.spread_pct   = SEQ_CLAMP_U8(v.spread_pct, 0u, 100u);
    v.blend_pct    = SEQ_CLAMP_U8(v.blend_pct, 0u, 100u);
    v.layout       = 0u;
    return v;
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
