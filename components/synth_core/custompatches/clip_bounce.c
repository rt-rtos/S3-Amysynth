/* clip_bounce.c - loop bounce state machine and capture path. The header
 * carries the contract; sample_rec.c is the sibling this follows (atomics for
 * the UI/render handshake, render task as the only buffer writer). */

#include "custompatches/clip_bounce.h"

#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fx_bus.h"
#include "sequencer_core.h"
#include "seq_core_config.h"
#include "custompatches/clip_player.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"
#include "arp_core.h"

static const char *TAG = "clip_bounce";

_Static_assert(AMY_NCHANS == 2, "the capture converts a stereo bus sum");

/* A stop landing closer than this to its boundary rolls to the next one:
 * the note-on reschedule and the trim must be through before the bar line,
 * and the render tick must still be writing well inside the trimmed block. */
#define CLIP_STOP_GUARD_TICKS 4u

static clip_bounce_shape_t s_shape = { .max_bars = 4, .stereo = false, .tail = CLIP_TAIL_BAR,
                                       .after = CLIP_AFTER_MUTE };

static _Atomic(clip_bounce_state_t) s_state = CLIP_BOUNCE_IDLE;
static _Atomic uint32_t s_write_idx;
static _Atomic uint32_t s_tail_idx;
/* Raised by the render task when the capture can no longer be trusted (the
 * tick rate changed under it); the UI service turns it into a cancel. */
static _Atomic bool s_abort;

/* Fixed at start; the stop press rewrites the end, the loop and the tail
 * (UI task, release stores) while the render task reads them per block. */
static uint8_t  s_slot;
static uint8_t  s_bars;
static bool     s_stereo;
static clip_after_t s_after;
static bool     s_stop_set;
static uint32_t s_start_tick;
static uint32_t s_unit_ticks;
static uint32_t s_us_per_tick;
static _Atomic uint32_t s_end_tick;
static _Atomic uint32_t s_loop_frames;
static _Atomic uint32_t s_alloc_frames;
static _Atomic uint32_t s_tail_frames;
static int16_t *s_buf;

/* One-level undo of the last commit: what the freeze replaced. layers is
 * the pre-Clear copy of every layer (PSRAM, a few KB each), NULL after a
 * Mute commit. */
static struct {
    bool    valid;
    uint8_t slot;
    uint8_t mutes[MAX_LAYERS];
    bool    arp;
    bool    drone;
    bool    drone_std;
    seq_layer_t *layers;
    uint8_t      layer_count;
} s_undo;

static void undo_drop_layers(void)
{
    heap_caps_free(s_undo.layers);
    s_undo.layers      = NULL;
    s_undo.layer_count = 0;
}

static bool undo_save_layers(void)
{
    uint8_t n = sequencer_core_get_num_layers();
    seq_layer_t *buf = heap_caps_malloc((size_t)n * sizeof(seq_layer_t), MALLOC_CAP_SPIRAM);
    if (buf == NULL) return false;
    for (uint8_t i = 0; i < n; i++) sequencer_core_export_layer(i, &buf[i]);
    s_undo.layers      = buf;
    s_undo.layer_count = n;
    return true;
}

/* One block of the source-bus sum, AMY_NCHANS sequential channel blocks like
 * the bus buffers themselves. Written by the hook under the lock, read by the
 * tick after it: both on the render task, so program order is enough. */
static SAMPLE s_acc[AMY_NCHANS * AMY_BLOCK_SIZE];

static uint32_t us_per_tick_now(void)
{
    uint32_t upt = amy_global.us_per_tick;
    if (upt == 0) {
        uint16_t bpm = sequencer_core_get_bpm();
        if (bpm == 0) bpm = 120;
        upt = (uint32_t)(60000000ull / ((uint64_t)bpm * AMY_SEQUENCER_PPQ));
    }
    return upt;
}

/* Exact frame count a tick span covers at this tick rate. AMY's tick period
 * is an integer microsecond count, so this is what the tick grid actually
 * spans, not the nominal BPM's. */
