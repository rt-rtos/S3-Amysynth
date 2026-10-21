#include "mixer.h"
#include "synth_level.h"
#include "synth_slots.h"
#include "sequencer_core.h"
#include "amy_fx.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"
#include "custompatches/clip_player.h"
#include "seq_core_config.h"   /* SEQ_TICKS_PER_BAR */
#include "sequencer.h"         /* sequencer_ticks */
#include "sdkconfig.h"
#if CONFIG_SYNTH_WIRELESS
#include "live_play.h"
#endif
#include <math.h>
#include <string.h>

/* Model and frame service: contract in mixer.h. */

_Static_assert(CLIP_SLOT_COUNT == MIX_MISC_CLIP1 - MIX_MISC_CLIP0 + 1, "one misc entry per clip slot");
_Static_assert(MAX_LAYERS <= 8 && SEQ_TRACKS <= 8 && MIX_MISC_COUNT <= 8, "mute masks are 8-bit");

/* Sub-unity present synths re-sent per frame (patch-load reset recovery). */
#define MIXER_REASSERT_PER_FRAME 8u

#define LAYER_MASK  ((uint8_t)((1u << MAX_LAYERS) - 1u))
#define TRACK_MASK  ((uint8_t)((1u << SEQ_TRACKS) - 1u))
#define MISC_MASK   ((uint8_t)((1u << MIX_MISC_COUNT) - 1u))
#define SYNTH_BYTES ((SYNTH_SLOT_COUNT + 7) / 8)

static mixer_snapshot_t s_mix;

/* Fades, one slot per fadeable source (fade_index()): master, the layers,
 * their tracks, the misc sources. restore_pos is where the last fade-out
 * started, the target of the next fade-in. s_link is the slot's link group,
 * kept apart from the fade runtime that fade_clear*() resets. */
#define FADE_LAYER0  1
#define FADE_TRACK0  (FADE_LAYER0 + MAX_LAYERS)
#define FADE_MISC0   (FADE_TRACK0 + MAX_LAYERS * SEQ_TRACKS)
#define FADE_SLOTS   (FADE_MISC0 + MIX_MISC_COUNT)

typedef struct {
    uint8_t  active;
    uint8_t  from_pos, to_pos;
    uint32_t start_tick;     /* first tick of the ramp */
    uint32_t len_ticks;      /* bars * SEQ_TICKS_PER_BAR */
} mix_fade_t;

static const uint8_t s_fade_bars[MIXER_FADE_BARS_COUNT] = { 1, 2, 4, 8, 16, 32 };
static mix_fade_t s_fade[FADE_SLOTS];
static uint8_t    s_restore_pos[FADE_SLOTS];
static uint8_t    s_link[FADE_SLOTS];

/* Sources whose synths go out on the next frame. */
static uint8_t s_layer_dirty;
static uint8_t s_track_dirty[MAX_LAYERS];
static uint8_t s_misc_dirty;

/* Per synth: present last frame, and the mixer factor last handed to
 * synth_level (1.0 = AMY's own reset value). */
static uint8_t  s_present_last[SYNTH_BYTES];
static float    s_sent[SYNTH_SLOT_COUNT];
static uint16_t s_rr;   /* reassert cursor, a synth number */

/* Frame scratch, rebuilt by every mixer_service(). */
static float   s_factor[SYNTH_SLOT_COUNT];
static uint8_t s_present[SYNTH_BYTES];
static uint8_t s_want[SYNTH_BYTES];

static inline bool bit_get(const uint8_t *m, uint16_t i) { return (m[i >> 3] >> (i & 7u)) & 1u; }
static inline void bit_set(uint8_t *m, uint16_t i)       { m[i >> 3] |= (uint8_t)(1u << (i & 7u)); }

static float gain(uint8_t pos)
{
    float g = (float)pos * 0.01f;
    return g * g;
}

