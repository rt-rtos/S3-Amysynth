/* clip_player.c - bounce clip slots as AMY synths. The header carries the
 * contract. Sibling of sample_rec.c for the memory-preset lifecycle: the UI
 * task allocates, the render task frees at a block edge. */

#include "custompatches/clip_player.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "amy.h"
#include "amy_helpers.h"
#include "amy_fx.h"
#include "fx_bus.h"
#include "sequencer_core.h"
#include "seq_core_config.h"
#include "custompatches/clip_bounce.h"

static const char *TAG = "clip_player";

/* Render blocks between the osc reset and the preset unload. The reset is a
 * queued delta and executes at the next block start; the unload waits well
 * past that so no render ever reads a freed table. */
#define CLIP_UNLOAD_DEFER_BLOCKS 4u

/* Drift guard tolerance in source frames. The stretcher's input timeline
 * leads its output by up to a hop, so this sits above one block plus a hop;
 * a real drift (the loop and the grid diverge by under half a frame per
 * pass) takes many minutes to reach it. */
#define CLIP_DRIFT_TOLERANCE_FRAMES 1024u

typedef struct {
    _Atomic(clip_slot_state_t) state;
    _Atomic uint8_t unload_countdown;
    _Atomic bool    reanchor_req;   /* raised by the drift guard (render task) */
    uint8_t  bars;
    bool     stereo;
    bool     playing;   /* user Play/Mute */
    bool     started;   /* a note-on is scheduled or sounding since the last reset */
    uint8_t  level;     /* percent */
    clip_tempo_mode_t mode;
    uint32_t bytes;
    uint32_t loop_frames;
    uint32_t clip_ticks;
    uint32_t rec_us_per_tick;
    uint32_t anchor_tick;   /* tick at which source frame 0 plays */
    uint32_t retrig_tick;   /* a start is scheduled here; the guard waits for it */
    uint16_t osc0;          /* base osc of the voice, resolved lazily; 0xFFFF = unknown */
} clip_slot_t;

static clip_slot_t s_slot[CLIP_SLOT_COUNT];
static bool s_bus_active;
static uint32_t s_last_bar;   /* bar index the guard last ran on */

static inline uint8_t  slot_synth(uint8_t slot)  { return (uint8_t)(CLIP_SYNTH_BASE + slot); }
static inline uint16_t slot_preset(uint8_t slot) { return (uint16_t)(CLIP_PRESET_BASE + slot); }
static inline uint8_t  slot_oscs(const clip_slot_t *s) { return s->stereo ? 2u : 1u; }
static inline uint32_t slot_tag_aux(uint8_t slot)   { return SEQ_CLIP_TAG_BASE + 2u * slot; }
static inline uint32_t slot_tag_start(uint8_t slot) { return SEQ_CLIP_TAG_BASE + 2u * slot + 1u; }

/* AMY's own integer tick period for a BPM (sequencer.c), so the comparison
 * with the recorded value is exact rather than through a float round trip. */
static uint32_t us_per_tick_for_bpm(uint16_t bpm)
{
    if (bpm == 0) bpm = 120;
    uint32_t upt = (uint32_t)(60000000.0f / ((float)bpm * (float)AMY_SEQUENCER_PPQ));
    return upt < 50u ? 50u : upt;
}

/* The freq const that plays the clip at rate rec/now: logfreq is log2 of
 * freq over ZERO_LOGFREQ_IN_HZ, and the PCM rate is 2^logfreq. Zero means
 * "unset" and restores the exact-rate path. */
static float vari_freq_const(const clip_slot_t *s, uint32_t upt_now)
{
    if (s->mode != CLIP_TEMPO_VARI || upt_now == s->rec_us_per_tick) return 0.0f;
    return ZERO_LOGFREQ_IN_HZ * (float)s->rec_us_per_tick / (float)upt_now;
}

static float slot_gain(const clip_slot_t *s)
{
    return s->playing ? (float)s->level / 100.0f : 0.0f;
}

