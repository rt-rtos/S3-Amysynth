#include "synth_ui/ui_mixer_quick.h"
#include "synth_ui/synth_ui_internal.h"   /* ui_gate_*, ui_note_name */
#include "sequencer_core.h"
#include "seq_core_config.h"               /* SEQ_SWING_MAX */
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"
#include "custompatches/drone_common.h"    /* DRONE_ROOT_MIN/MAX */
#include "display_text.h"                  /* display_hz_label */
#include "seq_clamp.h"
#include "sdkconfig.h"
#if CONFIG_SYNTH_WIRELESS
#include "live_play.h"
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Quick cells per row kind: contract in ui_mixer_quick.h. */

typedef enum {
    Q_NONE = 0, Q_CUT, Q_GAT, Q_SWG, Q_OCT, Q_GLD, Q_PAT, Q_RT, Q_RPT,
    Q_COUNT
} q_param_t;

static const char *const s_labels[Q_COUNT] = {
    [Q_NONE] = "", [Q_CUT] = "CUT", [Q_GAT] = "GAT", [Q_SWG] = "SWG",
    [Q_OCT] = "OCT", [Q_GLD] = "GLD", [Q_PAT] = "PAT", [Q_RT] = "RT",
    [Q_RPT] = "RPT",
};

/* Stutter gate patterns in drone_pattern_t order, four characters. */
static const char *const s_pattern_short[DRONE_PAT_COUNT] = {
    [DRONE_PAT_FULL] = "FULL", [DRONE_PAT_FOUR] = "FOUR", [DRONE_PAT_OFFBEAT] = "OFFB",
    [DRONE_PAT_GALLOP] = "GALP", [DRONE_PAT_DUB] = "DUB",
};

/* One table per row kind, in cell order. */
static const uint8_t s_melodic[MIXER_QUICK_MAX] = { Q_CUT, Q_GAT,  Q_NONE };
static const uint8_t s_drum[MIXER_QUICK_MAX]    = { Q_SWG, Q_NONE, Q_NONE };
static const uint8_t s_track[MIXER_QUICK_MAX]   = { Q_RPT, Q_NONE, Q_NONE };
static const uint8_t s_arp[MIXER_QUICK_MAX]     = { Q_OCT, Q_GLD,  Q_RPT };
static const uint8_t s_stut[MIXER_QUICK_MAX]    = { Q_PAT, Q_RT,   Q_NONE };
static const uint8_t s_std[MIXER_QUICK_MAX]     = { Q_RT,  Q_CUT,  Q_NONE };
#if CONFIG_SYNTH_WIRELESS
static const uint8_t s_live[MIXER_QUICK_MAX]    = { Q_GLD, Q_CUT,  Q_NONE };
#endif

#define CUT_MIN_HZ  65.0f
#define CUT_MAX_HZ  16000.0f
#define SWG_STEP    2
#define GLD_STEP    5
#define GLD_MAX     100

static q_param_t param_of(mix_src_t s, uint8_t slot)
{
    if (slot >= MIXER_QUICK_MAX) return Q_NONE;
    const uint8_t *t = NULL;
    switch (s.kind) {
        case MIX_SRC_LAYER:
            if (s.a >= sequencer_core_get_num_layers()) return Q_NONE;
            t = (sequencer_core_get_layer_type(s.a) == SEQ_LAYER_DRUM) ? s_drum : s_melodic;
            break;
        case MIX_SRC_TRACK:      t = s_track; break;
        case MIX_SRC_ARP:        t = s_arp;  break;
        case MIX_SRC_DRONE_STUT: t = s_stut; break;
        case MIX_SRC_DRONE_STD:  t = s_std;  break;
#if CONFIG_SYNTH_WIRELESS
        case MIX_SRC_LIVE:       t = s_live; break;
#endif
        default:                 return Q_NONE;
    }
    return (q_param_t)t[slot];
}