static uint8_t clamp_pos(uint8_t pos)
{
    return (pos > MIXER_POS_MAX) ? (uint8_t)MIXER_POS_MAX : pos;
}

void mixer_params_defaults(mixer_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    memset(out->layer_pos, MIXER_POS_MAX, sizeof(out->layer_pos));
    memset(out->track_pos, MIXER_POS_MAX, sizeof(out->track_pos));
    memset(out->misc_pos, MIXER_POS_MAX, sizeof(out->misc_pos));
    out->fade_bars_idx = MIXER_FADE_BARS_DEFAULT;
}

/* Layer L's entries back to mixer_params_defaults(). */
static void layer_defaults(uint8_t L)
{
    mixer_snapshot_t d;
    mixer_params_defaults(&d);
    s_mix.layer_pos[L] = d.layer_pos[L];
    s_mix.layer_mute  = (uint8_t)((s_mix.layer_mute & ~(1u << L)) | (d.layer_mute & (1u << L)));
    memcpy(s_mix.track_pos[L], d.track_pos[L], sizeof(s_mix.track_pos[L]));
    s_mix.track_mute[L] = d.track_mute[L];
}

static void fade_clear(int i)
{
    s_fade[i].active = 0;
    s_restore_pos[i] = 0;
}

static void fade_clear_all(void)
{
    memset(s_fade, 0, sizeof(s_fade));
    memset(s_restore_pos, 0, sizeof(s_restore_pos));
}

/* A layer's slot and its tracks' slots. */
static void fade_clear_layer(uint8_t L)
{
    fade_clear(FADE_LAYER0 + L);
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) fade_clear(FADE_TRACK0 + L * SEQ_TRACKS + t);
}

static void link_clear_layer(uint8_t L)
{
    s_link[FADE_LAYER0 + L] = MIXER_LINK_NONE;
    memset(&s_link[FADE_TRACK0 + L * SEQ_TRACKS], MIXER_LINK_NONE, SEQ_TRACKS);
}

static void mark_all_dirty(void)
{
    s_layer_dirty = LAYER_MASK;
    s_misc_dirty  = MISC_MASK;
}

/* Misc index of a non-layer source, or MIX_MISC_COUNT. */
static uint8_t misc_index(mix_src_t s)
{
    switch (s.kind) {
        case MIX_SRC_ARP:        return MIX_MISC_ARP;
        case MIX_SRC_DRONE_STD:  return MIX_MISC_DRONE_STD;
        case MIX_SRC_DRONE_STUT: return MIX_MISC_DRONE_STUT;
        case MIX_SRC_LIVE:       return MIX_MISC_LIVE;
        case MIX_SRC_CLIP:
            return (s.a < CLIP_SLOT_COUNT) ? (uint8_t)(MIX_MISC_CLIP0 + s.a) : (uint8_t)MIX_MISC_COUNT;
        default:                 return MIX_MISC_COUNT;
    }
}

/* Fade slot of a source, -1 for a source with none. */
static int fade_index(mix_src_t s)
{
    switch (s.kind) {
        case MIX_SRC_MASTER: return 0;
        case MIX_SRC_LAYER:  return (s.a < MAX_LAYERS) ? FADE_LAYER0 + s.a : -1;
        case MIX_SRC_TRACK:
            return (s.a < MAX_LAYERS && s.b < SEQ_TRACKS) ? FADE_TRACK0 + s.a * SEQ_TRACKS + s.b : -1;
        default: {
            uint8_t k = misc_index(s);
            return (k < MIX_MISC_COUNT) ? FADE_MISC0 + k : -1;
        }
    }
}