static void push_gain(uint8_t slot)
{
    const clip_slot_t *s = &s_slot[slot];
    for (uint8_t o = 0; o < slot_oscs(s); o++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth = slot_synth(slot);
        e->osc   = o;
        e->amp_coefs[COEF_CONST] = slot_gain(s);
        amy_helpers_config_send(e);
    }
}

/* One voice, one PCM osc per channel. A stereo clip chains the right osc to
 * the left one so a single note-on starts both. */
static void define_instrument(uint8_t slot)
{
    const clip_slot_t *s = &s_slot[slot];
    amy_event *e = amy_helpers_event_begin();
    e->synth          = slot_synth(slot);
    e->num_voices     = 1;
    e->oscs_per_voice = slot_oscs(s);
    e->synth_flags    = 0;
    e->bus            = FX_BUS_CLIPS;
    amy_helpers_config_send(e);
}

/* Osc-level config: wave, preset, forever loop, gain. Re-sent after a
 * transport stop, which resets the oscs but keeps the instrument. */
static void configure_oscs(uint8_t slot)
{
    const clip_slot_t *s = &s_slot[slot];
    amy_event *e = amy_helpers_event_begin();
    e->synth  = slot_synth(slot);
    e->osc    = 0;
    e->wave   = s->stereo ? PCM_LEFT : PCM;
    e->preset = (int16_t)slot_preset(slot);
    e->mode   = PCM_LOOP_FOREVER;
    e->amp_coefs[COEF_CONST] = slot_gain(s);
    if (s->stereo) {
        e->pan_coefs[COEF_CONST] = 0.0f;
        e->chained_osc = 1;
    }
    amy_helpers_config_send(e);
    if (s->stereo) {
        e = amy_helpers_event_begin();
        e->synth  = slot_synth(slot);
        e->osc    = 1;
        e->wave   = PCM_RIGHT;
        e->preset = (int16_t)slot_preset(slot);
        e->mode   = PCM_LOOP_FOREVER;
        e->amp_coefs[COEF_CONST] = slot_gain(s);
        e->pan_coefs[COEF_CONST] = 1.0f;
        amy_helpers_config_send(e);
    }
}

static void reset_oscs(uint8_t slot)
{
    const clip_slot_t *s = &s_slot[slot];
    for (uint8_t o = 0; o < slot_oscs(s); o++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth     = slot_synth(slot);
        e->reset_osc = o;
        amy_helpers_config_send(e);
    }
}

/* tick 0 / period 0 under a tag is AMY's cancel form. */
static void cancel_tag(uint8_t slot, uint32_t tag)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth               = slot_synth(slot);
    e->ticks[TICKS_TAG]    = tag;
    e->ticks[TICKS_TICK]   = 0;
    e->ticks[TICKS_PERIOD] = 0;
    amy_helpers_event_send(e);
}

static void cancel_start(uint8_t slot)
{
    cancel_tag(slot, slot_tag_aux(slot));
    cancel_tag(slot, slot_tag_start(slot));
}

/* Start parameters for one osc at a bar-line (re)start: where in the source
 * to begin, and the tempo regime. Stretch fits the remainder of the clip to
 * the remaining ticks, so the ratio equals a full-clip fit and the loop
 * keeps it; -1 turns the stretcher off. */
static void fill_start_params(amy_event *e, const clip_slot_t *s, uint32_t offset_ticks,
                              uint32_t start_frame, uint32_t upt_now)
{
    e->trigger_phase = (float)start_frame / (float)(1u << 23);   /* PCM_INDEX_BITS */
    bool stretch = s->mode == CLIP_TEMPO_STRETCH && upt_now != s->rec_us_per_tick;
    e->fit_ticks = stretch ? (float)(s->clip_ticks - offset_ticks) : -1.0f;
    e->freq_coefs[COEF_CONST] = vari_freq_const(s, upt_now);
}