static uint32_t frames_for_ticks(uint32_t ticks, uint32_t us_per_tick)
{
    uint64_t num = (uint64_t)ticks * us_per_tick * AMY_SAMPLE_RATE;
    return (uint32_t)((num + 500000ull) / 1000000ull);
}

static uint32_t ceil_to_block(uint32_t frames)
{
    return (frames + AMY_BLOCK_SIZE - 1u) / AMY_BLOCK_SIZE * AMY_BLOCK_SIZE;
}

/* The pattern period in bars, capped to the reservation ceiling. */
static uint8_t unit_bars_now(void)
{
    uint8_t u = sequencer_core_pattern_period_bars();
    if (u == 0) u = 1;
    if (u > CLIP_BOUNCE_MAX_BARS) u = CLIP_BOUNCE_MAX_BARS;
    return u;
}

/* Bars reserved for a shape: Max rounded up to whole periods, inside the
 * ceiling, never less than one period. */
static uint8_t reserve_bars_for(uint8_t max_bars, uint8_t unit)
{
    uint32_t r = ((uint32_t)max_bars + unit - 1u) / unit * unit;
    if (r > CLIP_BOUNCE_MAX_BARS) r = (CLIP_BOUNCE_MAX_BARS / unit) * unit;
    if (r < unit) r = unit;
    return (uint8_t)r;
}

static uint32_t tail_frames_for(clip_tail_t tail, uint32_t upt, uint32_t loop)
{
    uint32_t t = 0;
    if (tail == CLIP_TAIL_BEAT)     t = frames_for_ticks(SEQ_TICKS_PER_BAR / 4u, upt);
    else if (tail == CLIP_TAIL_BAR) t = frames_for_ticks(SEQ_TICKS_PER_BAR, upt);
    if (t > loop / 2u) t = loop / 2u;
    return t;
}

void clip_bounce_init(void)
{
    atomic_store_explicit(&s_state, CLIP_BOUNCE_IDLE, memory_order_relaxed);
    atomic_store_explicit(&s_write_idx, 0, memory_order_relaxed);
    atomic_store_explicit(&s_tail_idx, 0, memory_order_relaxed);
    atomic_store_explicit(&s_abort, false, memory_order_relaxed);
    s_undo.valid = false;
}

clip_bounce_state_t clip_bounce_get_state(void)
{
    return atomic_load_explicit(&s_state, memory_order_acquire);
}

void clip_bounce_get_shape(clip_bounce_shape_t *out)
{
    if (out) *out = s_shape;
}

void clip_bounce_set_shape(const clip_bounce_shape_t *shape)
{
    if (!shape) return;
    uint8_t b = shape->max_bars;
    if (b != 1 && b != 2 && b != 4 && b != 8 && b != 16) b = 4;
    s_shape.max_bars = b;
    s_shape.stereo   = shape->stereo;
    s_shape.tail     = (shape->tail > CLIP_TAIL_BAR) ? CLIP_TAIL_BAR : shape->tail;
    s_shape.after    = (shape->after > CLIP_AFTER_CLEAR) ? CLIP_AFTER_MUTE : shape->after;
}

uint32_t clip_bounce_bytes_for(const clip_bounce_shape_t *shape)
{
    if (!shape) return 0;
    uint8_t  bars   = reserve_bars_for(shape->max_bars, unit_bars_now());
    uint32_t frames = ceil_to_block(frames_for_ticks((uint32_t)bars * SEQ_TICKS_PER_BAR, us_per_tick_now()));
    return frames * (shape->stereo ? 2u : 1u) * (uint32_t)sizeof(int16_t);
}

bool clip_bounce_fits(const clip_bounce_shape_t *shape)
{
    uint64_t total = (uint64_t)clip_player_pool_used() + clip_bounce_bytes_for(shape);
    return total <= (uint64_t)CLIP_BOUNCE_POOL_KB * 1024u;
}

