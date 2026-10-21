#include "transport_stop.h"
#include "synth_level.h"
#include "synth_slots.h"
#include "mixer.h"
#include "amy_fx.h"
#include "fx_bus.h"
#include "amy_helpers.h"
#include "sequencer_core.h"
#include "seq_core_config.h"   /* SEQ_TICKS_PER_BAR */
#include "sequencer.h"         /* sequencer_ticks */
#include "custompatches/clip_bounce.h"
#include "synth_ui/synth_ui_internal.h"   /* seq_state.playing, the UI play mirror */
#include "esp_log.h"

/* State machine: contract in transport_stop.h. */

static const char *TAG = "transport_stop";

/* Cut ramp: TSTOP_RAMP_WAKES wakes of -6 dB each (about 3 dB per render
 * block), the last one exactly 0. The recovery ramp climbs at the same slope
 * from TSTOP_RAMP_FLOOR (-60 dB). */
#define TSTOP_RAMP_WAKES  10u
#define TSTOP_RAMP_DOWN   0.5011872f
#define TSTOP_RAMP_UP     1.9952623f
#define TSTOP_RAMP_FLOOR  0.001f

typedef enum { TS_IDLE = 0, TS_ARMED, TS_CUT_RAMP } ts_phase_t;

static const uint32_t s_fade_ticks[TSTOP_FADE_COUNT] = {
    AMY_SEQUENCER_PPQ, SEQ_TICKS_PER_BAR, 2u * SEQ_TICKS_PER_BAR, 4u * SEQ_TICKS_PER_BAR,
};

static uint8_t    s_mode     = TSTOP_RING;
static uint8_t    s_fade_idx = 1;

static ts_phase_t s_phase;
static uint8_t    s_armed_mode;   /* the mode the arm was taken in */
static uint32_t   s_q;            /* FADE: first tick of the ramp */
static uint32_t   s_x;            /* the stop tick */
static bool       s_own;          /* the periodic end horizon is ours */
static bool       s_recovering;   /* factor climbing back to 1 */
static bool       s_pending_play; /* Play pressed during the cut ramp */
static uint8_t    s_ramp_n;       /* cut-ramp wakes done */
static float      s_factor = 1.0f;

void transport_stop_defaults(transport_stop_params_t *out)
{
    out->mode     = TSTOP_RING;
    out->fade_idx = 1;
}

tstop_mode_t transport_stop_get_mode(void) { return (tstop_mode_t)s_mode; }

void transport_stop_set_mode(uint8_t mode)
{
    s_mode = (mode < TSTOP_MODE_COUNT) ? mode : (uint8_t)(TSTOP_MODE_COUNT - 1);
}

uint8_t transport_stop_get_fade_idx(void) { return s_fade_idx; }

void transport_stop_set_fade_idx(uint8_t idx)
{
    s_fade_idx = (idx < TSTOP_FADE_COUNT) ? idx : (uint8_t)(TSTOP_FADE_COUNT - 1);
}

uint32_t transport_stop_fade_ticks(uint8_t idx)
{
    return s_fade_ticks[(idx < TSTOP_FADE_COUNT) ? idx : TSTOP_FADE_COUNT - 1];
}

bool transport_stop_is_armed(void) { return s_phase == TS_ARMED; }

static inline bool tick_reached(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) >= 0;
}

static bool sequenced_present(uint8_t synth)
{
    /* The drop voices have no mixer source and are configured from boot. */
    if (synth == DROP_SYNTH || synth == DROP_SUB_SYNTH) return true;
    return mixer_synth_present(synth);
}

/* fn on every present sequenced synth. */
static void for_each_sequenced(void (*fn)(uint8_t))
{
    static const uint8_t fixed[] = {
        SEQ_ARP_SYNTH, DROP_SYNTH, DROP_SUB_SYNTH,
        DRONE_SYNTH_MAIN, DRONE_SYNTH_SUB, DRONE_STD_SYNTH_MAIN, DRONE_STD_SYNTH_SUB,
    };
    uint8_t n = sequencer_core_get_num_layers();
    if (n > MAX_LAYERS) n = MAX_LAYERS;
    for (uint8_t L = 0; L < n; L++) {
        uint8_t tracks = sequencer_core_get_layer_tracks(L);
        if (tracks > SEQ_TRACKS) tracks = SEQ_TRACKS;
        for (uint8_t t = 0; t < tracks; t++) {
            uint8_t s = sequencer_core_get_track_synth(L, t);
            if (s >= SYNTH_SLOT_COUNT || s == LIVE_SYNTH || !sequenced_present(s)) continue;
            fn(s);
        }
    }
    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
        if (sequenced_present(fixed[i])) fn(fixed[i]);
    }
    for (uint8_t c = 0; c < CLIP_SLOT_COUNT; c++) {
        uint8_t s = (uint8_t)(CLIP_SYNTH_BASE + c);
        if (sequenced_present(s)) fn(s);
    }
}

static void send_cut(uint8_t synth)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth     = synth;
    e->reset_osc = RESET_NOTE_CUT;
    amy_helpers_config_send(e);
}