/* Schedule the slot's (re)start at bar_tick from the source position the
 * grid implies there (frame 0 when the tick re-anchors the clip). The aux
 * config entry primes the right-channel osc first; the note-on then starts
 * both through the chain. */
static void schedule_start_at(uint8_t slot, uint32_t bar_tick, bool reanchor)
{
    clip_slot_t *s = &s_slot[slot];
    uint32_t upt_now = us_per_tick_for_bpm(sequencer_core_get_bpm());
    uint32_t offset_ticks = 0;
    if (!reanchor && s->clip_ticks != 0) {
        offset_ticks = (bar_tick - s->anchor_tick) % s->clip_ticks;
    }
    uint32_t start_frame = (s->clip_ticks != 0)
        ? (uint32_t)(((uint64_t)offset_ticks * s->loop_frames) / s->clip_ticks) : 0;
    if (reanchor) s->anchor_tick = bar_tick;

    if (s->stereo) {
        amy_event *e = amy_helpers_event_begin();
        e->synth = slot_synth(slot);
        e->osc   = 1;
        fill_start_params(e, s, offset_ticks, start_frame, upt_now);
        e->ticks[TICKS_TAG]    = slot_tag_aux(slot);
        e->ticks[TICKS_TICK]   = bar_tick;
        e->ticks[TICKS_PERIOD] = 0;
        amy_helpers_event_send(e);
    }
    amy_event *e = amy_helpers_event_begin();
    e->synth    = slot_synth(slot);
    e->velocity = 1.0f;
    fill_start_params(e, s, offset_ticks, start_frame, upt_now);
    e->ticks[TICKS_TAG]    = slot_tag_start(slot);
    e->ticks[TICKS_TICK]   = bar_tick;
    e->ticks[TICKS_PERIOD] = 0;
    amy_helpers_event_send(e);
    s->started     = true;
    s->retrig_tick = bar_tick;
}

void clip_player_schedule_start(uint8_t slot, uint32_t tick)
{
    if (slot >= CLIP_SLOT_COUNT) return;
    schedule_start_at(slot, tick, true);
}

/* Re-rate a VARI clip in place: the coefficient applies live, the playhead
 * runs on, so the change is seamless. */
static void push_vari_rate(uint8_t slot, uint32_t upt_now)
{
    const clip_slot_t *s = &s_slot[slot];
    for (uint8_t o = 0; o < slot_oscs(s); o++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth = slot_synth(slot);
        e->osc   = o;
        e->freq_coefs[COEF_CONST] = vari_freq_const(s, upt_now);
        amy_helpers_config_send(e);
    }
}

/* Retrigger at the next bar line under the current regime, keeping the
 * slot's grid position. No-op while stopped. */
static void retrigger_next_bar(uint8_t slot)
{
    uint32_t tick = sequencer_core_next_bar_tick();
    if (tick == 0) return;
    schedule_start_at(slot, tick, false);
}

void clip_player_init(void)
{
    for (uint8_t i = 0; i < CLIP_SLOT_COUNT; i++) {
        atomic_store_explicit(&s_slot[i].state, CLIP_SLOT_EMPTY, memory_order_relaxed);
        atomic_store_explicit(&s_slot[i].reanchor_req, false, memory_order_relaxed);
        s_slot[i].playing = true;
        s_slot[i].level   = 100;
        s_slot[i].mode    = CLIP_TEMPO_STRETCH;
        s_slot[i].osc0    = 0xFFFF;
    }
    s_bus_active = false;
    s_last_bar   = 0;
}

clip_tempo_mode_t clip_player_get_tempo_mode(uint8_t slot)
{
    return slot < CLIP_SLOT_COUNT ? s_slot[slot].mode : CLIP_TEMPO_STRETCH;
}