bool clip_bounce_start(uint8_t slot)
{
    if (clip_bounce_get_state() != CLIP_BOUNCE_IDLE) return false;
    if (slot == CLIP_SLOT_AUTO) slot = clip_player_first_empty_slot();
    if (slot >= CLIP_SLOT_COUNT) return false;
    if (clip_player_slot_state(slot) != CLIP_SLOT_EMPTY) return false;
    uint32_t start = sequencer_core_next_bar_tick();
    if (start == 0) return false;                 /* transport stopped */
    uint32_t upt = amy_global.us_per_tick;
    if (upt == 0) return false;
    clip_bounce_shape_t sh = s_shape;
    if (!clip_bounce_fits(&sh)) return false;

    uint8_t  unit  = unit_bars_now();
    uint8_t  bars  = reserve_bars_for(sh.max_bars, unit);
    uint32_t loop  = frames_for_ticks((uint32_t)bars * SEQ_TICKS_PER_BAR, upt);
    uint32_t alloc = ceil_to_block(loop);
    int16_t *buf = clip_player_slot_alloc(slot, bars, sh.stereo, alloc, loop, upt);
    if (buf == NULL) return false;

    uint32_t end = start + (uint32_t)bars * SEQ_TICKS_PER_BAR;
    s_slot        = slot;
    s_bars        = bars;
    s_stereo      = sh.stereo;
    s_after       = sh.after;
    s_stop_set    = false;
    s_start_tick  = start;
    s_unit_ticks  = (uint32_t)unit * SEQ_TICKS_PER_BAR;
    s_us_per_tick = upt;
    s_buf         = buf;
    atomic_store_explicit(&s_loop_frames,  loop,  memory_order_relaxed);
    atomic_store_explicit(&s_alloc_frames, alloc, memory_order_relaxed);
    atomic_store_explicit(&s_tail_frames,  tail_frames_for(sh.tail, upt, loop), memory_order_relaxed);
    atomic_store_explicit(&s_end_tick, end, memory_order_relaxed);
    atomic_store_explicit(&s_write_idx, 0, memory_order_relaxed);
    atomic_store_explicit(&s_tail_idx, 0, memory_order_relaxed);
    atomic_store_explicit(&s_abort, false, memory_order_relaxed);
    clip_player_schedule_start(slot, end);
    sequencer_core_freeze_set(end);
    atomic_store_explicit(&s_state, CLIP_BOUNCE_ARMED, memory_order_release);
    ESP_LOGI(TAG, "start: slot %u, up to %u bars (period %u), ticks %lu..%lu",
             (unsigned)slot, (unsigned)bars, (unsigned)unit,
             (unsigned long)start, (unsigned long)end);
    return true;
}

bool clip_bounce_stop(void)
{
    if (clip_bounce_get_state() != CLIP_BOUNCE_RECORDING) return false;
    if (s_stop_set) return true;
    uint32_t now = amy_global.sequencer_tick_count;
    uint32_t auto_end = atomic_load_explicit(&s_end_tick, memory_order_relaxed);
    uint32_t elapsed = now - s_start_tick;
    uint32_t end = s_start_tick + (elapsed / s_unit_ticks + 1u) * s_unit_ticks;
    if (end - now < CLIP_STOP_GUARD_TICKS) end += s_unit_ticks;
    s_stop_set = true;
    if (end >= auto_end) return true;             /* the reservation's own end */

    uint32_t ticks = end - s_start_tick;
    uint8_t  bars  = (uint8_t)(ticks / SEQ_TICKS_PER_BAR);
    uint32_t loop  = frames_for_ticks(ticks, s_us_per_tick);
    uint32_t alloc = ceil_to_block(loop);
    /* Bounds first, so the render tick never writes past the trimmed block;
     * its current position is a guard's worth of ticks below the new loop
     * end. Then the trim under the lock, then the earlier end tick. */
    atomic_store_explicit(&s_tail_frames,  tail_frames_for(s_shape.tail, s_us_per_tick, loop), memory_order_relaxed);
    atomic_store_explicit(&s_loop_frames,  loop,  memory_order_relaxed);
    atomic_store_explicit(&s_alloc_frames, alloc, memory_order_release);
    if (!clip_player_slot_trim(s_slot, bars, alloc, loop)) {
        ESP_LOGW(TAG, "trim failed; keeping the reservation");
        atomic_store_explicit(&s_loop_frames,  frames_for_ticks(auto_end - s_start_tick, s_us_per_tick), memory_order_relaxed);
        atomic_store_explicit(&s_alloc_frames, ceil_to_block(atomic_load_explicit(&s_loop_frames, memory_order_relaxed)), memory_order_release);
        return true;
    }
    s_bars = bars;
    atomic_store_explicit(&s_end_tick, end, memory_order_release);
    clip_player_schedule_start(s_slot, end);
    sequencer_core_freeze_set(end);
    ESP_LOGI(TAG, "stop: %u bars, end tick %lu", (unsigned)bars, (unsigned long)end);
    return true;
}