/* The source a fade slot belongs to (fade_index() inverted). */
static mix_src_t fade_src(int i)
{
    static const uint8_t misc_kind[MIX_MISC_COUNT] = {
        MIX_SRC_ARP, MIX_SRC_DRONE_STD, MIX_SRC_DRONE_STUT, MIX_SRC_LIVE, MIX_SRC_CLIP, MIX_SRC_CLIP,
    };
    if (i == 0) return (mix_src_t){ .kind = MIX_SRC_MASTER };
    if (i < FADE_TRACK0) return (mix_src_t){ .kind = MIX_SRC_LAYER, .a = (uint8_t)(i - FADE_LAYER0) };
    if (i < FADE_MISC0) {
        int j = i - FADE_TRACK0;
        return (mix_src_t){ .kind = MIX_SRC_TRACK, .a = (uint8_t)(j / SEQ_TRACKS),
                            .b = (uint8_t)(j % SEQ_TRACKS) };
    }
    uint8_t k = (uint8_t)(i - FADE_MISC0);
    return (mix_src_t){ .kind = misc_kind[k],
                        .a = (k >= MIX_MISC_CLIP0) ? (uint8_t)(k - MIX_MISC_CLIP0) : 0u };
}

static bool live_ready(void)
{
#if CONFIG_SYNTH_WIRELESS
    return live_play_is_ready();
#else
    return false;
#endif
}

void mixer_init(void)
{
    mixer_params_defaults(&s_mix);
    fade_clear_all();
    memset(s_link, MIXER_LINK_NONE, sizeof(s_link));
    s_layer_dirty = 0;
    memset(s_track_dirty, 0, sizeof(s_track_dirty));
    s_misc_dirty = 0;
    memset(s_present_last, 0, sizeof(s_present_last));
    for (uint16_t i = 0; i < SYNTH_SLOT_COUNT; i++) s_sent[i] = 1.0f;
    s_rr = 0;
}

void mixer_reset_defaults(void)
{
    mixer_params_defaults(&s_mix);
    fade_clear_all();
    mixer_clear_links();
    mark_all_dirty();
}

bool mixer_src_present(mix_src_t s)
{
    uint8_t n = sequencer_core_get_num_layers();
    switch (s.kind) {
        case MIX_SRC_MASTER:     return true;
        case MIX_SRC_LAYER:      return s.a < n;
        case MIX_SRC_TRACK:      return s.a < n && s.b < sequencer_core_get_layer_tracks(s.a);
        case MIX_SRC_ARP:        return true;
        case MIX_SRC_DRONE_STD:  return drone_std_get_enabled();
        case MIX_SRC_DRONE_STUT: return drone_get_enabled();
        case MIX_SRC_LIVE:       return live_ready();
        case MIX_SRC_CLIP:
            return s.a < CLIP_SLOT_COUNT && clip_player_slot_state(s.a) == CLIP_SLOT_LOADED;
        default:                 return false;
    }
}

bool mixer_synth_present(uint8_t synth)
{
    /* The synth-to-source map and presence rules of mixer_service(). */
    if (synth == 0u || synth >= SYNTH_SLOT_COUNT) return false;
    uint8_t n = sequencer_core_get_num_layers();
    if (n > MAX_LAYERS) n = MAX_LAYERS;
    for (uint8_t L = 0; L < n; L++) {
        uint8_t tracks = sequencer_core_get_layer_tracks(L);
        if (tracks > SEQ_TRACKS) tracks = SEQ_TRACKS;
        for (uint8_t t = 0; t < tracks; t++) {
            if (sequencer_core_get_track_synth(L, t) == synth) return true;
        }
    }
    switch (synth) {
        case SEQ_ARP_SYNTH:        return true;
        case DRONE_STD_SYNTH_MAIN: return drone_std_get_enabled();
        case DRONE_STD_SYNTH_SUB:  return drone_std_get_enabled() && drone_std_get_sub_enabled();
        case DRONE_SYNTH_MAIN:     return drone_get_enabled();
        case DRONE_SYNTH_SUB:      return drone_get_enabled() && drone_get_sub_enabled();
        case LIVE_SYNTH:           return live_ready();
        default: break;
    }
    if (synth >= CLIP_SYNTH_BASE && synth < CLIP_SYNTH_BASE + CLIP_SLOT_COUNT) {
        return clip_player_slot_state((uint8_t)(synth - CLIP_SYNTH_BASE)) == CLIP_SLOT_LOADED;
    }
    return false;
}