void clip_player_set_tempo_mode(uint8_t slot, clip_tempo_mode_t mode)
{
    if (slot >= CLIP_SLOT_COUNT) return;
    clip_slot_t *s = &s_slot[slot];
    if (s->mode == mode) return;
    s->mode = mode;
    if (clip_player_slot_state(slot) != CLIP_SLOT_LOADED || !s->started) return;
    /* Leaving VARI: the rate goes back to exact at once; the stretch regime
     * (if the tempo differs) starts at the bar line. Entering VARI: the
     * retrigger drops the stretcher and the rate applies from the bar. */
    retrigger_next_bar(slot);
}

void clip_player_on_tempo_change(uint16_t bpm)
{
    uint32_t upt_now = us_per_tick_for_bpm(bpm);
    for (uint8_t i = 0; i < CLIP_SLOT_COUNT; i++) {
        clip_slot_t *s = &s_slot[i];
        if (clip_player_slot_state(i) != CLIP_SLOT_LOADED || !s->started) continue;
        if (s->mode == CLIP_TEMPO_VARI) {
            push_vari_rate(i, upt_now);
        } else {
            retrigger_next_bar(i);
        }
    }
}

clip_slot_state_t clip_player_slot_state(uint8_t slot)
{
    if (slot >= CLIP_SLOT_COUNT) return CLIP_SLOT_EMPTY;
    return atomic_load_explicit(&s_slot[slot].state, memory_order_acquire);
}

uint32_t clip_player_pool_used(void)
{
    uint32_t used = 0;
    for (uint8_t i = 0; i < CLIP_SLOT_COUNT; i++) {
        if (clip_player_slot_state(i) != CLIP_SLOT_EMPTY) used += s_slot[i].bytes;
    }
    return used;
}

bool clip_player_get_playing(uint8_t slot)
{
    return slot < CLIP_SLOT_COUNT ? s_slot[slot].playing : false;
}

void clip_player_set_playing(uint8_t slot, bool on)
{
    if (slot >= CLIP_SLOT_COUNT) return;
    clip_slot_t *s = &s_slot[slot];
    if (s->playing == on) return;
    s->playing = on;
    if (clip_player_slot_state(slot) != CLIP_SLOT_LOADED) return;
    if (on && !s->started) {
        /* Never started since the last reset (unmuted after a transport
         * start): join at the next bar line, in grid position. Nothing to do
         * while stopped; the transport start schedules it. */
        uint32_t tick = sequencer_core_next_bar_tick();
        if (tick == 0) return;
        configure_oscs(slot);
        schedule_start_at(slot, tick, false);
        return;
    }
    push_gain(slot);
}

uint8_t clip_player_get_level(uint8_t slot)
{
    return slot < CLIP_SLOT_COUNT ? s_slot[slot].level : 0;
}

void clip_player_set_level(uint8_t slot, uint8_t pct)
{
    if (slot >= CLIP_SLOT_COUNT) return;
    if (pct > 100) pct = 100;
    clip_slot_t *s = &s_slot[slot];
    if (s->level == pct) return;
    s->level = pct;
    if (clip_player_slot_state(slot) == CLIP_SLOT_LOADED) push_gain(slot);
}

static void slot_release(uint8_t slot)
{
    clip_slot_t *s = &s_slot[slot];
    if (clip_player_slot_state(slot) != CLIP_SLOT_LOADED) return;
    cancel_start(slot);
    reset_oscs(slot);
    s->started = false;
    s->osc0    = 0xFFFF;
    atomic_store_explicit(&s->reanchor_req, false, memory_order_relaxed);
    atomic_store_explicit(&s->unload_countdown, CLIP_UNLOAD_DEFER_BLOCKS, memory_order_relaxed);
    atomic_store_explicit(&s->state, CLIP_SLOT_CLEARING, memory_order_release);
}

void clip_player_clear(uint8_t slot)
{
    if (slot >= CLIP_SLOT_COUNT) return;
    if (clip_bounce_get_state() != CLIP_BOUNCE_IDLE && clip_bounce_slot() == slot) {
        clip_bounce_cancel();   /* releases the slot itself */
        return;
    }
    slot_release(slot);
}