void clip_bounce_cancel(void)
{
    if (clip_bounce_get_state() == CLIP_BOUNCE_IDLE) return;
    /* IDLE first: the render tick checks the state before every write, and
     * the slot's buffer outlives any write already in flight (the unload is
     * deferred several blocks on the render task itself). */
    atomic_store_explicit(&s_state, CLIP_BOUNCE_IDLE, memory_order_release);
    sequencer_core_freeze_clear();
    clip_player_clear(s_slot);
    ESP_LOGI(TAG, "cancelled");
}

void clip_bounce_chord(bool long_press)
{
    switch (clip_bounce_get_state()) {
        case CLIP_BOUNCE_IDLE:
            if (!long_press) clip_bounce_start(CLIP_SLOT_AUTO);
            break;
        case CLIP_BOUNCE_ARMED:
            clip_bounce_cancel();
            break;
        case CLIP_BOUNCE_RECORDING:
            if (long_press) clip_bounce_cancel();
            else            clip_bounce_stop();
            break;
        default:
            break;
    }
}

void clip_bounce_undo(void)
{
    if (!s_undo.valid) return;
    s_undo.valid = false;
    if (s_undo.layers != NULL) {
        uint8_t n = sequencer_core_get_num_layers();
        for (uint8_t i = 0; i < n && i < s_undo.layer_count; i++) {
            sequencer_core_import_layer(i, &s_undo.layers[i]);
        }
        undo_drop_layers();
    }
    sequencer_core_set_mute_masks(s_undo.mutes);
    arp_set_enabled(s_undo.arp);
    drone_set_enabled(s_undo.drone);
    drone_std_set_enabled(s_undo.drone_std);
    if (clip_bounce_get_state() != CLIP_BOUNCE_IDLE && s_slot == s_undo.slot) {
        clip_bounce_cancel();
    } else {
        clip_player_clear(s_undo.slot);
    }
    ESP_LOGI(TAG, "undo: slot %u", (unsigned)s_undo.slot);
}

uint8_t clip_bounce_slot(void)        { return s_slot; }
uint8_t clip_bounce_bars_total(void)  { return s_bars; }
bool    clip_bounce_stop_set(void)    { return s_stop_set; }

uint8_t clip_bounce_bars_done(void)
{
    if (clip_bounce_get_state() != CLIP_BOUNCE_RECORDING || s_bars == 0) return 0;
    uint32_t per_bar = atomic_load_explicit(&s_loop_frames, memory_order_relaxed) / s_bars;
    if (per_bar == 0) return 0;
    return (uint8_t)(atomic_load_explicit(&s_write_idx, memory_order_relaxed) / per_bar);
}