uint8_t mixer_get_pos(mix_src_t s)
{
    switch (s.kind) {
        case MIX_SRC_MASTER: {
            float v = amy_fx_get_master_volume();
            if (v > 1.0f) v = 1.0f;
            if (v < 0.0f) v = 0.0f;
            return (uint8_t)(100.0f * sqrtf(v) + 0.5f);
        }
        case MIX_SRC_LAYER:
            return (s.a < MAX_LAYERS) ? s_mix.layer_pos[s.a] : (uint8_t)MIXER_POS_MAX;
        case MIX_SRC_TRACK:
            return (s.a < MAX_LAYERS && s.b < SEQ_TRACKS) ? s_mix.track_pos[s.a][s.b]
                                                          : (uint8_t)MIXER_POS_MAX;
        default: {
            uint8_t k = misc_index(s);
            return (k < MIX_MISC_COUNT) ? s_mix.misc_pos[k] : (uint8_t)MIXER_POS_MAX;
        }
    }
}

/* Position write shared by the public setter and the fade stepper; marks
 * the source dirty (master: written at once). Does not touch the fade. */
static void write_pos(mix_src_t s, uint8_t pos)
{
    pos = clamp_pos(pos);
    switch (s.kind) {
        case MIX_SRC_MASTER:
            amy_fx_set_master_volume(gain(pos));
            return;
        case MIX_SRC_LAYER:
            if (s.a >= MAX_LAYERS) return;
            s_mix.layer_pos[s.a] = pos;
            s_layer_dirty |= (uint8_t)(1u << s.a);
            return;
        case MIX_SRC_TRACK:
            if (s.a >= MAX_LAYERS || s.b >= SEQ_TRACKS) return;
            s_mix.track_pos[s.a][s.b] = pos;
            s_track_dirty[s.a] |= (uint8_t)(1u << s.b);
            return;
        default: {
            uint8_t k = misc_index(s);
            if (k >= MIX_MISC_COUNT) return;
            s_mix.misc_pos[k] = pos;
            s_misc_dirty |= (uint8_t)(1u << k);
            return;
        }
    }
}

void mixer_set_pos(mix_src_t s, uint8_t pos)
{
    int f = fade_index(s);
    if (f >= 0) s_fade[f].active = 0;   /* manual wins */
    write_pos(s, pos);
}

bool mixer_has_mute(mix_src_t s)
{
    switch (s.kind) {
        case MIX_SRC_MASTER: return false;
        case MIX_SRC_LAYER:  return s.a < MAX_LAYERS;
        case MIX_SRC_TRACK:  return s.a < MAX_LAYERS && s.b < SEQ_TRACKS;
        default:             return misc_index(s) < MIX_MISC_COUNT;
    }
}

bool mixer_get_mute(mix_src_t s)
{
    if (!mixer_has_mute(s)) return false;
    switch (s.kind) {
        case MIX_SRC_LAYER: return (s_mix.layer_mute >> s.a) & 1u;
        case MIX_SRC_TRACK: return (s_mix.track_mute[s.a] >> s.b) & 1u;
        default:            return (s_mix.misc_mute >> misc_index(s)) & 1u;
    }
}

