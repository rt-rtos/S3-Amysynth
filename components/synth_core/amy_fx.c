/* amy_fx.c - cached state for AMY's effect buses (EQ, echo, chorus, reverb,
 * distortion) plus the master output volume and the per-bus trims. Kept out of
 * synth_ui.c so the sequencer/arp/drone cores can call synth_ui_fx_reassert()
 * without depending on the UI headers (u8g2, display_*).
 *
 * One cache per bus; fx_bus.c decides which synths feed which bus. A bus that
 * carries nothing is held muted in the engine regardless of what its cache
 * holds, so an unused bus costs no render time. */

#include "amy_fx.h"
#include "fx_bus.h"
#include "amy.h"
#include "amy_helpers.h"
#include "sdkconfig.h"
#include "seq_clamp.h"

/* ── Default initialisation guards ─────────────────────────────────────── */
#ifndef CONFIG_SEQ_FX_DEFAULT_ECHO
#define CONFIG_SEQ_FX_DEFAULT_ECHO    0
#endif
#ifndef CONFIG_SEQ_FX_DEFAULT_REVERB
#define CONFIG_SEQ_FX_DEFAULT_REVERB  0
#endif
#ifndef CONFIG_SEQ_FX_DEFAULT_CHORUS
#define CONFIG_SEQ_FX_DEFAULT_CHORUS  0
#endif

/* ── FX state ───────────────────────────────────────────────────────────── */

/* The silent bus: every level at zero, every sentinel unset, distortion off.
 * Serves double duty as the boot state of the buses beyond 0 and as the shape
 * pushed to any bus that is not currently carrying a group. */
#define FX_BUS_EMPTY_INIT { \
    .eq_low_db  = 0, \
    .eq_mid_db  = 0, \
    .eq_high_db = 0, \
    .echo_level   = 0, \
    .chorus_level = 0, \
    .reverb_level = 0, \
    .echo_delay_ms   = FX_PARAM_UNSET, \
    .echo_feedback   = FX_PARAM_UNSET, \
    .echo_tone       = FX_PARAM_UNSET, \
    .reverb_liveness = FX_PARAM_UNSET, \
    .reverb_damping  = FX_PARAM_UNSET, \
    .reverb_xover_hz = FX_PARAM_UNSET, \
    .chorus_rate     = FX_PARAM_UNSET, \
    .chorus_depth    = FX_PARAM_UNSET, \
    .chorus_delay    = FX_PARAM_UNSET, \
    .bus_dist_type   = 0, \
    .bus_dist_drive  = 1, \
    .bus_dist_bits   = 16, \
    .bus_dist_rate   = 1, \
    .bus_dist_mix    = 0, \
    .level           = 100, \
}

fx_state_t s_fx[FX_BUS_COUNT] = {
    /* Bus 0 is where everything renders until a group is split off, so it
     * carries the configured boot FX. */
    [FX_BUS_HOME] = {
        .eq_low_db  = 0,
        .eq_mid_db  = 0,
        .eq_high_db = 0,
        .echo_level          = CONFIG_SEQ_FX_DEFAULT_ECHO,
        .chorus_level        = CONFIG_SEQ_FX_DEFAULT_CHORUS,
        .reverb_level        = CONFIG_SEQ_FX_DEFAULT_REVERB,
        /* Extended params start UNSET so AMY keeps its factory character until
         * edited. Load-bearing: designated-init would zero these, and 0
         * liveness/damping silently mangles the reverb. */
        .echo_delay_ms   = FX_PARAM_UNSET,
        .echo_feedback   = FX_PARAM_UNSET,
        .echo_tone       = FX_PARAM_UNSET,
        .reverb_liveness = FX_PARAM_UNSET,
        .reverb_damping  = FX_PARAM_UNSET,
        .reverb_xover_hz = FX_PARAM_UNSET,
        .chorus_rate     = FX_PARAM_UNSET,
        .chorus_depth    = FX_PARAM_UNSET,
        .chorus_delay    = FX_PARAM_UNSET,
        /* Stage off, unity drive, transparent crusher (mirrors bus_reset()).
         * Mix starts dry, diverging from the engine's wet default: at bus
         * scope the wet amount is small and dialed in deliberately. */
        .bus_dist_type   = 0,
        .bus_dist_drive  = 1,
        .bus_dist_bits   = 16,
        .bus_dist_rate   = 1,
        .bus_dist_mix    = 0,
        .level           = 100,
    },
    [1] = FX_BUS_EMPTY_INIT,
    [2] = FX_BUS_EMPTY_INIT,
    [3] = FX_BUS_EMPTY_INIT,
};