void clip_player_on_transport(bool playing)
{
    if (!playing) {
        clip_bounce_cancel();
        for (uint8_t i = 0; i < CLIP_SLOT_COUNT; i++) {
            if (clip_player_slot_state(i) != CLIP_SLOT_LOADED) continue;
            cancel_start(i);
            reset_oscs(i);
            s_slot[i].started = false;
        }
        return;
    }
    uint32_t tick = sequencer_core_next_bar_tick();
    if (tick == 0) return;
    for (uint8_t i = 0; i < CLIP_SLOT_COUNT; i++) {
        if (clip_player_slot_state(i) != CLIP_SLOT_LOADED) continue;
        if (!s_slot[i].playing) continue;
        configure_oscs(i);
        schedule_start_at(i, tick, false);
    }
}

uint8_t clip_player_first_empty_slot(void)
{
    for (uint8_t i = 0; i < CLIP_SLOT_COUNT; i++) {
        if (clip_player_slot_state(i) == CLIP_SLOT_EMPTY) return i;
    }
    return 0xFF;
}

bool clip_player_slot_trim(uint8_t slot, uint8_t bars, uint32_t alloc_frames, uint32_t loop_frames)
{
    if (slot >= CLIP_SLOT_COUNT) return false;
    if (clip_player_slot_state(slot) != CLIP_SLOT_LOADED) return false;
    clip_slot_t *s = &s_slot[slot];
    uint8_t channels = s->stereo ? 2u : 1u;
    amy_grab_lock();
    bool ok = pcm_shrink_preset(slot_preset(slot), alloc_frames);
    amy_release_lock();
    if (!ok) return false;
    s->bytes       = alloc_frames * channels * (uint32_t)sizeof(int16_t);
    s->loop_frames = loop_frames;
    s->bars        = bars;
    s->clip_ticks  = (uint32_t)bars * SEQ_TICKS_PER_BAR;
    return true;
}

void clip_player_service(void)
{
    for (uint8_t i = 0; i < CLIP_SLOT_COUNT; i++) {
        clip_slot_t *s = &s_slot[i];
        if (!atomic_load_explicit(&s->reanchor_req, memory_order_acquire)) continue;
        atomic_store_explicit(&s->reanchor_req, false, memory_order_relaxed);
        if (clip_player_slot_state(i) != CLIP_SLOT_LOADED || !s->started) continue;
        ESP_LOGI(TAG, "slot %u: re-anchor", (unsigned)i);
        retrigger_next_bar(i);
    }

    if (!s_bus_active) return;
    for (uint8_t i = 0; i < CLIP_SLOT_COUNT; i++) {
        if (clip_player_slot_state(i) != CLIP_SLOT_EMPTY) return;
    }
    s_bus_active = false;
    fx_bus_set_clips_loaded(false);
    fx_bus_sync(FX_BUS_CLIPS);
}

void clip_player_bar_check(void)
{
    uint32_t now = amy_global.sequencer_tick_count;
    uint32_t bar = now / SEQ_TICKS_PER_BAR;
    if (bar == s_last_bar) return;
    s_last_bar = bar;

    for (uint8_t i = 0; i < CLIP_SLOT_COUNT; i++) {
        clip_slot_t *s = &s_slot[i];
        if (atomic_load_explicit(&s->state, memory_order_acquire) != CLIP_SLOT_LOADED) continue;
        if (!s->started || !s->playing || s->clip_ticks == 0) continue;
        if ((int32_t)(now - s->retrig_tick) < 0) continue;   /* a start is still pending */
        if (atomic_load_explicit(&s->reanchor_req, memory_order_relaxed)) continue;

        if (s->osc0 == 0xFFFF) {
            uint16_t voices[MAX_VOICES_PER_INSTRUMENT];
            uint16_t base = 0;
            if (instrument_get_num_voices(slot_synth(i), voices) < 1) continue;
            if (!amy_voice_base_osc(voices[0], &base)) continue;
            s->osc0 = base;
        }
        uint32_t expected = (uint32_t)(((uint64_t)((now - s->anchor_tick) % s->clip_ticks)
                                        * s->loop_frames) / s->clip_ticks);
        uint32_t actual = pcm_osc_frame(s->osc0);
        if (actual >= s->loop_frames) actual -= s->loop_frames;
        uint32_t d = (actual > expected) ? actual - expected : expected - actual;
        if (d > s->loop_frames - d) d = s->loop_frames - d;   /* around the seam */
        if (d > CLIP_DRIFT_TOLERANCE_FRAMES) {
            atomic_store_explicit(&s->reanchor_req, true, memory_order_release);
        }
    }
}