void mixer_set_mute(mix_src_t s, bool on)
{
    if (!mixer_has_mute(s)) return;
    switch (s.kind) {
        case MIX_SRC_LAYER: {
            uint8_t b = (uint8_t)(1u << s.a);
            s_mix.layer_mute = on ? (uint8_t)(s_mix.layer_mute | b) : (uint8_t)(s_mix.layer_mute & ~b);
            s_layer_dirty |= b;
            return;
        }
        case MIX_SRC_TRACK: {
            uint8_t b = (uint8_t)(1u << s.b);
            s_mix.track_mute[s.a] = on ? (uint8_t)(s_mix.track_mute[s.a] | b)
                                       : (uint8_t)(s_mix.track_mute[s.a] & ~b);
            s_track_dirty[s.a] |= b;
            return;
        }
        default: {
            uint8_t b = (uint8_t)(1u << misc_index(s));
            s_mix.misc_mute = on ? (uint8_t)(s_mix.misc_mute | b) : (uint8_t)(s_mix.misc_mute & ~b);
            s_misc_dirty |= b;
            return;
        }
    }
}

uint8_t mixer_get_fade_bars_idx(void)
{
    return s_mix.fade_bars_idx;
}

void mixer_set_fade_bars_idx(uint8_t idx)
{
    s_mix.fade_bars_idx = (idx < MIXER_FADE_BARS_COUNT) ? idx : (uint8_t)(MIXER_FADE_BARS_COUNT - 1);
}

uint8_t mixer_fade_bars(uint8_t idx)
{
    return s_fade_bars[(idx < MIXER_FADE_BARS_COUNT) ? idx : MIXER_FADE_BARS_COUNT - 1];
}

/* One slot's toggle; returns true when it now heads out (to_pos 0). */
static bool fade_toggle_one(int i, mix_src_t s)
{
    mix_fade_t *f = &s_fade[i];
    uint8_t pos = mixer_get_pos(s);
    uint8_t back = s_restore_pos[i] ? s_restore_pos[i] : (uint8_t)MIXER_POS_MAX;
    f->len_ticks = (uint32_t)mixer_fade_bars(s_mix.fade_bars_idx) * SEQ_TICKS_PER_BAR;
    if (f->active) {
        if (pos == f->from_pos) {
            /* Not moving yet (armed for the bar line, or just started):
             * flip the shape. An out becomes an in from silence, the row
             * goes quiet now and rises from the same start; an in becomes
             * an out from its remembered level. */
            if (f->to_pos == 0u) { f->from_pos = 0;    f->to_pos = back; write_pos(s, 0); }
            else                 { f->from_pos = back; f->to_pos = 0;    write_pos(s, back); }
            return f->to_pos == 0u;
        }
        /* Under way: turn around from where it is, at once, no re-quantize. */
        f->to_pos     = (f->to_pos != 0u) ? 0u : back;
        f->from_pos   = pos;
        f->start_tick = sequencer_ticks();
        return f->to_pos == 0u;
    }
    if (pos > 0u) {
        s_restore_pos[i] = pos;
        f->to_pos = 0;
    } else {
        f->to_pos = back;
    }
    f->from_pos = pos;
    /* Next bar line; no bar grid while stopped, so start now. */
    uint32_t bar = sequencer_core_next_bar_tick();
    f->start_tick = bar ? bar : sequencer_ticks();
    f->active = 1;
    return f->to_pos == 0u;
}

/* The direction a toggle of slot i would choose: true = out. */
static bool fade_would_go_out(int i, mix_src_t s)
{
    if (s_fade[i].active) return s_fade[i].to_pos != 0u;   /* a toggle reverses it */
    return mixer_get_pos(s) > 0u;
}

void mixer_fade_toggle(mix_src_t s)
{
    int i = fade_index(s);
    if (i < 0) return;
    bool out = fade_toggle_one(i, s);
    if (s_link[i] == MIXER_LINK_NONE) return;
    /* Members already at the target or already heading there are left
     * alone; the rest toggle, reading the same next bar line. */
    for (int j = 0; j < FADE_SLOTS; j++) {
        if (j == i || s_link[j] != s_link[i]) continue;
        mix_src_t src = fade_src(j);
        if (!mixer_src_present(src)) continue;
        if (fade_would_go_out(j, src) == out) fade_toggle_one(j, src);
    }
}

uint8_t mixer_get_link(mix_src_t s)
{
    int i = fade_index(s);
    return (i < 0) ? (uint8_t)MIXER_LINK_NONE : s_link[i];
}