void clip_bounce_service(void)
{
    if (atomic_load_explicit(&s_abort, memory_order_acquire)) {
        atomic_store_explicit(&s_abort, false, memory_order_relaxed);
        ESP_LOGW(TAG, "tempo changed during capture");
        clip_bounce_cancel();
        return;
    }
    if (clip_bounce_get_state() != CLIP_BOUNCE_READY) return;
    /* The sources are silent since the release at end_tick; make the freeze
     * theirs to undo: real mute flags (or, under Clear, empty patterns) and
     * enable flags, then lift the horizon so the tracks resume. What they
     * were is kept for Undo. A Clear that cannot afford its undo copy mutes
     * instead: nothing is thrown away without a way back. */
    undo_drop_layers();
    sequencer_core_get_mute_masks(s_undo.mutes);
    s_undo.arp       = arp_get_enabled();
    s_undo.drone     = drone_get_enabled();
    s_undo.drone_std = drone_std_get_enabled();
    s_undo.slot      = s_slot;
    s_undo.valid     = true;
    clip_after_t after = s_after;
    if (after == CLIP_AFTER_CLEAR && !undo_save_layers()) {
        ESP_LOGW(TAG, "no RAM for the pattern undo; muting instead");
        after = CLIP_AFTER_MUTE;
    }
    if (after == CLIP_AFTER_CLEAR) {
        uint8_t n = sequencer_core_get_num_layers();
        for (uint8_t i = 0; i < n; i++) {
            for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
                sequencer_core_clear_track_pattern(i, t);
            }
        }
    } else {
        sequencer_core_mute_all_tracks();
    }
    arp_set_enabled(false);
    drone_set_enabled(false);
    drone_std_set_enabled(false);
    sequencer_core_freeze_clear();
    atomic_store_explicit(&s_state, CLIP_BOUNCE_IDLE, memory_order_release);
    ESP_LOGI(TAG, "slot %u committed", (unsigned)s_slot);
}

void clip_bounce_bus_hook(uint16_t bus, SAMPLE *buf, uint16_t len)
{
    clip_bounce_state_t st = atomic_load_explicit(&s_state, memory_order_relaxed);
    if (st == CLIP_BOUNCE_IDLE || st == CLIP_BOUNCE_READY) return;
    if (bus == 0) {
        /* The tick count advanced at this block's start; bus 0 is the first
         * hook call of the block, so a transition covers the whole block. */
        uint32_t now = amy_global.sequencer_tick_count;
        if (st == CLIP_BOUNCE_ARMED) {
            if ((int32_t)(now - s_start_tick) < 0) return;
            st = CLIP_BOUNCE_RECORDING;
            atomic_store_explicit(&s_state, st, memory_order_release);
        }
        if (st == CLIP_BOUNCE_RECORDING) {
            if (amy_global.us_per_tick != s_us_per_tick) {
                atomic_store_explicit(&s_abort, true, memory_order_release);
                return;
            }
            if ((int32_t)(now - atomic_load_explicit(&s_end_tick, memory_order_acquire)) >= 0) {
                /* The clip's note-on fired at this block's start; from here
                 * the sum is ring-out. Release the sources (one block late,
                 * on the pump task) and fold the tail behind the playhead. */
                sequencer_core_freeze_release_enqueue();
                st = CLIP_BOUNCE_TAIL;
                atomic_store_explicit(&s_state, st, memory_order_release);
            }
        }
    }
    if (st != CLIP_BOUNCE_RECORDING && st != CLIP_BOUNCE_TAIL) return;
    if (bus >= FX_BUS_CLIPS) return;

    /* The same per-bus gain the mixdown applies (amy_render_audio), so the
     * clip holds what the host hears; the clip bus carries the inverse
     * (FX_BUS_CLIPS_MAKEUP) so the mixdown's 0.1 is not applied twice. */
    SAMPLE scale = MUL4_SS(F2S(0.1f), F2S(amy_global.volume[bus]));
    uint32_t n = (uint32_t)len * AMY_NCHANS;
    if (n > AMY_NCHANS * AMY_BLOCK_SIZE) n = AMY_NCHANS * AMY_BLOCK_SIZE;

    /* AMY walks the buses in order and bus 0 is never idle, so bus 0 opens
     * the block's sum instead of clearing the accumulator separately. */
    if (bus == 0) {
        for (uint32_t i = 0; i < n; i++) s_acc[i] = MUL8_SS(scale, buf[i]);
    } else {
        for (uint32_t i = 0; i < n; i++) s_acc[i] += MUL8_SS(scale, buf[i]);
    }
}

/* s8.23 bus sum to the int16 the mixdown would emit (S2L, hard clamp). The
 * mixdown's soft clip is not replicated: the sources rarely reach it, and the
 * clip plays back through that same output stage anyway. */
static inline int16_t acc_to_int16(SAMPLE v)
{
    int32_t l = S2L(v);
    if (l > SAMPLE_MAX)  l = SAMPLE_MAX;
    if (l < -SAMPLE_MAX) l = -SAMPLE_MAX;
    return (int16_t)l;
}