bool mixer_quick_has(mix_src_t s, uint8_t slot)
{
    return param_of(s, slot) != Q_NONE;
}

const char *mixer_quick_label(mix_src_t s, uint8_t slot)
{
    return s_labels[param_of(s, slot)];
}

static float cut_ratio(float hz, float ratio)
{
    return SEQ_CLAMP_F32(hz * ratio, CUT_MIN_HZ, CUT_MAX_HZ);
}

/* The cutoff a CUT cell shows: a melodic layer's first row, else the
 * source's own filter. */
static float cut_get(mix_src_t s)
{
    seq_filter_t f = { 0 };
    switch (s.kind) {
        case MIX_SRC_LAYER:     sequencer_core_get_melodic_filter(s.a, 0, &f); break;
        case MIX_SRC_DRONE_STD: drone_std_get_filter(&f); break;
#if CONFIG_SYNTH_WIRELESS
        case MIX_SRC_LIVE:      live_play_get_filter(&f); break;
#endif
        default: break;
    }
    return f.cutoff_hz;
}

static void cut_step(mix_src_t s, int detents)
{
    float ratio = exp2f((float)detents / 12.0f);
    seq_filter_t f;
    switch (s.kind) {
        case MIX_SRC_LAYER: {
            /* Read every track before writing any: in LAYER patch scope the
             * rows share one filter block and a set fans out to the peers,
             * so a read-modify-write per track would compound the ratio. */
            uint8_t tracks = sequencer_core_get_layer_tracks(s.a);
            if (tracks > SEQ_TRACKS) tracks = SEQ_TRACKS;
            seq_filter_t fs[SEQ_TRACKS];
            bool have[SEQ_TRACKS];
            for (uint8_t t = 0; t < tracks; t++) {
                have[t] = sequencer_core_get_melodic_filter(s.a, t, &fs[t]);
                if (have[t]) fs[t].cutoff_hz = cut_ratio(fs[t].cutoff_hz, ratio);
            }
            for (uint8_t t = 0; t < tracks; t++) {
                if (have[t]) sequencer_core_set_melodic_filter(s.a, t, &fs[t]);
            }
            break;
        }
        case MIX_SRC_DRONE_STD:
            drone_std_get_filter(&f);
            f.cutoff_hz = cut_ratio(f.cutoff_hz, ratio);
            drone_std_set_filter(&f);
            break;
#if CONFIG_SYNTH_WIRELESS
        case MIX_SRC_LIVE:
            live_play_get_filter(&f);
            f.cutoff_hz = cut_ratio(f.cutoff_hz, ratio);
            live_play_set_filter(&f);
            break;
#endif
        default: break;
    }
}

static uint16_t gld_get(mix_src_t s)
{
#if CONFIG_SYNTH_WIRELESS
    if (s.kind == MIX_SRC_LIVE) return live_play_get_glide_ms();
#endif
    (void)s;
    return arp_get_portamento_ms();
}

static uint8_t rt_get(mix_src_t s)
{
    return (s.kind == MIX_SRC_DRONE_STUT) ? drone_get_root_note() : drone_std_get_root_note();
}

static uint8_t rpt_get(mix_src_t s)
{
    return (s.kind == MIX_SRC_TRACK) ? (uint8_t)sequencer_core_get_track_repeat_rate(s.a, s.b)
                                     : arp_get_repeat_rate();
}

/* 1, 2, 4, 8: one doubling or halving per step, clamped at both ends. */
static uint8_t rate_step(uint8_t rate, int steps)
{
    int r = (int)rate;
    for (; steps > 0 && r < SEQ_REPEAT_8; steps--) r <<= 1;
    for (; steps < 0 && r > SEQ_REPEAT_1; steps++) r >>= 1;
    return (uint8_t)r;
}