void mixer_set_link(mix_src_t s, uint8_t group)
{
    int i = fade_index(s);
    if (i < 0) return;
    s_link[i] = (group > MIXER_LINK_COUNT) ? (uint8_t)MIXER_LINK_COUNT : group;
}

void mixer_clear_links(void)
{
    memset(s_link, MIXER_LINK_NONE, sizeof(s_link));
}

bool mixer_fade_active(mix_src_t s, uint8_t *to_pos)
{
    int i = fade_index(s);
    if (i < 0 || !s_fade[i].active) return false;
    if (to_pos) *to_pos = s_fade[i].to_pos;
    return true;
}

uint8_t mixer_fade_pos(uint8_t from_pos, uint8_t to_pos, uint32_t start_tick,
                       uint32_t len_ticks, uint32_t now, bool *done)
{
    *done = false;
    if ((int32_t)(now - start_tick) < 0) return from_pos;
    uint32_t elapsed = now - start_tick;
    if (elapsed >= len_ticks) {
        *done = true;
        return to_pos;
    }
    int32_t d    = (int32_t)to_pos - (int32_t)from_pos;
    int32_t len  = (int32_t)len_ticks;
    int32_t num  = d * (int32_t)elapsed;
    int32_t step = (num + ((num >= 0) ? len / 2 : -len / 2)) / len;
    int32_t p    = (int32_t)from_pos + step;
    int32_t lo   = (d >= 0) ? from_pos : to_pos;
    int32_t hi   = (d >= 0) ? to_pos : from_pos;
    return (uint8_t)((p < lo) ? lo : (p > hi) ? hi : p);
}

float mixer_pos_gain(uint8_t pos)
{
    return gain(pos);
}

/* Step 0: every active fade's position from the tick count, written only
 * when it moved. */
static void fade_step(void)
{
    uint32_t now = sequencer_ticks();
    for (int i = 0; i < FADE_SLOTS; i++) {
        mix_fade_t *f = &s_fade[i];
        if (!f->active) continue;
        mix_src_t s = fade_src(i);
        bool done;
        uint8_t pos = mixer_fade_pos(f->from_pos, f->to_pos, f->start_tick, f->len_ticks, now, &done);
        if (done) f->active = 0;
        if (pos != mixer_get_pos(s)) write_pos(s, pos);
    }
}

static float track_factor(uint8_t L, uint8_t t)
{
    if (((s_mix.layer_mute >> L) & 1u) || ((s_mix.track_mute[L] >> t) & 1u)) return 0.0f;
    return gain(s_mix.layer_pos[L]) * gain(s_mix.track_pos[L][t]);
}

static float misc_factor(uint8_t k)
{
    return ((s_mix.misc_mute >> k) & 1u) ? 0.0f : gain(s_mix.misc_pos[k]);
}

/* Record one present synth for this frame. */
static void note_synth(uint8_t synth, float factor, bool dirty)
{
    if (synth == 0u || synth >= SYNTH_SLOT_COUNT) return;
    bit_set(s_present, synth);
    s_factor[synth] = factor;
    if (dirty) bit_set(s_want, synth);
}

static void note_misc(uint8_t k, uint8_t synth, bool present)
{
    if (present) note_synth(synth, misc_factor(k), (s_misc_dirty >> k) & 1u);
}