/* New transport factor, reasserted on every present sequenced synth when it
 * changed. */
static void set_factor(float f)
{
    if (f == s_factor) return;
    s_factor = f;
    synth_level_set_transport(f);
    for_each_sequenced(synth_level_reassert);
}

static void begin_recovery(void)
{
    if (s_factor < 1.0f) s_recovering = true;
}

static void plain_stop(void)
{
    seq_state.playing = false;
    sequencer_core_set_playing(false);
}

static void plain_start(void)
{
    seq_state.playing = true;
    sequencer_core_set_playing(true);
}

static void release_horizon(void)
{
    if (!s_own) return;
    sequencer_core_freeze_clear();
    s_own = false;
}

/* One wake of the cut ramp. Wake TSTOP_RAMP_WAKES lands the factor at 0 and
 * hard-stops; the factor returns to 1 only on the wake after. synth_level
 * applies at ingest while the hard stop runs at the next block's deltas on
 * the render core, so a 1 sent in the same burst could reach a block that
 * still holds the released notes. The next wake also starts the drains and
 * runs a pending Play. */
static void ramp_step(void)
{
    s_ramp_n++;
    if (s_ramp_n < TSTOP_RAMP_WAKES) {
        set_factor(s_factor * TSTOP_RAMP_DOWN);
        return;
    }
    if (s_ramp_n == TSTOP_RAMP_WAKES) {
        set_factor(0.0f);
        for_each_sequenced(send_cut);
        return;
    }
    set_factor(1.0f);
    amy_fx_drain_begin(FX_BUS_HOME);
    for (uint8_t bus = 0; bus < FX_BUS_COUNT; bus++) {
        if (bus != FX_BUS_HOME && fx_bus_is_active(bus)) amy_fx_drain_begin(bus);
    }
    s_phase = TS_IDLE;
    if (s_pending_play) {
        s_pending_play = false;
        plain_start();
    }
}

static void cut_begin(void)
{
    plain_stop();
    release_horizon();
    s_phase      = TS_CUT_RAMP;
    s_recovering = false;
    s_ramp_n     = 0;
    ESP_LOGI(TAG, "cut at tick %u", (unsigned)sequencer_ticks());
    ramp_step();
}

/* Back to playing: the horizon released, the factor climbing back to 1. */
static void cancel_arm(void)
{
    release_horizon();
    s_phase = TS_IDLE;
    begin_recovery();
}

void transport_stop_press(void)
{
    if (s_phase == TS_CUT_RAMP) return;
    if (s_phase == TS_ARMED) {
        cancel_arm();
        return;
    }
    uint8_t m = s_mode;
    if (m == TSTOP_RING || clip_bounce_get_state() != CLIP_BOUNCE_IDLE) {
        plain_stop();
        return;
    }
    uint32_t now = sequencer_ticks();
    s_q = (now / AMY_SEQUENCER_PPQ + 1u) * AMY_SEQUENCER_PPQ;
    s_x = (m == TSTOP_CUT) ? now
        : (m == TSTOP_CUT_QUARTER) ? s_q
        : s_q + transport_stop_fade_ticks(s_fade_idx);
    s_armed_mode = m;
    sequencer_core_freeze_set(s_x);
    s_own   = true;
    s_phase = TS_ARMED;
    if (m == TSTOP_CUT) {
        cut_begin();
        return;
    }
    ESP_LOGI(TAG, "stop armed: mode %u, tick %u -> %u", (unsigned)m, (unsigned)now, (unsigned)s_x);
}

void transport_stop_play_press(void)
{
    switch (s_phase) {
        case TS_ARMED:
            cancel_arm();
            return;
        case TS_CUT_RAMP:
            s_pending_play = true;
            return;
        default:
            plain_start();
            return;
    }
}

void transport_stop_service(void)
{
    if (s_phase == TS_IDLE && !s_recovering) return;
    if (s_phase == TS_CUT_RAMP) {
        ramp_step();
        return;
    }
    uint32_t now = sequencer_ticks();
    if (s_phase == TS_ARMED) {
        if (clip_bounce_get_state() != CLIP_BOUNCE_IDLE) {
            /* The bounce set its own horizon over ours: leave it. */
            s_own   = false;
            s_phase = TS_IDLE;
            begin_recovery();
        } else if (tick_reached(now, s_x)) {
            cut_begin();
            return;
        }
    }
    float target = 1.0f;
    if (s_phase == TS_ARMED && s_armed_mode == TSTOP_FADE && tick_reached(now, s_q)) {
        bool done;
        target = mixer_pos_gain(mixer_fade_pos((uint8_t)MIXER_POS_MAX, 0u, s_q, s_x - s_q, now, &done));
    }
    if (s_recovering) {
        float r = (s_factor < TSTOP_RAMP_FLOOR) ? TSTOP_RAMP_FLOOR : s_factor * TSTOP_RAMP_UP;
        if (r >= target) s_recovering = false;
        else             target = r;
    }
    set_factor(target);
}

void transport_stop_reset(void)
{
    release_horizon();
    s_phase        = TS_IDLE;
    s_recovering   = false;
    s_pending_play = false;
    set_factor(1.0f);
}