int16_t *clip_player_slot_alloc(uint8_t slot, uint8_t bars, bool stereo,
                                uint32_t alloc_frames, uint32_t loop_frames,
                                uint32_t rec_us_per_tick)
{
    if (slot >= CLIP_SLOT_COUNT) return NULL;
    if (clip_player_slot_state(slot) != CLIP_SLOT_EMPTY) return NULL;
    clip_slot_t *s = &s_slot[slot];
    uint8_t channels = stereo ? 2u : 1u;

    /* pcm_load() mutates pcm.c's unlocked memory-preset list, which
     * render_pcm() walks inside the render body. Take the same lock
     * add_delta_to_queue() uses. */
    amy_grab_lock();
    int16_t *buf = pcm_load(slot_preset(slot), alloc_frames, AMY_SAMPLE_RATE, channels,
                            60.0f, 0, loop_frames);
    if (buf != NULL) {
        memset(buf, 0, (size_t)alloc_frames * channels * sizeof(int16_t));
    }
    amy_release_lock();
    if (buf == NULL) {
        ESP_LOGW(TAG, "slot %u: no RAM for %lu frames x%u", (unsigned)slot,
                 (unsigned long)alloc_frames, (unsigned)channels);
        return NULL;
    }

    s->bars            = bars;
    s->stereo          = stereo;
    s->bytes           = alloc_frames * channels * (uint32_t)sizeof(int16_t);
    s->loop_frames     = loop_frames;
    s->clip_ticks      = (uint32_t)bars * SEQ_TICKS_PER_BAR;
    s->rec_us_per_tick = rec_us_per_tick;
    s->started         = false;
    s->osc0            = 0xFFFF;
    atomic_store_explicit(&s->reanchor_req, false, memory_order_relaxed);
    atomic_store_explicit(&s->state, CLIP_SLOT_LOADED, memory_order_release);

    define_instrument(slot);
    configure_oscs(slot);
    if (!s_bus_active) {
        s_bus_active = true;
        fx_bus_set_clips_loaded(true);
        fx_bus_sync(FX_BUS_CLIPS);
    }
    ESP_LOGI(TAG, "slot %u: %u bars %s, %lu KB", (unsigned)slot, (unsigned)bars,
             stereo ? "stereo" : "mono", (unsigned long)(s->bytes / 1024u));
    return buf;
}

void clip_player_render_service(void)
{
    for (uint8_t i = 0; i < CLIP_SLOT_COUNT; i++) {
        clip_slot_t *s = &s_slot[i];
        if (atomic_load_explicit(&s->state, memory_order_acquire) != CLIP_SLOT_CLEARING) continue;
        uint8_t c = atomic_load_explicit(&s->unload_countdown, memory_order_relaxed);
        if (c > 0) {
            atomic_store_explicit(&s->unload_countdown, (uint8_t)(c - 1), memory_order_relaxed);
            continue;
        }
        amy_grab_lock();
        pcm_unload_preset(slot_preset(i));
        amy_release_lock();
        s->bytes = 0;
        atomic_store_explicit(&s->state, CLIP_SLOT_EMPTY, memory_order_release);
    }
}