bool s_fx_presets_alter_global = false;

static const fx_state_t s_fx_muted = FX_BUS_EMPTY_INIT;

/* What the engine must be holding for this bus: its cache while the bus
 * carries a group, the silent shape otherwise. */
static const fx_state_t *fx_engine_state(uint8_t bus)
{
    return fx_bus_is_active(bus) ? &s_fx[bus] : &s_fx_muted;
}

/* ── Master volume ──────────────────────────────────────────────────────── */
/* Range 0..2.0, unity=1.0. Matches AMY's own init (amy_start sets
 * amy_global.volume[bus]=1.0f), so no push is needed at boot. */
static float s_master_volume = 1.0f;

/* What amy_global.volume[bus] holds: master x trim, times the clip bus's
 * makeup (FX_BUS_CLIPS_MAKEUP). Direct write: an aligned float store, atomic
 * on Xtensa. UI task, never the render body. */
static void push_volume(uint8_t bus)
{
    if (bus >= amy_global.config.max_buses) return;
    float v = s_master_volume * (float)s_fx[bus].level / 100.0f;
    if (bus == FX_BUS_CLIPS) v *= FX_BUS_CLIPS_MAKEUP;
    amy_global.volume[bus] = v;
}

/* ── FX push helpers ────────────────────────────────────────────────────── */
void fx_push_eq(uint8_t bus)
{
    const fx_state_t *f = fx_engine_state(bus);
    amy_event *e = amy_helpers_event_begin();
    e->bus  = bus;
    e->eq_l = (float)f->eq_low_db;   /* AMY interprets these as dB */
    e->eq_m = (float)f->eq_mid_db;
    e->eq_h = (float)f->eq_high_db;
    amy_helpers_event_send(e);
}

void fx_push_echo(uint8_t bus)
{
    const fx_state_t *f = fx_engine_state(bus);
    amy_event *e = amy_helpers_event_begin();
    e->bus        = bus;
    e->echo_level = (float)f->echo_level / 100.0f;
    /* Only send sub-params the user has set; unset ones stay AMY_UNSET so
     * config_echo keeps the bus's current value. */
    if (f->echo_delay_ms != FX_PARAM_UNSET)
        e->echo_delay_ms   = (float)f->echo_delay_ms;
    if (f->echo_feedback != FX_PARAM_UNSET)
        e->echo_feedback   = (float)f->echo_feedback / 100.0f;
    if (f->echo_tone != FX_PARAM_UNSET)
        e->echo_filter_coef = (float)f->echo_tone / 100.0f;
    amy_helpers_event_send(e);
}

void fx_push_chorus(uint8_t bus)
{
    const fx_state_t *f = fx_engine_state(bus);
    amy_event *e = amy_helpers_event_begin();
    e->bus          = bus;
    e->chorus_level = (float)f->chorus_level / 100.0f;
    if (f->chorus_rate != FX_PARAM_UNSET)
        e->chorus_lfo_freq = (float)f->chorus_rate / 100.0f;   /* centi-Hz -> Hz */
    if (f->chorus_depth != FX_PARAM_UNSET)
        e->chorus_depth    = (float)f->chorus_depth / 100.0f;
    if (f->chorus_delay != FX_PARAM_UNSET)
        e->chorus_max_delay = (float)f->chorus_delay;
    amy_helpers_event_send(e);
}

void fx_push_reverb(uint8_t bus)
{
    const fx_state_t *f = fx_engine_state(bus);
    amy_event *e = amy_helpers_event_begin();
    e->bus          = bus;
    e->reverb_level = (float)f->reverb_level / 100.0f;
    if (f->reverb_liveness != FX_PARAM_UNSET)
        e->reverb_liveness = (float)f->reverb_liveness / 100.0f;
    if (f->reverb_damping != FX_PARAM_UNSET)
        e->reverb_damping  = (float)f->reverb_damping / 100.0f;
    if (f->reverb_xover_hz != FX_PARAM_UNSET)
        e->reverb_xover_hz = (float)f->reverb_xover_hz;
    amy_helpers_event_send(e);
}