static inline int16_t sat16(int32_t v)
{
    if (v > SAMPLE_MAX)  return SAMPLE_MAX;
    if (v < -SAMPLE_MAX) return -SAMPLE_MAX;
    return (int16_t)v;
}

/* Loop frames: the block sum written straight into the clip. */
static void write_loop_block(uint32_t frames)
{
    uint32_t alloc = atomic_load_explicit(&s_alloc_frames, memory_order_acquire);
    uint32_t idx = atomic_load_explicit(&s_write_idx, memory_order_relaxed);
    if (idx >= alloc) return;
    uint32_t n = frames;
    if (n > alloc - idx) n = alloc - idx;

    const SAMPLE *l = s_acc;
    const SAMPLE *r = s_acc + AMY_BLOCK_SIZE;
    if (s_stereo) {
        int16_t *dst = s_buf + (size_t)idx * 2u;
        for (uint32_t i = 0; i < n; i++) {
            dst[i * 2u]      = acc_to_int16(l[i]);
            dst[i * 2u + 1u] = acc_to_int16(r[i]);
        }
    } else {
        int16_t *dst = s_buf + idx;
        for (uint32_t i = 0; i < n; i++) {
            dst[i] = acc_to_int16((l[i] >> 1) + (r[i] >> 1));
        }
    }
    atomic_store_explicit(&s_write_idx, idx + n, memory_order_release);
}

/* Tail frames: the block sum added into the clip head at the same offset
 * the playhead has just passed. The clip's render read these head frames
 * before this call, so the fold lands behind it. */
static void fold_tail_block(uint32_t frames)
{
    uint32_t tail = atomic_load_explicit(&s_tail_frames, memory_order_relaxed);
    uint32_t idx = atomic_load_explicit(&s_tail_idx, memory_order_relaxed);
    if (idx >= tail) return;
    uint32_t n = frames;
    if (n > tail - idx) n = tail - idx;

    const SAMPLE *l = s_acc;
    const SAMPLE *r = s_acc + AMY_BLOCK_SIZE;
    if (s_stereo) {
        int16_t *dst = s_buf + (size_t)idx * 2u;
        for (uint32_t i = 0; i < n; i++) {
            dst[i * 2u]      = sat16((int32_t)dst[i * 2u]      + acc_to_int16(l[i]));
            dst[i * 2u + 1u] = sat16((int32_t)dst[i * 2u + 1u] + acc_to_int16(r[i]));
        }
    } else {
        int16_t *dst = s_buf + idx;
        for (uint32_t i = 0; i < n; i++) {
            dst[i] = sat16((int32_t)dst[i] + acc_to_int16((l[i] >> 1) + (r[i] >> 1)));
        }
    }
    atomic_store_explicit(&s_tail_idx, idx + n, memory_order_release);
}

void clip_bounce_render_tick(const int16_t *interleaved_stereo, uint16_t frames)
{
    (void)interleaved_stereo;
    clip_player_render_service();
    clip_player_bar_check();

    clip_bounce_state_t st = atomic_load_explicit(&s_state, memory_order_acquire);
    if (st != CLIP_BOUNCE_RECORDING && st != CLIP_BOUNCE_TAIL) return;
    uint32_t n = frames;
    if (n > AMY_BLOCK_SIZE) n = AMY_BLOCK_SIZE;

    /* The block that reaches end_tick can still owe the loop its last
     * frames and is the tail's first: the same sum serves both. */
    if (atomic_load_explicit(&s_write_idx, memory_order_relaxed)
        < atomic_load_explicit(&s_loop_frames, memory_order_acquire)) {
        write_loop_block(n);
    }
    if (st == CLIP_BOUNCE_TAIL) {
        fold_tail_block(n);
        if (atomic_load_explicit(&s_tail_idx, memory_order_relaxed)
            >= atomic_load_explicit(&s_tail_frames, memory_order_relaxed)) {
            atomic_store_explicit(&s_state, CLIP_BOUNCE_READY, memory_order_release);
        }
    }
}