void mixer_service(void)
{
    fade_step();

    memset(s_present, 0, sizeof(s_present));
    memset(s_want, 0, sizeof(s_want));

    /* Present synths and their factors; dirty sources request a push. */
    uint8_t n = sequencer_core_get_num_layers();
    if (n > MAX_LAYERS) n = MAX_LAYERS;
    for (uint8_t L = 0; L < n; L++) {
        uint8_t tracks = sequencer_core_get_layer_tracks(L);
        if (tracks > SEQ_TRACKS) tracks = SEQ_TRACKS;
        for (uint8_t t = 0; t < tracks; t++) {
            bool dirty = ((s_layer_dirty >> L) & 1u) || ((s_track_dirty[L] >> t) & 1u);
            note_synth(sequencer_core_get_track_synth(L, t), track_factor(L, t), dirty);
        }
    }
    note_misc(MIX_MISC_ARP, SEQ_ARP_SYNTH, true);
    bool std_on = drone_std_get_enabled();
    note_misc(MIX_MISC_DRONE_STD, DRONE_STD_SYNTH_MAIN, std_on);
    note_misc(MIX_MISC_DRONE_STD, DRONE_STD_SYNTH_SUB, std_on && drone_std_get_sub_enabled());
    bool stut_on = drone_get_enabled();
    note_misc(MIX_MISC_DRONE_STUT, DRONE_SYNTH_MAIN, stut_on);
    note_misc(MIX_MISC_DRONE_STUT, DRONE_SYNTH_SUB, stut_on && drone_get_sub_enabled());
    note_misc(MIX_MISC_LIVE, LIVE_SYNTH, live_ready());
    for (uint8_t c = 0; c < CLIP_SLOT_COUNT; c++) {
        note_misc((uint8_t)(MIX_MISC_CLIP0 + c), (uint8_t)(CLIP_SYNTH_BASE + c),
                  clip_player_slot_state(c) == CLIP_SLOT_LOADED);
    }
    /* A dirty source that is not present is dropped: it is brought to its
     * gain by the presence edge once it appears. */
    s_layer_dirty = 0;
    memset(s_track_dirty, 0, sizeof(s_track_dirty));
    s_misc_dirty = 0;

    /* Steps 1 and 2: dirty sources and presence edges. A synth at unity
     * whose last mixer factor was unity already sits at AMY's own value. */
    uint8_t pushed[SYNTH_BYTES];
    memset(pushed, 0, sizeof(pushed));
    for (uint16_t s = 1; s < SYNTH_SLOT_COUNT; s++) {
        if (!bit_get(s_present, s)) continue;
        if (!bit_get(s_want, s) && bit_get(s_present_last, s)) continue;
        float f = s_factor[s];
        if (f == 1.0f && s_sent[s] == 1.0f) continue;
        synth_level_set_mix((uint8_t)s, f);
        s_sent[s] = f;
        bit_set(pushed, s);
    }
    memcpy(s_present_last, s_present, sizeof(s_present_last));

    /* Step 3: re-send sub-unity present synths round robin, re-establishing
     * a level a patch load reset to unity. */
    uint8_t  sent = 0;
    uint16_t i;
    for (i = 0; i < SYNTH_SLOT_COUNT && sent < MIXER_REASSERT_PER_FRAME; i++) {
        uint16_t s = (uint16_t)((s_rr + i) % SYNTH_SLOT_COUNT);
        if (!bit_get(s_present, s) || bit_get(pushed, s) || s_factor[s] == 1.0f) continue;
        synth_level_set_mix((uint8_t)s, s_factor[s]);
        s_sent[s] = s_factor[s];
        sent++;
    }
    s_rr = (uint16_t)((s_rr + i) % SYNTH_SLOT_COUNT);
}

void mixer_on_layer_added(uint8_t idx)
{
    if (idx >= MAX_LAYERS) return;
    layer_defaults(idx);
    fade_clear_layer(idx);
    link_clear_layer(idx);
    s_layer_dirty |= (uint8_t)(1u << idx);
}