void fx_push_dist(uint8_t bus)
{
    /* One set of dist fields serves both scopes; the event decides which.
     * Naming no osc puts them at bus scope, and the named bus is the one they
     * land on. Naming a synth would instead fan them out over its voices. */
    const fx_state_t *f = fx_engine_state(bus);
    amy_event *e = amy_helpers_event_begin();
    e->bus = bus;
    /* bus_dist_type is a stage mask; author all three enables explicitly
     * so a mask change turns dropped stages off. */
    e->dist_clip  = !!(f->bus_dist_type & 1u);
    e->dist_fold  = !!(f->bus_dist_type & 2u);
    e->dist_crush = !!(f->bus_dist_type & 4u);
    e->dist_bits  = (uint8_t)f->bus_dist_bits;
    e->dist_rate  = (uint16_t)f->bus_dist_rate;
    /* A bus sum has no per-note modulation sources, so only the CONST coef
     * of each rail reaches it: drive linear 1..16, mix linear 0..1. */
    e->dist_drive_coefs[COEF_CONST] = (float)f->bus_dist_drive;
    e->dist_mix_coefs[COEF_CONST]   = (float)f->bus_dist_mix / 100.0f;
    amy_helpers_event_send(e);
}

void fx_bus_sync(uint8_t bus)
{
    fx_push_eq(bus);
    fx_push_chorus(bus);
    fx_push_echo(bus);
    fx_push_reverb(bus);
    fx_push_dist(bus);
    /* AMY boots every bus at 1.0; the clip bus's makeup has to be in place
     * before its first clip sounds. */
    push_volume(bus);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Move a group's existing synths onto whatever bus the routing table now names.
 * Slots with no instrument behind them are skipped: a bare synth event would
 * otherwise create one. The event names the bus explicitly, so the ingress
 * routing hook leaves it alone. */
static void fx_retag_group(fx_group_t g)
{
    uint8_t slots[8];
    uint8_t n   = fx_group_slots(g, slots);
    uint8_t bus = fx_bus_of_group(g);
    for (uint8_t i = 0; i < n; i++) {
        if (!instrument_number_exists(slots[i], NULL)) continue;
        amy_event *e = amy_helpers_event_begin();
        e->synth = slots[i];
        e->bus   = bus;
        amy_helpers_config_send(e);
    }
}

void amy_fx_set_bus_split(fx_group_t g, bool on)
{
    if (!fx_bus_set_split(g, on)) return;
    fx_retag_group(g);
    /* Sync the group's OWN bus either way: splitting arms its cached FX,
     * folding back mutes it. Bus 0 keeps whatever the user dialed in. */
    fx_bus_sync(fx_group_own_bus(g));
}

void amy_fx_apply_routing(void)
{
    static const fx_group_t movable[] = {
        FX_GROUP_DRUMS, FX_GROUP_DRONES, FX_GROUP_CLIPS
    };
    for (unsigned i = 0; i < sizeof movable / sizeof movable[0]; i++) {
        fx_retag_group(movable[i]);
    }
    for (uint8_t b = 0; b < FX_BUS_COUNT; b++) fx_bus_sync(b);
}

/* Re-impose a bus's cached FX after a patch load so a preset cannot hijack the
 * EQ/chorus/echo/reverb of the bus the loading synth renders on. No-op when the
 * user opted into letting presets drive bus FX. Safe from the sequencer/arp/
 * drone task contexts: each fx_push_* serialises through the shared
 * amy_helpers mutex.
 *
 * These events are queued AFTER the patch's own FX deltas, so they win, and
 * both drain in the same render quantum - no audible blip. */
void synth_ui_fx_reassert(uint8_t slot)
{
    if (s_fx_presets_alter_global) return;
    uint8_t bus = fx_bus_for_synth(slot);
    fx_push_eq(bus);
    fx_push_chorus(bus);
    fx_push_echo(bus);
    fx_push_reverb(bus);
}

void synth_ui_fx_reassert_all(void)
{
    if (s_fx_presets_alter_global) return;
    for (uint8_t b = 0; b < FX_BUS_COUNT; b++) {
        fx_push_eq(b);
        fx_push_chorus(b);
        fx_push_echo(b);
        fx_push_reverb(b);
    }
}

void amy_fx_set_master_volume(float v)
{
    v = SEQ_CLAMP_F32(v, 0.0f, 2.0f);
    s_master_volume = v;
    for (uint8_t b = 0; b < FX_BUS_COUNT; b++) push_volume(b);
}

float amy_fx_get_master_volume(void)
{
    return s_master_volume;
}

void amy_fx_set_bus_level(uint8_t bus, uint8_t pct)
{
    if (bus >= FX_BUS_COUNT) return;
    s_fx[bus].level = (uint8_t)SEQ_CLAMP_INT((int)pct, 0, 200);
    push_volume(bus);
}
