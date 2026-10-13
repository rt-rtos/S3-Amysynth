#include "mixer.h"
#include "synth_level.h"
#include "synth_slots.h"
#include "sequencer_core.h"
#include "amy_fx.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"
#include "custompatches/clip_player.h"
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

static void layer_defaults(uint8_t L)
{
    s_mix.layer_pos[L] = MIXER_POS_MAX;
    s_mix.layer_mute  &= (uint8_t)~(1u << L);
    memset(s_mix.track_pos[L], MIXER_POS_MAX, sizeof(s_mix.track_pos[L]));
    s_mix.track_mute[L] = 0;
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
    memset(&s_mix, 0, sizeof(s_mix));
    for (uint8_t L = 0; L < MAX_LAYERS; L++) layer_defaults(L);
    memset(s_mix.misc_pos, MIXER_POS_MAX, sizeof(s_mix.misc_pos));
    s_layer_dirty = 0;
    memset(s_track_dirty, 0, sizeof(s_track_dirty));
    s_misc_dirty = 0;
    memset(s_present_last, 0, sizeof(s_present_last));
    for (uint16_t i = 0; i < SYNTH_SLOT_COUNT; i++) s_sent[i] = 1.0f;
    s_rr = 0;
}

void mixer_reset_defaults(void)
{
    for (uint8_t L = 0; L < MAX_LAYERS; L++) layer_defaults(L);
    memset(s_mix.misc_pos, MIXER_POS_MAX, sizeof(s_mix.misc_pos));
    s_mix.misc_mute = 0;
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

void mixer_set_pos(mix_src_t s, uint8_t pos)
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
    s_layer_dirty |= (uint8_t)(1u << idx);
}

void mixer_on_layer_deleted(uint8_t idx)
{
    if (idx >= MAX_LAYERS) return;
    for (uint8_t L = idx; L + 1u < MAX_LAYERS; L++) {
        s_mix.layer_pos[L] = s_mix.layer_pos[L + 1u];
        memcpy(s_mix.track_pos[L], s_mix.track_pos[L + 1u], sizeof(s_mix.track_pos[L]));
        s_mix.track_mute[L] = s_mix.track_mute[L + 1u];
    }
    uint8_t below = (uint8_t)(s_mix.layer_mute & ((1u << idx) - 1u));
    uint8_t above = (uint8_t)((s_mix.layer_mute >> (idx + 1u)) << idx);
    s_mix.layer_mute = (uint8_t)(below | above);
    layer_defaults(MAX_LAYERS - 1u);
    s_layer_dirty |= (uint8_t)(LAYER_MASK & ~((1u << idx) - 1u));
}

void mixer_export(mixer_snapshot_t *out)
{
    if (out) *out = s_mix;
}

void mixer_import(const mixer_snapshot_t *in)
{
    if (!in) return;
    uint8_t n = sequencer_core_get_num_layers();
    for (uint8_t L = 0; L < MAX_LAYERS; L++) {
        if (L >= n) {
            layer_defaults(L);
            continue;
        }
        s_mix.layer_pos[L] = clamp_pos(in->layer_pos[L]);
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            s_mix.track_pos[L][t] = clamp_pos(in->track_pos[L][t]);
        }
        s_mix.track_mute[L] = (uint8_t)(in->track_mute[L] & TRACK_MASK);
        uint8_t b = (uint8_t)(1u << L);
        s_mix.layer_mute = (in->layer_mute & b) ? (uint8_t)(s_mix.layer_mute | b)
                                                : (uint8_t)(s_mix.layer_mute & ~b);
    }
    for (uint8_t k = 0; k < MIX_MISC_COUNT; k++) {
        s_mix.misc_pos[k] = clamp_pos(in->misc_pos[k]);
    }
    s_mix.misc_mute = (uint8_t)(in->misc_mute & MISC_MASK);
    mark_all_dirty();
}