void mixer_on_layer_deleted(uint8_t idx)
{
    if (idx >= MAX_LAYERS) return;
    for (uint8_t L = idx; L + 1u < MAX_LAYERS; L++) {
        s_mix.layer_pos[L] = s_mix.layer_pos[L + 1u];
        memcpy(s_mix.track_pos[L], s_mix.track_pos[L + 1u], sizeof(s_mix.track_pos[L]));
        s_mix.track_mute[L] = s_mix.track_mute[L + 1u];
        s_fade[FADE_LAYER0 + L]        = s_fade[FADE_LAYER0 + L + 1];
        s_restore_pos[FADE_LAYER0 + L] = s_restore_pos[FADE_LAYER0 + L + 1];
        s_link[FADE_LAYER0 + L]        = s_link[FADE_LAYER0 + L + 1];
        memcpy(&s_fade[FADE_TRACK0 + L * SEQ_TRACKS], &s_fade[FADE_TRACK0 + (L + 1) * SEQ_TRACKS],
               SEQ_TRACKS * sizeof(s_fade[0]));
        memcpy(&s_restore_pos[FADE_TRACK0 + L * SEQ_TRACKS],
               &s_restore_pos[FADE_TRACK0 + (L + 1) * SEQ_TRACKS], SEQ_TRACKS);
        memcpy(&s_link[FADE_TRACK0 + L * SEQ_TRACKS],
               &s_link[FADE_TRACK0 + (L + 1) * SEQ_TRACKS], SEQ_TRACKS);
    }
    uint8_t below = (uint8_t)(s_mix.layer_mute & ((1u << idx) - 1u));
    uint8_t above = (uint8_t)((s_mix.layer_mute >> (idx + 1u)) << idx);
    s_mix.layer_mute = (uint8_t)(below | above);
    layer_defaults(MAX_LAYERS - 1u);
    fade_clear_layer(MAX_LAYERS - 1u);
    link_clear_layer(MAX_LAYERS - 1u);
    s_layer_dirty |= (uint8_t)(LAYER_MASK & ~((1u << idx) - 1u));
}

void mixer_export(mixer_snapshot_t *out)
{
    if (!out) return;
    *out = s_mix;
    out->master_link = s_link[0];
    memcpy(out->layer_link, &s_link[FADE_LAYER0], sizeof(out->layer_link));
    memcpy(out->track_link, &s_link[FADE_TRACK0], sizeof(out->track_link));
    memcpy(out->misc_link, &s_link[FADE_MISC0], sizeof(out->misc_link));
}

static uint8_t clamp_link(uint8_t g)
{
    return (g > MIXER_LINK_COUNT) ? (uint8_t)MIXER_LINK_COUNT : g;
}

void mixer_import(const mixer_snapshot_t *in)
{
    if (!in) return;
    uint8_t n = sequencer_core_get_num_layers();
    for (uint8_t L = 0; L < MAX_LAYERS; L++) {
        if (L >= n) {
            layer_defaults(L);
            link_clear_layer(L);
            continue;
        }
        s_mix.layer_pos[L] = clamp_pos(in->layer_pos[L]);
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            s_mix.track_pos[L][t] = clamp_pos(in->track_pos[L][t]);
        }
        s_link[FADE_LAYER0 + L] = clamp_link(in->layer_link[L]);
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            s_link[FADE_TRACK0 + L * SEQ_TRACKS + t] = clamp_link(in->track_link[L][t]);
        }
        s_mix.track_mute[L] = (uint8_t)(in->track_mute[L] & TRACK_MASK);
        uint8_t b = (uint8_t)(1u << L);
        s_mix.layer_mute = (in->layer_mute & b) ? (uint8_t)(s_mix.layer_mute | b)
                                                : (uint8_t)(s_mix.layer_mute & ~b);
    }
    for (uint8_t k = 0; k < MIX_MISC_COUNT; k++) {
        s_mix.misc_pos[k] = clamp_pos(in->misc_pos[k]);
        s_link[FADE_MISC0 + k] = clamp_link(in->misc_link[k]);
    }
    s_link[0] = clamp_link(in->master_link);
    s_mix.misc_mute = (uint8_t)(in->misc_mute & MISC_MASK);
    mixer_set_fade_bars_idx(in->fade_bars_idx);
    fade_clear_all();
    mark_all_dirty();
}