void mixer_quick_text(mix_src_t s, uint8_t slot, char out[5])
{
    out[0] = '\0';
    switch (param_of(s, slot)) {
        case Q_CUT: {
            char hz[6];
            display_hz_label(hz, cut_get(s));
            memcpy(out, hz, 4);   /* at most four characters by construction */
            out[4] = '\0';
            break;
        }
        case Q_GAT: ui_gate_format(out, 5, sequencer_core_get_layer_gate_pct(s.a)); break;
        case Q_SWG: snprintf(out, 5, "%u%%", (unsigned)sequencer_core_get_layer_swing(s.a)); break;
        case Q_OCT: snprintf(out, 5, "%u", (unsigned)arp_get_octaves()); break;
        case Q_GLD: {
            unsigned ms = gld_get(s);
            snprintf(out, 5, "%u", (ms > GLD_MAX) ? (unsigned)GLD_MAX : ms);
            break;
        }
        case Q_PAT: {
            drone_pattern_t p = drone_get_pattern();
            snprintf(out, 5, "%s", (p < DRONE_PAT_COUNT) ? s_pattern_short[p] : "?");
            break;
        }
        case Q_RT: {
            char note[4];
            ui_note_name(rt_get(s), note);
            snprintf(out, 5, "%s", note);
            break;
        }
        case Q_RPT: snprintf(out, 5, "%u", (unsigned)rpt_get(s)); break;
        default: break;
    }
}

void mixer_quick_step(mix_src_t s, uint8_t slot, int detents)
{
    if (detents == 0) return;
    int dir = (detents > 0) ? 1 : -1;
    int n   = (detents > 0) ? detents : -detents;
    switch (param_of(s, slot)) {
        case Q_CUT:
            cut_step(s, detents);
            break;
        case Q_GAT:
            /* One ladder detent and one re-emit per detent, as the Layer page. */
            for (int i = 0; i < n; i++) {
                sequencer_core_set_layer_gate_pct(
                    s.a, ui_gate_ladder_step(sequencer_core_get_layer_gate_pct(s.a), dir));
            }
            break;
        case Q_SWG:
            for (int i = 0; i < n; i++) {
                int v = (int)sequencer_core_get_layer_swing(s.a) + dir * SWG_STEP;
                sequencer_core_set_layer_swing(s.a, (uint8_t)SEQ_CLAMP_INT(v, 0, SEQ_SWING_MAX));
            }
            break;
        case Q_OCT: {
            int v = (int)arp_get_octaves() + detents;
            arp_set_octaves((uint8_t)SEQ_CLAMP_INT(v, 1, ARP_OCT_MAX));
            break;
        }
        case Q_GLD: {
            int v = SEQ_CLAMP_INT((int)gld_get(s) + detents * GLD_STEP, 0, GLD_MAX);
#if CONFIG_SYNTH_WIRELESS
            if (s.kind == MIX_SRC_LIVE) { live_play_set_glide_ms((uint16_t)v); break; }
#endif
            arp_set_portamento_ms((uint16_t)v);
            break;
        }
        case Q_PAT: {
            int v = (int)drone_get_pattern() + detents;
            drone_set_pattern((drone_pattern_t)SEQ_CLAMP_INT(v, 0, DRONE_PAT_COUNT - 1));
            break;
        }
        case Q_RT: {
            int v = SEQ_CLAMP_INT((int)rt_get(s) + detents, DRONE_ROOT_MIN, DRONE_ROOT_MAX);
            if (s.kind == MIX_SRC_DRONE_STUT) drone_set_root_note((uint8_t)v);
            else                              drone_std_set_root_note((uint8_t)v);
            break;
        }
        case Q_RPT: {
            uint8_t r = rate_step(rpt_get(s), detents);
            if (s.kind == MIX_SRC_TRACK) sequencer_core_set_track_repeat_rate(s.a, s.b, (seq_repeat_rate_t)r);
            else                         arp_set_repeat_rate(r);
            break;
        }
        default: break;
    }
}
