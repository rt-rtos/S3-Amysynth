#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"
#if CONFIG_SYNTH_WIRELESS
#include "live_play.h"     /* BLE MIDI live-play voice: GRAPH_TGT_LIVE target */
#if CONFIG_SYNTH_CUSTOM_FM
#include "custompatches/fm_voice.h"   /* s_fm_voice: GRAPH_TGT_FM_OP target */
#endif
#endif
#include "graph_popup.h"
#include "filter_graph.h"
#include "display_lfo.h"
#include "display_dist.h"
#include "seq_defaults.h"
#include "amy_helpers.h"
#include "filter_scope.h"
#if CONFIG_FILTER_SCOPE
#include "amy.h"              /* instrument_get_num_voices, amy_voice_base_osc */
#endif
#include "seq_core_config.h"  /* SEQ_SWING_MAX, SEQ_ARP_SYNTH */
#include "seq_clamp.h"
#include "voice_config.h"  /* shared voice constants incl. envelope bounds */
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "synth_ui";

/* ── Graph pop-up integration (isolated, easily removable) ───────────────────
 * Everything up to the "graph pop-up: end" marker wires the reusable
 * graph_popup widget. Removing it also means removing the gated branch in
 * synth_ui_task() and the public synth_ui_graph_* entry points.
 * Melodic envelope defaults come from seq_defaults.h. */

static gpopup_t s_graph_popup;
static bool     s_graph_popup_inited = false;

/* (layer,track) captured at open time so write-back targets the same row even
 * if the selection moves underneath. */
uint8_t  s_graph_layer = 0;
uint8_t  s_graph_track = 0;

/* Which backend the open editor edits, captured at open time so seed/commit
 * route to the right envelope store. One widget + mapping serves all. */
typedef enum {
    GRAPH_TGT_MELODIC   = 0,
    GRAPH_TGT_DRONE     = 1,
    GRAPH_TGT_ARP       = 2,
    GRAPH_TGT_DRONE_STD = 3,
    /* BLE MIDI live-play voice (slot 10). Structurally the arp's twin - one
     * voice, no layer/track scope - so it follows the arp branch everywhere,
     * incl. the LFO split (native carrier on wave patches, 20 Hz software
     * stepper on patch strings via live_play_lfo_service). */
    GRAPH_TGT_LIVE      = 4,
    /* One operator of the live FM_CUSTOM voice (s_fm_voice.op_env[op], the
     * amp-trim sub-mode edits its level). Bound from the FM screen off
     * synth_ui_fm_selected_op(); no layer/track scope, no EG1 page, no
     * filter tab (the operator oscs render outside the per-osc filter). */
    GRAPH_TGT_FM_OP     = 5,
} graph_target_t;
static graph_target_t s_graph_target = GRAPH_TGT_MELODIC;
#if CONFIG_SYNTH_CUSTOM_FM
static uint8_t   s_graph_fm_op = 0;         /* operator index bound at open */
static seq_env_t s_graph_fm_env_open;       /* cancel restores this */
#endif

/* Which of the target's two AMY breakpoint generators is shown. 0 = EG0 (amp),
 * 1 = EG1 (typically the filter sweep, see sequencer_core_push_envelope_eg1()).
 * Reset to 0 on every open; MY_BUTTON_3 visits EG0 then EG1 before the filter
 * editor. */
static uint8_t s_graph_eg_index = 0;

/* Scratch curve type of the shown EG (AMY ENVELOPE_*: 0=Normal, 1=Linear,
 * 2=DX7, 3=TrueExp). Mirrors the target's stored eg_type; folded into the view
 * signature so the top-bar readout redraws. */
static uint8_t s_graph_eg_type_disp = 0;

/* Short top-bar code for an AMY envelope curve type (eg_type 0..3). */
static const char *graph_eg_type_code(uint8_t eg_type)
{
    switch (eg_type & 3u) {
        case 1:  return "LIN";   /* ENVELOPE_LINEAR          */
        case 2:  return "DX7";   /* ENVELOPE_DX7             */
        case 3:  return "EXP";   /* ENVELOPE_TRUE_EXPONENTIAL*/
        default: return "NRM";   /* ENVELOPE_NORMAL          */
    }
}

/* Full name for the transient top-bar readout shown right after a type cycle. */
static const char *graph_eg_type_name(uint8_t eg_type)
{
    switch (eg_type & 3u) {
        case 1:  return "LINEAR";
        case 2:  return "DX7";
        case 3:  return "TRUE EXP";
        default: return "NORMAL";
    }
}

/* Type-cycle flash: the top bar shows the full type name for a short window
 * after a type change (the right slot is otherwise owned by the point readout).
 * The derived active flag is folded into the view signature so the bar redraws
 * when the window closes. */
#define GRAPH_TYPE_FLASH_MS 1200u
static TickType_t s_graph_type_flash_until = 0;

static bool graph_type_flash_active(void)
{
    return (int32_t)(s_graph_type_flash_until - xTaskGetTickCount()) > 0;
}

/* ── Curve-type preview shaping ──────────────────────────────────────────────
 * Per-segment shape callback for the ADSR plot: a float mirror of AMY's
 * compute_breakpoint_scale() (envelope.c) in the widget's normalised 0..1 level
 * space, so the drawn curve matches what eg_type will sound like. Keep in sync
 * with envelope.c. UI tick only, never the render path. */
#define GRAPH_ENV_EPS 0.0002f   /* BREAKPOINT_EPS: floor for the log-domain types */

static float graph_env_shape(float v0, float v1, float t)
{
    uint8_t eg_type = (uint8_t)(s_graph_eg_type_disp & 3u);

    if (eg_type == 1) {         /* ENVELOPE_LINEAR */
        return v0 + (v1 - v0) * t;
    }

    if (eg_type == 2 || eg_type == 3) {   /* DX7 / TRUE_EXPONENTIAL */
        float a = (v0 > GRAPH_ENV_EPS) ? v0 : GRAPH_ENV_EPS;
        float b = (v1 > GRAPH_ENV_EPS) ? v1 : GRAPH_ENV_EPS;
        if (eg_type == 2 && b > a) {
            /* DX7 attack law: levels map linear->DX7 (log2 + 12.375) then
             * through the attack-range curve; time is normalised so only the
             * ratio matters. Degenerate spans fall through to true-exp. */
            float l0 = log2f(a) + 12.375f;
            float l1 = log2f(b) + 12.375f;
            float m0 = 1.0f - ((l0 > 4.25f) ? (l0 - 4.25f) : 0.0f) / 9.375f;
            float m1 = 1.0f - ((l1 > 4.25f) ? (l1 - 4.25f) : 0.0f) / 9.375f;
            float dl = log2f(m0) - log2f(m1);
            if (dl > 1e-6f) {
                float t_const = 1.0f / dl;
                float my_t0   = -t_const * log2f(m0);
                float level   = 4.25f
                    + 9.375f * (1.0f - exp2f(-(my_t0 + t) / t_const));
                return exp2f(level - 12.375f);
            }
        }
        /* TRUE_EXPONENTIAL, and DX7 decay/release (also plain true-exp). */
        float la = log2f(a), lb = log2f(b);
        return exp2f(la + (lb - la) * t);
    }

    /* ENVELOPE_NORMAL: overshoot-compensated "false exponential". */
    const float rate      = -4.328085f;   /* EXP_RATE_VAL */
    const float overshoot = 1.0f / (1.0f - exp2f(rate));
    float y = v0 + (v1 - v0) * overshoot * (1.0f - exp2f(rate * t));
    if (y < 0.0f) y = 0.0f;
    return y;
}

/* ── Time-range mapping ──────────────────────────────────────────────────────
 * Normalised 0..1 X maps to ms with a switchable full-width budget, so one
 * editor serves plucks and pads:
 *   SHORT: 0..2000 ms, LINEAR (fine control where short notes live)
 *   LONG : 0..15000 ms, LOG-SQUASHED (tail compressed so the early portion
 *          keeps most of the pixels). */
#define GRAPH_RANGE_SHORT_MS 2000u
#define GRAPH_RANGE_LONG_MS  15000u
/* Curvature of the long-view squash: larger = more compression of the tail. */
#define GRAPH_LONG_SQUASH    9.0f

static bool s_graph_long_range = false;   /* false = SHORT, true = LONG (auto-switched) */

/* Amp-mode state (MY_BUTTON_2 while the ADSR editor is open). The scratch trim
 * is committed to the target on close and shown in the topbar right slot. */
static bool  s_graph_amp_mode = false;
static float s_graph_amp_edit = 1.0f;   /* scratch 0..1, seeds from target on open */
/* Swing sub-mode, the second stop on MY_BUTTON_2's cycle. Swing is per LAYER,
 * so it is offered only on the EG0 page of a melodic row reading the layer's
 * shared voice block - the same rows the badge marks L. Unlike the amp trim it
 * has no separate store: the setter is the model, so the scratch exists purely
 * to throttle and to cancel back to the open-time value. */
static bool    s_graph_swing_mode = false;
static uint8_t s_graph_swing_edit = 0;
static bool  s_graph_env_dirty = false; /* set only when user moves an ADSR point */

/* EG1 filter-env depth (melodic only): scratch octaves, seeded from the row's
 * seq_filter_t.filter_env_amount. Committed ONLY when dirty, so an untouched
 * editor session never authors the row's filter. */
static float s_graph_fenv_edit  = 0.0f;
static bool  s_graph_fenv_dirty = false;

/* Voice-block source. Which block a row reads and the editors write - its own
 * or the layer's shared one - is engine state (persisted per row, resolved by
 * every get/set/preview call). The editors only query it for the badge, run
 * previews and cancel-restores over the rows sharing the block, and flip it
 * via synth_ui_toggle_editor_source(). Commits are single setter calls: the
 * engine fans a shared block out to its readers. */
static bool editor_src_is_layer(uint8_t li, uint8_t tr)
{
    return sequencer_core_get_melodic_vp_source(li, tr) == SEQ_VP_SRC_LAYER;
}

/* Header badge for one tab: '>' = our copy of this group sounds and commits
 * land here; 'P' = the patch still owns it (never committed, or released).
 * Second letter = the block the row reads, L (layer) or T (own). */
static void editor_src_badge(uint8_t li, uint8_t tr, seq_vp_group_t group,
                             char out[4])
{
    out[0] = sequencer_core_melodic_group_authored(li, tr, group) ? '>' : 'P';
    out[1] = editor_src_is_layer(li, tr) ? 'L' : 'T';
    out[2] = '\0';
}

/* Per-editor "user changed something" flags: a commit only takes authority
 * over a group when it was edited. Cycling through the tabs or flipping the
 * source must not author untouched groups - that would silently replace the
 * patch's own filter/LFO/dist with the editor's seed values. */
static bool s_filter_dirty = false;
static bool s_lfo_dirty    = false;
static bool s_dist_dirty   = false;

/* Live-preview bookkeeping (see "Live preview while editing" below): which
 * parts of the session pushed scratch state to AMY, plus the amp value and
 * throttle needed to restore or pace those pushes. */
static bool       s_graph_live_env  = false; /* env points/type live-pushed   */
static bool       s_graph_live_fenv = false; /* EG1 sweep depth live-pushed   */
static bool       s_amp_live_applied = false;/* a live amp apply landed       */
static bool       s_amp_live_pending = false;
static float      s_graph_amp_open  = 1.0f;  /* amp trim at open (cancel)     */
static TickType_t s_amp_live_last_apply = 0;
static bool       s_swing_live_pending = false;
static uint8_t    s_graph_swing_open = 0;    /* layer swing at open (cancel)  */
static TickType_t s_swing_live_last_apply = 0;

/* log(1 + GRAPH_LONG_SQUASH): normaliser of the long-range squash curve,
 * cached so the mapping helpers don't recompute it per call. */
static float graph_log1p_squash(void)
{
    static float s_val = 0.0f;
    if (s_val == 0.0f) s_val = logf(1.0f + GRAPH_LONG_SQUASH);
    return s_val;
}

/* Normalised X (0..1) -> milliseconds, range/curve aware. */
static uint32_t graph_x_to_ms(float x)
{
    x = SEQ_CLAMP_F32(x, 0.0f, 1.0f);
    if (!s_graph_long_range) {
        return (uint32_t)(x * (float)GRAPH_RANGE_SHORT_MS + 0.5f);
    }
    /* Long view: expand the squashed display X back to a linear time fraction.
     * Display compresses with log1p(k*t)/log1p(k); invert it here. */
    float k = GRAPH_LONG_SQUASH;
    float t = (expf(x * graph_log1p_squash()) - 1.0f) / k;   /* 0..1 linear time */
    return (uint32_t)(t * (float)GRAPH_RANGE_LONG_MS + 0.5f);
}

/* Milliseconds -> normalised X (0..1), range/curve aware (inverse of above). */
static float graph_ms_to_x(uint32_t ms)
{
    if (!s_graph_long_range) {
        float x = (float)ms / (float)GRAPH_RANGE_SHORT_MS;
        return x > 1.0f ? 1.0f : x;
    }
    float k = GRAPH_LONG_SQUASH;
    float t = (float)ms / (float)GRAPH_RANGE_LONG_MS;   /* 0..1 linear time */
    t = SEQ_CLAMP_F32(t, 0.0f, 1.0f);
    /* Compress: more pixels to small t, fewer to the long tail. */
    return logf(1.0f + k * t) / graph_log1p_squash();
}

/* Push the engine's attack floor into the widget as its minimum point spacing.
 * The widget works in normalised X and cannot know what a millisecond is worth,
 * so otherwise it enforces a legibility gap stricter than
 * VOICE_ENV_ATTACK_MIN_MS, differently per range. Re-sync on every range change. */
static void graph_sync_min_gap(void)
{
    graph_popup_set_min_x_gap(&s_graph_popup, graph_ms_to_x(VOICE_ENV_ATTACK_MIN_MS));
}

/* Audio-taper encoder stride for the envelope X axis (the popup's xstep hook).
 * Each detent scales the SEGMENT duration - time since the previous point, i.e.
 * the A/D/R value itself, not the cumulative axis position - by ~8%, plus a
 * minimum stride so the bottom of the range stays reachable. A fixed normalised
 * step would make the smallest attack jump 2->40 ms, which silences short notes
 * under the log-domain curve types. Per-role: attack finest, release coarsest. */
static const float grx_ratio[4]  = { 1.08f, 1.04f, 1.08f, 1.10f }; /* -,A,D,R */
static const float grx_stride[4] = { 1.0f,  1.0f,  1.0f,  2.0f  }; /* min ms  */

static float graph_xstep(uint8_t idx, float x, long delta)
{
    uint32_t prev_ms = 0;
    if (idx > 0 && idx <= s_graph_popup.num_points) {
        prev_ms = graph_x_to_ms(s_graph_popup.points[idx - 1].x);
    }
    uint32_t cum_ms = graph_x_to_ms(x);
    float seg = (cum_ms > prev_ms) ? (float)(cum_ms - prev_ms) : 0.0f;

    float ratio  = grx_ratio[idx & 3u];
    float stride = grx_stride[idx & 3u];
    for (long n = delta; n > 0; --n) {
        float g = seg * ratio;
        seg = (g > seg + stride) ? g : seg + stride;
    }
    for (long n = delta; n < 0; ++n) {
        float g = seg / ratio;
        seg = (g < seg - stride) ? g : seg - stride;
    }
    if (seg < 0.0f) seg = 0.0f;

    float range = s_graph_long_range ? (float)GRAPH_RANGE_LONG_MS
                                     : (float)GRAPH_RANGE_SHORT_MS;
    float cum = (float)prev_ms + seg;
    if (cum > range) cum = range;
    return graph_ms_to_x((uint32_t)(cum + 0.5f));
}

/* Dual-colour panel: rows 0..15 are yellow (context bar), plot fills 16..63. */
#define GRAPH_TOPBAR_H 16

/* ── Auto-decay rule (derived-decay mode only) ───────────────────────────────
 * With CONFIG_SEQ_ADSR_EXPLICIT_DECAY OFF the decay TIME is derived: the
 * sustain point is Y-only in the widget and its X is recomputed here from
 * attack + sustain (lower sustain -> longer fall). With the Kconfig ON
 * (default) the user owns that X and this whole rule is inert. */
#if !CONFIG_SEQ_ADSR_EXPLICIT_DECAY
#define DECAY_BASE_MS          120u
#define DECAY_ATTACK_K         0.5f
#define DECAY_SUSTAIN_SPAN_MS  400.0f
#define DECAY_MIN_MS           20u
#define DECAY_MAX_MS           2000u

static uint32_t graph_decay_ms(uint32_t attack_ms, float sustain_frac)
{
    sustain_frac = SEQ_CLAMP_F32(sustain_frac, 0.0f, 1.0f);
    float d = (float)DECAY_BASE_MS
            + (float)attack_ms * DECAY_ATTACK_K
            + (1.0f - sustain_frac) * DECAY_SUSTAIN_SPAN_MS;
    d = SEQ_CLAMP_F32(d, (float)DECAY_MIN_MS, (float)DECAY_MAX_MS);
    return (uint32_t)(d + 0.5f);
}
#endif /* !CONFIG_SEQ_ADSR_EXPLICIT_DECAY */

static void graph_popup_ensure_init(void)
{
    if (s_graph_popup_inited) return;
    /* Full-screen plot under the yellow context bar (rows 16..63). */
    graph_popup_init(&s_graph_popup, 0, GRAPH_TOPBAR_H, 128,
                     (uint8_t)(64 - GRAPH_TOPBAR_H));
    graph_popup_set_style(&s_graph_popup, GPOPUP_STYLE_ADSR);
    graph_sync_min_gap();
    /* Shape reads s_graph_eg_type_disp at draw time, so type cycles retint
     * instantly. */
    graph_popup_set_shape(&s_graph_popup, graph_env_shape);
    graph_popup_set_xstep(&s_graph_popup, graph_xstep);
#if CONFIG_SEQ_ADSR_EXPLICIT_DECAY
    /* Explicit decay: sustain point draggable on both axes - X = decay ms,
     * Y = sustain level. */
    graph_popup_set_adsr_lock_sx(&s_graph_popup, false);
#else
    /* Derived decay: sustain point is Y-only, X comes from
     * graph_recompute_decay(). */
    graph_popup_set_adsr_lock_sx(&s_graph_popup, true);
#endif
    s_graph_popup_inited = true;
}

/* Recompute the sustain point's X (decay time) from attack + sustain level.
 * Keeps all ms math host-side so the widget stays AMY-agnostic. Expects the
 * standard 4-point ADSR layout. */
static void graph_recompute_decay(void)
{
#if CONFIG_SEQ_ADSR_EXPLICIT_DECAY
    /* User owns the sustain X here; one guard covers every call site. */
    return;
#else
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    if (n < 4) return;

    uint32_t attack_ms   = graph_x_to_ms(pts[1].x);
    float    sustain_frac = pts[2].y;
    uint32_t decay_ms    = graph_decay_ms(attack_ms, sustain_frac);

    pts[2].x = graph_ms_to_x(attack_ms + decay_ms);
    /* Preserve edit state across the points rewrite. */
    uint8_t saved_cursor  = s_graph_popup.cursor;
    bool    saved_editing = s_graph_popup.editing_value;
    bool    saved_axis_y  = s_graph_popup.adjust_axis_y;
    graph_popup_set_points(&s_graph_popup, pts, n);
    s_graph_popup.cursor        = saved_cursor;
    s_graph_popup.editing_value = saved_editing;
    s_graph_popup.adjust_axis_y = saved_axis_y;
#endif /* !CONFIG_SEQ_ADSR_EXPLICIT_DECAY */
}

/* Bottom-margin time ticks for the active range, mapped through graph_ms_to_x()
 * so spacing reflects the (non-linear in LONG) axis. */
static void graph_update_ticks(void)
{
    float xs[6];
    uint8_t n = 0;
    if (!s_graph_long_range) {
        /* SHORT (2s linear): 0, 0.5, 1, 1.5, 2 s. */
        static const uint32_t tms[] = { 0, 500, 1000, 1500, 2000 };
        for (uint8_t i = 0; i < 5; ++i) xs[n++] = graph_ms_to_x(tms[i]);
    } else {
        /* LONG (15s log-squashed): 0, 0.1, 0.5, 1, 5, 15 s. */
        static const uint32_t tms[] = { 0, 100, 500, 1000, 5000, 15000 };
        for (uint8_t i = 0; i < 6; ++i) xs[n++] = graph_ms_to_x(tms[i]);
    }
    graph_popup_set_ticks(&s_graph_popup, xs, n);
}

bool synth_ui_graph_is_active(void)
{
    return graph_popup_is_active(&s_graph_popup);
}

/* Seed the editor's points from the stored envelope under the current time
 * range. Cumulative time is squashed segment by segment so the shape matches
 * what write-back reconstructs. */
static void graph_seed_from_env(const seq_env_t *env)
{
    uint32_t cum_a = env->attack_ms;
    uint32_t cum_d = env->attack_ms + env->decay_ms;
    uint32_t cum_r = env->attack_ms + env->decay_ms + env->release_ms;

    gpopup_point_t pts[4];
    pts[0].x = 0.0f;                 pts[0].y = 0.0f;                       /* origin     */
    pts[1].x = graph_ms_to_x(cum_a); pts[1].y = 1.0f;                       /* attack peak*/
    pts[2].x = graph_ms_to_x(cum_d); pts[2].y = (float)env->sustain_pct / 100.0f; /* sustain */
    pts[3].x = graph_ms_to_x(cum_r); pts[3].y = 0.0f;                       /* release end*/
    graph_popup_set_points(&s_graph_popup, pts, 4);
    /* Derived-decay mode: snap sustain X to the auto-decay rule. No-op in
     * explicit-decay mode, where the stored decay_ms is shown as-is. */
    graph_recompute_decay();
}

/* Read the bound target's envelope for eg_index (0=EG0, 1=EG1). Returns false
 * only if the melodic target has no valid row (caller seeds defaults). */
static bool graph_read_target_env_idx(seq_env_t *env, uint8_t eg_index)
{
    if (eg_index == 1) {
        switch (s_graph_target) {
            case GRAPH_TGT_DRONE:
                drone_get_envelope2(env);
                return true;
            case GRAPH_TGT_DRONE_STD:
                drone_std_get_envelope2(env);
                return true;
            case GRAPH_TGT_ARP:
                arp_get_envelope2(env);
                return true;
#if CONFIG_SYNTH_WIRELESS
            case GRAPH_TGT_LIVE:
                live_play_get_envelope2(env);
                return true;
#endif
            case GRAPH_TGT_FM_OP:
                return false;           /* no EG1 page; caller seeds defaults */
            case GRAPH_TGT_MELODIC:
            default:
                return sequencer_core_get_melodic_envelope2(s_graph_layer,
                                                            s_graph_track, env);
        }
    }
    switch (s_graph_target) {
#if CONFIG_SYNTH_CUSTOM_FM
        case GRAPH_TGT_FM_OP:
            *env = s_fm_voice.op_env[s_graph_fm_op];
            return true;
#endif
        case GRAPH_TGT_DRONE:
            drone_get_envelope(env);
            return true;
        case GRAPH_TGT_DRONE_STD:
            drone_std_get_envelope(env);
            return true;
        case GRAPH_TGT_ARP:
            arp_get_envelope(env);
            return true;
#if CONFIG_SYNTH_WIRELESS
        case GRAPH_TGT_LIVE:
            live_play_get_envelope(env);
            return true;
#endif
        case GRAPH_TGT_MELODIC:
        default:
            return sequencer_core_get_melodic_envelope(s_graph_layer,
                                                       s_graph_track, env);
    }
}

/* Write `env` back to the bound target's store for eg_index (0=EG0, 1=EG1). */
static void graph_write_target_env_idx(const seq_env_t *env, uint8_t eg_index)
{
    if (eg_index == 1) {
        switch (s_graph_target) {
            case GRAPH_TGT_DRONE:
                drone_set_envelope2(env);
                break;
            case GRAPH_TGT_DRONE_STD:
                drone_std_set_envelope2(env);
                break;
            case GRAPH_TGT_ARP:
                arp_set_envelope2(env);
                break;
#if CONFIG_SYNTH_WIRELESS
            case GRAPH_TGT_LIVE:
                live_play_set_envelope2(env);
                break;
#endif
            case GRAPH_TGT_FM_OP:
                break;                  /* no EG1 on an operator */
            case GRAPH_TGT_MELODIC:
            default:
                sequencer_core_set_melodic_envelope2(s_graph_layer, s_graph_track, env);
                break;
        }
        return;
    }
    switch (s_graph_target) {
#if CONFIG_SYNTH_CUSTOM_FM
        case GRAPH_TGT_FM_OP:
            /* The store IS the live voice: writing it and pushing the
             * operator is both the preview and the commit. */
            s_fm_voice.op_env[s_graph_fm_op] = *env;
            sequencer_core_fm_voice_changed(s_graph_fm_op);
            break;
#endif
        case GRAPH_TGT_DRONE:
            drone_set_envelope(env);
            break;
        case GRAPH_TGT_DRONE_STD:
            drone_std_set_envelope(env);
            break;
        case GRAPH_TGT_ARP:
            arp_set_envelope(env);
            break;
#if CONFIG_SYNTH_WIRELESS
        case GRAPH_TGT_LIVE:
            live_play_set_envelope(env);
            break;
#endif
        case GRAPH_TGT_MELODIC:
        default:
            sequencer_core_set_melodic_envelope(s_graph_layer, s_graph_track, env);
            break;
    }
}

/* Backward-compatible wrapper: reads whichever eg_index is currently shown. */
static bool graph_read_target_env(seq_env_t *env)
{
    return graph_read_target_env_idx(env, s_graph_eg_index);
}

/* Open the editor seeded from the active screen's envelope: drone/arp screens
 * edit their own env, otherwise the selected melodic row.
 *
 * The Wireless page is a menu overlay, not a ui_mode, so it must be tested
 * BEFORE the mode ladder - the mode underneath an overlay is whatever screen
 * the user came from. */
void synth_ui_graph_open_envelope(void)
{
    graph_popup_ensure_init();

#if CONFIG_SYNTH_WIRELESS
    if (synth_ui_wireless_page_is_open()) {
        s_graph_target = GRAPH_TGT_LIVE;
    } else
#endif
    if (seq_state.ui_mode == UI_MODE_DRONE) {
        s_graph_target = GRAPH_TGT_DRONE;
    } else if (seq_state.ui_mode == UI_MODE_DRONE_STD) {
        s_graph_target = GRAPH_TGT_DRONE_STD;
    } else if (seq_state.ui_mode == UI_MODE_ARP) {
        s_graph_target = GRAPH_TGT_ARP;
#if CONFIG_SYNTH_CUSTOM_FM
    } else if (seq_state.ui_mode == UI_MODE_FM) {
        s_graph_target = GRAPH_TGT_FM_OP;
        s_graph_fm_op  = synth_ui_fm_selected_op();
        if (s_graph_fm_op >= FM_NUM_OPS) s_graph_fm_op = 0;
        s_graph_fm_env_open = s_fm_voice.op_env[s_graph_fm_op];
#endif
    } else {
        s_graph_target = GRAPH_TGT_MELODIC;
    }

    /* Melodic write-back targets a specific row; capture it at open time. */
    s_graph_layer = seq_state.active_layer_idx;
    s_graph_track = seq_state.selected_track;
    /* Always open on EG0 (amp); MY_BUTTON_3 advances to the EG1 page. */
    s_graph_eg_index = 0;

    seq_env_t env;
    if (!graph_read_target_env(&env)) {
        env = seq_default_melodic_env();
    }
    s_graph_eg_type_disp = env.eg_type;
    s_graph_type_flash_until = 0;   /* no stale type flash from a prior session */
    /* Fresh live-preview session: nothing pushed yet. */
    s_graph_live_env   = false;
    s_graph_live_fenv  = false;
    s_amp_live_applied = false;
    s_amp_live_pending = false;

    /* Set the range BEFORE seeding so graph_ms_to_x() uses the right mapping.
     * No remap needed: there are no popup points yet. */
    uint32_t total_env_ms = env.attack_ms + env.decay_ms + env.release_ms;
    s_graph_long_range = (total_env_ms >= GRAPH_RANGE_SHORT_MS);

    /* Seed amp scratch from the target's current trim; reset amp mode and dirty flag. */
    s_graph_amp_mode  = false;
    s_graph_env_dirty = false;
    switch (s_graph_target) {
        case GRAPH_TGT_DRONE:
            s_graph_amp_edit = drone_get_amp_trim();
            break;
        case GRAPH_TGT_DRONE_STD:
            s_graph_amp_edit = drone_std_get_amp_trim();
            break;
        case GRAPH_TGT_ARP:
            s_graph_amp_edit = arp_get_amp_scale();
            break;
#if CONFIG_SYNTH_WIRELESS
        case GRAPH_TGT_LIVE:
            s_graph_amp_edit = live_play_get_amp_scale();
            break;
#endif
#if CONFIG_SYNTH_CUSTOM_FM
        case GRAPH_TGT_FM_OP:
            s_graph_amp_edit = s_fm_voice.op_level[s_graph_fm_op];
            break;
#endif
        case GRAPH_TGT_MELODIC:
        default:
            s_graph_amp_edit = sequencer_core_get_melodic_amp_scale(
                s_graph_layer, s_graph_track);
            break;
    }
    s_graph_amp_open = s_graph_amp_edit;   /* cancel restores this value */

    /* Swing scratch: layer-scoped, so it seeds from the row's layer regardless
     * of target - graph_swing_available() decides whether it is reachable. */
    s_graph_swing_mode   = false;
    s_swing_live_pending = false;
    s_graph_swing_edit   = sequencer_core_get_layer_swing(s_graph_layer);
    s_graph_swing_open   = s_graph_swing_edit;

    graph_seed_from_env(&env);

    /* Seed the EG1 filter-env depth scratch (melodic rows + arp). */
    s_graph_fenv_dirty = false;
    s_graph_fenv_edit  = 0.0f;
    if (s_graph_target == GRAPH_TGT_MELODIC) {
        seq_filter_t f;
        if (sequencer_core_get_melodic_filter(s_graph_layer, s_graph_track, &f)) {
            s_graph_fenv_edit = f.filter_env_amount;
        }
    } else if (s_graph_target == GRAPH_TGT_ARP) {
        seq_filter_t f;
        arp_get_filter(&f);
        s_graph_fenv_edit = f.filter_env_amount;
#if CONFIG_SYNTH_WIRELESS
    } else if (s_graph_target == GRAPH_TGT_LIVE) {
        seq_filter_t f;
        live_play_get_filter(&f);
        s_graph_fenv_edit = f.filter_env_amount;
#endif
    }

    graph_update_ticks();
    graph_popup_open(&s_graph_popup, GPOPUP_MODE_EDIT, NULL);
    graph_popup_set_style(&s_graph_popup, GPOPUP_STYLE_ADSR);
    s_force_redraw = true;
    ESP_LOGI(TAG, "graph editor open: target=%d L%u T%u range=%s amp=%.2f",
             (int)s_graph_target, s_graph_layer + 1u, s_graph_track + 1u,
             s_graph_long_range ? "LONG" : "SHORT", (double)s_graph_amp_edit);
}

/* Convert the current popup points into `env`'s ADSR fields plus the shown
 * curve type. `env` must be pre-seeded from the target so non-ADSR fields carry
 * through. Returns false when the popup has no full ADSR point set. */
static bool graph_points_to_env(seq_env_t *env)
{
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    if (n < 4) return false;   /* expect origin + A + D + R */

    uint32_t cum_a = graph_x_to_ms(pts[1].x);
    uint32_t cum_d = graph_x_to_ms(pts[2].x);
    uint32_t cum_r = graph_x_to_ms(pts[3].x);

    /* Convert cumulative times back to per-segment durations (clamp monotonic). */
    uint32_t a = SEQ_CLAMP_U32(cum_a, VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    env->attack_ms   = a;
    env->decay_ms    = (cum_d > cum_a) ? (cum_d - cum_a) : 0;
    env->release_ms  = (cum_r > cum_d) ? (cum_r - cum_d) : 0;
    env->sustain_pct = (uint8_t)(pts[2].y * 100.0f + 0.5f);
    env->eg_type     = s_graph_eg_type_disp;
    return true;
}

/* Write the popup points to eg_index's store. graph_toggle_eg_index() uses this
 * on the DEPARTING index before switching the view. */
static void graph_write_points_to_env(uint8_t eg_index)
{
    seq_env_t env;
    if (!graph_read_target_env_idx(&env, eg_index)) return;
    if (!graph_points_to_env(&env)) return;
    graph_write_target_env_idx(&env, eg_index);
}

/* ── Live preview while editing ──────────────────────────────────────────────
 * Edits are auditioned by pushing scratch values to AMY only; the store does
 * not change until confirm. Cancel re-pushes the store - or reloads the layer's
 * patch for a never-authored melodic row, whose live state came from the patch
 * string itself. Amp trim is the exception: it lives in the step-emit path, so
 * its live apply goes through the real setter (throttled, since each melodic
 * apply re-emits the track's steps) and cancel restores the open-time value. */
#define GRAPH_AMP_LIVE_MS 200u               /* min spacing of amp re-emits   */

static void graph_live_push_env(void)
{
    seq_env_t env;
    if (!graph_read_target_env_idx(&env, s_graph_eg_index)) return;
    if (!graph_points_to_env(&env)) return;
    switch (s_graph_target) {
        case GRAPH_TGT_DRONE:
            if (s_graph_eg_index == 1) drone_preview_envelope2(&env);
            else                       drone_preview_envelope(&env);
            break;
        case GRAPH_TGT_DRONE_STD:
            if (s_graph_eg_index == 1) drone_std_preview_envelope2(&env);
            else                       drone_std_preview_envelope(&env);
            break;
        case GRAPH_TGT_ARP:
            if (s_graph_eg_index == 1) arp_preview_envelope2(&env);
            else                       arp_preview_envelope(&env);
            break;
#if CONFIG_SYNTH_WIRELESS
        case GRAPH_TGT_LIVE:
            if (s_graph_eg_index == 1) live_play_preview_envelope2(&env);
            else                       live_play_preview_envelope(&env);
            break;
#endif
#if CONFIG_SYNTH_CUSTOM_FM
        case GRAPH_TGT_FM_OP:
            s_fm_voice.op_env[s_graph_fm_op] = env;
            sequencer_core_fm_voice_changed(s_graph_fm_op);
            break;
#endif
        case GRAPH_TGT_MELODIC:
        default: {
            uint8_t peers[SEQ_TRACKS];
            uint8_t n = sequencer_core_melodic_vp_peers(s_graph_layer, s_graph_track, peers);
            for (uint8_t i = 0; i < n; ++i) {
                if (s_graph_eg_index == 1)
                    sequencer_core_preview_melodic_envelope2(s_graph_layer, peers[i], &env);
                else
                    sequencer_core_preview_melodic_envelope(s_graph_layer, peers[i], &env);
            }
            break;
        }
    }
    s_graph_live_env = true;
}

/* EG1 sweep depth: stored filter + the scratch depth, previewed per row. */
static void graph_live_push_fenv(void)
{
    if (s_graph_target == GRAPH_TGT_ARP) {
        seq_filter_t f;
        arp_get_filter(&f);
        f.filter_env_amount = s_graph_fenv_edit;
        arp_preview_filter(&f);
        s_graph_live_fenv = true;
        return;
    }
#if CONFIG_SYNTH_WIRELESS
    if (s_graph_target == GRAPH_TGT_LIVE) {
        seq_filter_t f;
        live_play_get_filter(&f);
        f.filter_env_amount = s_graph_fenv_edit;
        live_play_preview_filter(&f);
        s_graph_live_fenv = true;
        return;
    }
#endif
    if (s_graph_target != GRAPH_TGT_MELODIC) return;
    uint8_t peers[SEQ_TRACKS];
    uint8_t n = sequencer_core_melodic_vp_peers(s_graph_layer, s_graph_track, peers);
    for (uint8_t i = 0; i < n; ++i) {
        seq_filter_t f;
        if (!sequencer_core_get_melodic_filter(s_graph_layer, peers[i], &f)) continue;
        f.filter_env_amount = s_graph_fenv_edit;
        sequencer_core_preview_melodic_filter(s_graph_layer, peers[i], &f);
    }
    s_graph_live_fenv = true;
}

static void graph_amp_live_set(float v)
{
    switch (s_graph_target) {
        case GRAPH_TGT_DRONE:     drone_set_amp_trim(v);     break;
        case GRAPH_TGT_DRONE_STD: drone_std_set_amp_trim(v); break;
        case GRAPH_TGT_ARP:       arp_set_amp_scale(v);      break;
#if CONFIG_SYNTH_WIRELESS
        case GRAPH_TGT_LIVE:      live_play_set_amp_scale(v); break;
#endif
#if CONFIG_SYNTH_CUSTOM_FM
        case GRAPH_TGT_FM_OP:
            s_fm_voice.op_level[s_graph_fm_op] = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
            sequencer_core_fm_voice_changed(s_graph_fm_op);
            break;
#endif
        case GRAPH_TGT_MELODIC:
        default:
            /* Trim is per row regardless of which voice block the row reads. */
            sequencer_core_set_melodic_amp_scale(s_graph_layer, s_graph_track, v);
            break;
    }
}

/* Apply a pending amp edit once the throttle window passes. Called per detent
 * (leading edge) and from the UI tick (trailing flush), so the final value
 * always lands within ~GRAPH_AMP_LIVE_MS of the last detent. */
static void graph_amp_live_flush(bool force)
{
    if (!s_amp_live_pending) return;
    if (!graph_popup_is_active(&s_graph_popup)) {
        s_amp_live_pending = false;
        return;
    }
    if (!force && (int32_t)(xTaskGetTickCount() - s_amp_live_last_apply)
                      < (int32_t)pdMS_TO_TICKS(GRAPH_AMP_LIVE_MS)) {
        return;
    }
    s_amp_live_last_apply = xTaskGetTickCount();
    s_amp_live_pending = false;
    s_amp_live_applied = true;
    graph_amp_live_set(s_graph_amp_edit);
}

/* Does MY_BUTTON_2's cycle offer the swing stop here? Swing is a property of the
 * layer, so only a melodic row reading the shared block (badge L) may edit it -
 * from a row on its own block the control would silently reach past what the
 * badge says the editor is touching. EG1 is the filter page and keeps the sweep
 * depth in the right slot. */
static bool graph_swing_available(void)
{
    return s_graph_target == GRAPH_TGT_MELODIC && s_graph_eg_index == 0 &&
           editor_src_is_layer(s_graph_layer, s_graph_track);
}

/* Same leading-edge/trailing-flush shape as the amp trim, and for the same
 * reason: sequencer_core_set_layer_swing() re-emits every step of the layer, so
 * an un-throttled encoder spin would flood the ingest pump with re-emits. */
static void graph_swing_live_flush(bool force)
{
    if (!s_swing_live_pending) return;
    if (!graph_popup_is_active(&s_graph_popup)) {
        s_swing_live_pending = false;
        return;
    }
    if (!force && (int32_t)(xTaskGetTickCount() - s_swing_live_last_apply)
                      < (int32_t)pdMS_TO_TICKS(GRAPH_AMP_LIVE_MS)) {
        return;
    }
    s_swing_live_last_apply = xTaskGetTickCount();
    s_swing_live_pending = false;
    sequencer_core_set_layer_swing(s_graph_layer, s_graph_swing_edit);
}

/* Undo every live push on cancel: amp back to its open value, previewed
 * envelope/depth back to the store - or a full layer reload when a touched
 * melodic row was never authored (only the patch knows its state). */
static void graph_live_cancel_restore(void)
{
    s_amp_live_pending = false;
    if (s_amp_live_applied) {
        graph_amp_live_set(s_graph_amp_open);
        s_amp_live_applied = false;
    }
    /* Swing has no scratch store to re-push: the setter IS the model, so the
     * open-time value going back through it is the whole restore. */
    s_swing_live_pending = false;
    if (s_graph_swing_edit != s_graph_swing_open) {
        s_graph_swing_edit = s_graph_swing_open;
        sequencer_core_set_layer_swing(s_graph_layer, s_graph_swing_open);
    }
    if (!s_graph_live_env && !s_graph_live_fenv) return;

    if (s_graph_target == GRAPH_TGT_MELODIC) {
        /* Every peer shares the block, so its authority is one answer. */
        bool need_reload =
            (s_graph_live_env &&
             !sequencer_core_melodic_env_authored(s_graph_layer, s_graph_track,
                                                  s_graph_eg_index)) ||
            (s_graph_live_fenv &&
             !sequencer_core_melodic_filter_authored(s_graph_layer, s_graph_track));
        if (need_reload) {
            sequencer_core_reload_layer_synth(s_graph_layer);
        } else {
            uint8_t peers[SEQ_TRACKS];
            uint8_t n = sequencer_core_melodic_vp_peers(s_graph_layer, s_graph_track, peers);
            seq_env_t env;
            if (s_graph_live_env && graph_read_target_env_idx(&env, s_graph_eg_index)) {
                for (uint8_t i = 0; i < n; ++i) {
                    if (s_graph_eg_index == 1)
                        sequencer_core_preview_melodic_envelope2(s_graph_layer, peers[i], &env);
                    else
                        sequencer_core_preview_melodic_envelope(s_graph_layer, peers[i], &env);
                }
            }
            if (s_graph_live_fenv) {
                for (uint8_t i = 0; i < n; ++i) {
                    seq_filter_t f;
                    if (sequencer_core_get_melodic_filter(s_graph_layer, peers[i], &f))
                        sequencer_core_preview_melodic_filter(s_graph_layer, peers[i], &f);
                }
            }
        }
    } else {
        if (s_graph_live_fenv && s_graph_target == GRAPH_TGT_ARP) {
            /* Re-push the stored filter so the previewed sweep depth reverts. */
            seq_filter_t f;
            arp_get_filter(&f);
            arp_preview_filter(&f);
        }
#if CONFIG_SYNTH_WIRELESS
        if (s_graph_live_fenv && s_graph_target == GRAPH_TGT_LIVE) {
            seq_filter_t f;
            live_play_get_filter(&f);
            live_play_preview_filter(&f);
        }
#endif
        seq_env_t env;
#if CONFIG_SYNTH_CUSTOM_FM
        if (s_graph_target == GRAPH_TGT_FM_OP) {
            /* The store was edited in place: put the open-time envelope back. */
            if (s_graph_live_env) {
                s_fm_voice.op_env[s_graph_fm_op] = s_graph_fm_env_open;
                sequencer_core_fm_voice_changed(s_graph_fm_op);
            }
            s_graph_live_env  = false;
            s_graph_live_fenv = false;
            return;
        }
#endif
        if (s_graph_live_env && graph_read_target_env_idx(&env, s_graph_eg_index)) {
            if (s_graph_target == GRAPH_TGT_ARP) {
                if (s_graph_eg_index == 1) arp_preview_envelope2(&env);
                else                       arp_preview_envelope(&env);
#if CONFIG_SYNTH_WIRELESS
            } else if (s_graph_target == GRAPH_TGT_LIVE) {
                if (s_graph_eg_index == 1) live_play_preview_envelope2(&env);
                else                       live_play_preview_envelope(&env);
#endif
            } else if (s_graph_target == GRAPH_TGT_DRONE_STD) {
                if (s_graph_eg_index == 1) drone_std_preview_envelope2(&env);
                else                       drone_std_preview_envelope(&env);
            } else {
                if (s_graph_eg_index == 1) drone_preview_envelope2(&env);
                else                       drone_preview_envelope(&env);
            }
        }
    }
    s_graph_live_env  = false;
    s_graph_live_fenv = false;
    /* Drop the preview slot the fenv restore may have re-armed. */
    sequencer_core_preview_melodic_clear();
}

/* Convert the edited points back to ms and push them to the bound target. */
static void graph_commit_to_env(void)
{
    /* A volume-only edit must not overwrite the stored envelope. */
    if (s_graph_env_dirty) {
        graph_write_points_to_env(s_graph_eg_index);
    }

    /* Commit the amp trim: setters no-op when unchanged, so this is safe
     * unconditionally. */
    switch (s_graph_target) {
        case GRAPH_TGT_DRONE:
            drone_set_amp_trim(s_graph_amp_edit);
            break;
        case GRAPH_TGT_DRONE_STD:
            drone_std_set_amp_trim(s_graph_amp_edit);
            break;
        case GRAPH_TGT_ARP:
            arp_set_amp_scale(s_graph_amp_edit);
            break;
#if CONFIG_SYNTH_WIRELESS
        case GRAPH_TGT_LIVE:
            live_play_set_amp_scale(s_graph_amp_edit);
            break;
#endif
#if CONFIG_SYNTH_CUSTOM_FM
        case GRAPH_TGT_FM_OP:
            if (s_fm_voice.op_level[s_graph_fm_op] != s_graph_amp_edit) {
                s_fm_voice.op_level[s_graph_fm_op] = s_graph_amp_edit;
                sequencer_core_fm_voice_changed(s_graph_fm_op);
            }
            break;
#endif
        case GRAPH_TGT_MELODIC:
        default:
            sequencer_core_set_melodic_amp_scale(s_graph_layer, s_graph_track,
                                                 s_graph_amp_edit);
            break;
    }
    s_graph_amp_mode = false;   /* clear mode so topbar reverts on next open */

    /* Swing rides on the layer, not the target's voice block: flush the last
     * throttled detent (the setter no-ops when unchanged) and drop the mode. */
    if (s_graph_target == GRAPH_TGT_MELODIC) {
        graph_swing_live_flush(true);
    }
    s_graph_swing_mode = false;

    /* Commit the EG1 depth only if edited. Read-modify-write through the public
     * filter API so the COEF_EG1 push, the EG1 breakpoints, the 0..8 clamp and
     * filter_authored all stay in the engine. Honors the layer/track scope. */
    if (s_graph_fenv_dirty && s_graph_target == GRAPH_TGT_ARP) {
        seq_filter_t f;
        arp_get_filter(&f);
        f.filter_env_amount = s_graph_fenv_edit;
        arp_set_filter(&f);
        s_graph_fenv_dirty = false;
    }
#if CONFIG_SYNTH_WIRELESS
    if (s_graph_fenv_dirty && s_graph_target == GRAPH_TGT_LIVE) {
        seq_filter_t f;
        live_play_get_filter(&f);
        f.filter_env_amount = s_graph_fenv_edit;
        live_play_set_filter(&f);
        s_graph_fenv_dirty = false;
    }
#endif
    if (s_graph_fenv_dirty && s_graph_target == GRAPH_TGT_MELODIC) {
        seq_filter_t f;
        if (sequencer_core_get_melodic_filter(s_graph_layer, s_graph_track, &f)) {
            f.filter_env_amount = s_graph_fenv_edit;
            sequencer_core_set_melodic_filter(s_graph_layer, s_graph_track, &f);
        }
        s_graph_fenv_dirty = false;
    }
    sequencer_core_preview_melodic_clear();
}

/* Set the time-range mode and remap on-screen points so in-progress edits
 * survive the switch. The single place that flips s_graph_long_range - all
 * callers must go through here. */
static void graph_set_range(bool long_range)
{
    if (s_graph_long_range == long_range) return;

    /* Snapshot ms under the OLD range, flip, remap under the NEW one.
     * Y is range-independent. */
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    uint32_t ms[GPOPUP_MAX_POINTS];
    for (uint8_t i = 0; i < n; ++i) ms[i] = graph_x_to_ms(pts[i].x);

    s_graph_long_range = long_range;
    graph_sync_min_gap();   /* the floor's normalised width moves with the axis */

    for (uint8_t i = 0; i < n; ++i) pts[i].x = graph_ms_to_x(ms[i]);
    uint8_t saved_cursor  = s_graph_popup.cursor;
    bool    saved_editing = s_graph_popup.editing_value;
    bool    saved_axis_y  = s_graph_popup.adjust_axis_y;
    graph_popup_set_points(&s_graph_popup, pts, n);
    s_graph_popup.cursor        = saved_cursor;
    s_graph_popup.editing_value = saved_editing;
    s_graph_popup.adjust_axis_y = saved_axis_y;

    graph_update_ticks();
    ESP_LOGI(TAG, "graph range -> %s", long_range ? "LONG(15s)" : "SHORT(2s)");
}

/* Auto-switch the range on total envelope time. Metric is the rightmost point
 * (cum_r = A+D+R), the literal x-extent of the curve. Hysteresis: grow at
 * >=2000 ms, shrink only at <=1700 ms, so the range cannot flicker. */
static void graph_auto_range_check(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return;
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    if (n < 4) return;
    uint32_t total_ms = graph_x_to_ms(pts[3].x);
    if (!s_graph_long_range && total_ms >= GRAPH_RANGE_SHORT_MS) {
        graph_set_range(true);   /* grow to LONG at the SHORT axis ceiling */
    } else if (s_graph_long_range && total_ms <= 1700u) {
        graph_set_range(false);  /* shrink back to SHORT below hysteresis floor */
    }
}

/* Manual SHORT<->LONG toggle, kept for external callers. */
bool synth_ui_graph_toggle_range(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;
    graph_set_range(!s_graph_long_range);
    return true;
}

/* Cycle MY_BUTTON_2's topbar sub-modes: OFF -> AMP -> SWG -> OFF, with the SWG
 * stop skipped where graph_swing_available() says swing is not this editor's to
 * touch. In a sub-mode the encoder edits that value instead of moving ADSR
 * points. Reset on editor open/close. (Name kept: main.c's press-down route and
 * the header contract both call it.) */
void synth_ui_graph_toggle_amp_mode(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return;
    if (s_graph_amp_mode) {
        s_graph_amp_mode   = false;
        s_graph_swing_mode = graph_swing_available();
    } else if (s_graph_swing_mode) {
        graph_swing_live_flush(true);
        s_graph_swing_mode = false;
    } else {
        s_graph_amp_mode = true;
    }
    s_force_redraw = true;
    ESP_LOGI(TAG, "graph topbar mode -> %s",
             s_graph_amp_mode ? "AMP" : (s_graph_swing_mode ? "SWG" : "OFF"));
}

/* Does the bound target expose an EG1 page? The free-running drone never sees
 * another note-on after its enable gate, so its EG1 would fire once and park at
 * sustain forever - a static offset, not an envelope. Hide the page for it (the
 * stutter drone keeps it: its stutter gates are real note-ons). The editor
 * cycle consults this so a hidden page is skipped, not dead-ended on EG0. */
static bool graph_target_has_eg1(void)
{
    return s_graph_target != GRAPH_TGT_DRONE_STD &&
           s_graph_target != GRAPH_TGT_FM_OP;
}

/* Targets carrying an EG1->cutoff sweep DEPTH field (seq_filter_t's
 * filter_env_amount). The drones have none - their EG1 page edits the envelope
 * only. Gates the depth readout, the depth adjust and the polarity flip. */
static bool graph_target_has_eg1_depth(void)
{
#if CONFIG_SYNTH_WIRELESS
    if (s_graph_target == GRAPH_TGT_LIVE) return true;
#endif
    return s_graph_target == GRAPH_TGT_MELODIC || s_graph_target == GRAPH_TGT_ARP;
}

/* Switch between the target's EG0 and EG1 breakpoint sets. An uncommitted edit
 * on the departing index is written through first so flipping tabs never
 * discards work, then the curve and range are reseeded from the other index -
 * the two envelopes can have very different shapes. */
static void graph_toggle_eg_index(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return;
    if (!graph_target_has_eg1()) return;

    if (s_graph_env_dirty) {
        graph_write_points_to_env(s_graph_eg_index);
    }
    /* Swing belongs to the EG0 page: leave the sub-mode behind with it, or the
     * right slot would fight the EG1 sweep-depth readout. */
    if (s_graph_swing_mode) {
        graph_swing_live_flush(true);
        s_graph_swing_mode = false;
    }

    s_graph_eg_index = (s_graph_eg_index == 0) ? 1 : 0;

    seq_env_t env;
    if (!graph_read_target_env(&env)) {
        env = (s_graph_eg_index == 1) ? seq_default_melodic_env1()
                                       : seq_default_melodic_env();
    }
    s_graph_eg_type_disp = env.eg_type;
    uint32_t total_env_ms = env.attack_ms + env.decay_ms + env.release_ms;
    s_graph_long_range = (total_env_ms >= GRAPH_RANGE_SHORT_MS);

    graph_seed_from_env(&env);
    graph_update_ticks();
    s_graph_env_dirty = false;
    s_force_redraw = true;
    ESP_LOGI(TAG, "graph eg index -> EG%u", s_graph_eg_index);
}

/* ── Perceived-duration equivalence across curve types ───────────────────────
 * Fraction of a segment's nominal time at which each type has covered its
 * first/last 40 dB - where a falling segment goes effectively silent, or a
 * rising one becomes audible. Derived from envelope.c's segment laws. Cycling
 * types rescales each segment by g_old/g_new so the AUDIBLE length survives;
 * without it LIN->EXP at fixed ms turns a long release into a pluck. */
static const float graph_g_fall[4] = { 0.94f, 0.99f, 0.54f, 0.54f }; /* NRM LIN DX7 EXP */
static const float graph_g_rise[4] = { 0.94f, 0.99f, 0.94f, 0.54f };

static void graph_rescale_points_for_type(uint8_t old_type, uint8_t new_type)
{
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    if (n < 4) return;

    float cum_a = (float)graph_x_to_ms(pts[1].x);
    float cum_d = (float)graph_x_to_ms(pts[2].x);
    float cum_r = (float)graph_x_to_ms(pts[3].x);
    float a = cum_a;
    float d = (cum_d > cum_a) ? (cum_d - cum_a) : 0.0f;
    float r = (cum_r > cum_d) ? (cum_r - cum_d) : 0.0f;

    a *= graph_g_rise[old_type & 3u] / graph_g_rise[new_type & 3u];
    float kf = graph_g_fall[old_type & 3u] / graph_g_fall[new_type & 3u];
    d *= kf;
    r *= kf;
    if (a < (float)VOICE_ENV_ATTACK_MIN_MS) a = (float)VOICE_ENV_ATTACK_MIN_MS;

    /* Grow into LONG up front so a lengthening rescale isn't clipped by the
     * SHORT axis; the auto-range check below shrinks back if allowed. */
    if (!s_graph_long_range && (a + d + r) >= (float)GRAPH_RANGE_SHORT_MS) {
        graph_set_range(true);
    }
    float range = s_graph_long_range ? (float)GRAPH_RANGE_LONG_MS
                                     : (float)GRAPH_RANGE_SHORT_MS;
    float ca = (a > range) ? range : a;
    float cd = (ca + d > range) ? range : (ca + d);
    float cr = (cd + r > range) ? range : (cd + r);
    pts[1].x = graph_ms_to_x((uint32_t)(ca + 0.5f));
    pts[2].x = graph_ms_to_x((uint32_t)(cd + 0.5f));
    pts[3].x = graph_ms_to_x((uint32_t)(cr + 0.5f));

    uint8_t saved_cursor  = s_graph_popup.cursor;
    bool    saved_editing = s_graph_popup.editing_value;
    bool    saved_axis_y  = s_graph_popup.adjust_axis_y;
    graph_popup_set_points(&s_graph_popup, pts, n);
    s_graph_popup.cursor        = saved_cursor;
    s_graph_popup.editing_value = saved_editing;
    s_graph_popup.adjust_axis_y = saved_axis_y;

    graph_recompute_decay();
    graph_update_ticks();
    graph_auto_range_check();
}

/* Cycle the shown EG's curve type Normal->Linear->DX7->TrueExp (MY_BUTTON_1 in
 * the envelope editor). Live-previewed like every other edit: the store keeps
 * the old type until commit and cancel restores it. Segment times are rescaled
 * to keep the perceived length (graph_rescale_points_for_type). */
void synth_ui_graph_cycle_eg_type(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return;

    uint8_t old_type = s_graph_eg_type_disp;
    s_graph_eg_type_disp = (uint8_t)((s_graph_eg_type_disp + 1u) & 3u);
    graph_rescale_points_for_type(old_type, s_graph_eg_type_disp);
    s_graph_env_dirty = true;   /* commit persists the type with the points */
    graph_live_push_env();
    s_graph_type_flash_until =
        xTaskGetTickCount() + pdMS_TO_TICKS(GRAPH_TYPE_FLASH_MS);
    s_force_redraw = true;
    ESP_LOGI(TAG, "graph EG%u type -> %s(%u) (preview)", s_graph_eg_index,
             graph_eg_type_code(s_graph_eg_type_disp),
             (unsigned)s_graph_eg_type_disp);
}

/* Hint-strip b2 label: MY_BUTTON_2's trim mode edits amplitude on the EG0 page
 * but the EG1->cutoff sweep depth on the melodic EG1 page. */
const char *synth_ui_graph_hint_b2(void)
{
    /* Deliberately narrower than graph_target_has_eg1_depth(): the arp keeps
     * its "Amp" label here. */
    bool env_slot = (s_graph_target == GRAPH_TGT_MELODIC);
#if CONFIG_SYNTH_WIRELESS
    env_slot = env_slot || (s_graph_target == GRAPH_TGT_LIVE);
#endif
    return (s_graph_eg_index == 1 && env_slot) ? "Env" : "Amp";
}

/* Flip the sign of the EG1->cutoff sweep (MY_BUTTON_SHOULDER on the EG1 page).
 * No-op at 0.0 depth: nothing to invert, and it keeps -0.0 out of the readout. */
void synth_ui_graph_flip_eg1_polarity(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return;
    if (s_graph_eg_index != 1) return;
    if (!graph_target_has_eg1_depth()) return;
    if (s_graph_fenv_edit == 0.0f) return;
    s_graph_fenv_edit  = -s_graph_fenv_edit;
    s_graph_fenv_dirty = true;
    s_force_redraw = true;
    ESP_LOGI(TAG, "EG1 polarity -> %+.2f oct", (double)s_graph_fenv_edit);
}

/* Route an encoder delta to the pop-up. Returns true if the pop-up consumed it
 * (host should then skip its normal sequencer routing). */
bool synth_ui_graph_handle_encoder(long delta)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;

    if (s_graph_swing_mode) {
        /* Layer swing in 2% steps: fine enough to find the 54-58% pocket, and
         * the whole 0..66 range is still a third of a turn. */
        int v = (int)s_graph_swing_edit + (int)delta * 2;
        v = SEQ_CLAMP_INT(v, 0, SEQ_SWING_MAX);
        s_graph_swing_edit = (uint8_t)v;
        s_swing_live_pending = true;
        graph_swing_live_flush(false);
        s_force_redraw = true;
        return true;
    }

    if (s_graph_amp_mode) {
        if (s_graph_eg_index == 1 && graph_target_has_eg1_depth()) {
            /* EG1->cutoff depth, 0.25 oct/detent. Bipolar -8..+8; negative =
             * downward sweep. Same field as the filter editor's EG cursor. */
            float v = s_graph_fenv_edit + (float)delta * 0.25f;
            v = SEQ_CLAMP_F32(v, -8.0f, 8.0f);
            s_graph_fenv_edit  = v;
            s_graph_fenv_dirty = true;
            graph_live_push_fenv();
            s_force_redraw = true;
            return true;
        }
        /* Amp trim in 5% steps, applied live but throttled (melodic applies
         * re-emit the track). */
        float v = s_graph_amp_edit + (float)delta * 0.05f;
        v = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
        s_graph_amp_edit = v;
        s_amp_live_pending = true;
        graph_amp_live_flush(false);
        s_force_redraw = true;
        return true;
    }

    s_graph_env_dirty = true;   /* user moved an ADSR control point */
    bool adjusting = s_graph_popup.editing_value;
    graph_popup_handle_encoder(&s_graph_popup, delta);
    /* Derived-decay mode: A or S moved, so re-snap S.x. */
    graph_recompute_decay();
    graph_auto_range_check();
    /* Point actually moved (not just cursor selection): audition it. */
    if (adjusting) graph_live_push_env();
    return true;
}

/* Route a button event to the pop-up. is_long selects short vs long press.
 * Returns true if the pop-up consumed the event. On confirm/cancel the pop-up
 * is closed here. */
bool synth_ui_graph_handle_button(bool is_long)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;

    /* Encoder press while the amp/EG1-bipolar sub-mode is active commits
     * it (values are already live-pushed per turn) and returns the encoder
     * to the ADSR points - the same enter/exit symmetry every other editor
     * has. Without this, the press fell through to the popup widget and
     * toggled a hidden cursor flag while the sub-mode stayed stuck on. */
    if (s_graph_amp_mode || s_graph_swing_mode) {
        /* Exit, never advance: the press is the sub-mode's own confirm, so it
         * must not land on the next stop of MY_BUTTON_2's cycle. */
        if (s_graph_swing_mode) graph_swing_live_flush(true);
        s_graph_amp_mode   = false;
        s_graph_swing_mode = false;
        s_force_redraw = true;
        return true;
    }

    gpopup_result_t r = is_long
        ? graph_popup_handle_button_long(&s_graph_popup)
        : graph_popup_handle_button(&s_graph_popup);

    if (r == GPOPUP_RESULT_CONFIRMED) {
        ESP_LOGI(TAG, "graph popup confirmed -> writing envelope");
        graph_commit_to_env();
        graph_popup_close(&s_graph_popup);
    } else if (r == GPOPUP_RESULT_CANCELLED) {
        ESP_LOGI(TAG, "graph popup cancelled (restoring stored state)");
        graph_live_cancel_restore();
        graph_popup_close(&s_graph_popup);
    }
    return true;
}

/* Commit and close (MY_BUTTON_0 short tap). The discard path is
 * synth_ui_graph_handle_button(true), on a long press. */
bool synth_ui_graph_close_commit(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;
    ESP_LOGI(TAG, "graph editor close (commit envelope)");
    graph_commit_to_env();
    graph_popup_close(&s_graph_popup);
    s_force_redraw = true;
    return true;
}

/* ── graph pop-up: end ──────────────────────────────────────────────────── */

/* ── Filter editor ──────────────────────────────────────────────────────── */

bool         s_filter_active = false;
static filter_graph_t s_fgraph;
static seq_filter_t s_filter_edit;   /* scratch copy while editing */

/* ── LFO editor state ── */
bool       s_lfo_active = false;
static lfo_view_t s_lfo_view;
#if CONFIG_SYNTH_WIRELESS
/* Captured at open: the LFO editor targets the live voice (the Wireless page
 * is a menu overlay, not a ui_mode, so commit cannot route off seq_state). */
static bool s_lfo_live_target = false;
#endif

/* Normalisation helpers shared between open and commit. */
static float filter_hz_to_norm(float hz)
{
    hz = SEQ_CLAMP_F32(hz, FGRAPH_CUTOFF_HZ_MIN, FGRAPH_CUTOFF_HZ_MAX);
    return log2f(hz / FGRAPH_CUTOFF_HZ_MIN) /
           log2f(FGRAPH_CUTOFF_HZ_MAX / FGRAPH_CUTOFF_HZ_MIN);
}

static float filter_norm_to_hz(float norm)
{
    return FGRAPH_CUTOFF_HZ_MIN *
           powf(FGRAPH_CUTOFF_HZ_MAX / FGRAPH_CUTOFF_HZ_MIN,
                SEQ_CLAMP_F32(norm, 0.0f, 1.0f));
}

static float filter_q_to_norm(float q)
{
    float n = (q - FGRAPH_RES_MIN) / (FGRAPH_RES_MAX - FGRAPH_RES_MIN);
    return SEQ_CLAMP_F32(n, 0.0f, 1.0f);
}

static float filter_norm_to_q(float n)
{
    return FGRAPH_RES_MIN + SEQ_CLAMP_F32(n, 0.0f, 1.0f) *
           (FGRAPH_RES_MAX - FGRAPH_RES_MIN);
}

/* Which backend the FILTER editor is bound to. Unlike the graph editor (which
 * captures s_graph_target at open time) these are derived on every call off
 * seq_state.ui_mode. LIVE must be tested FIRST: the Wireless page is an
 * overlay, so the mode underneath is whichever screen the menu was opened from,
 * and a live filter opened over the drone screen would otherwise take the
 * drone's fixed-LPF24 cursor map and push to the drone's sweep.
 *
 * REFACTOR TARGET: deriving the edit target from UI state couples every editor
 * to which SCREEN is showing, when what they need is which VOICE they were
 * opened on - the same thing only for targets owning a top-level screen. Each
 * further non-screen target (second live slot, per-drum-track editor,
 * MIDI-learn) costs another predicate here plus a ladder arm at every call
 * site. The decoupled shape is s_graph_target's: bind once at open into a
 * backend vtable (get/set/preview env, env1, filter, amp, plus "has EG1 depth"
 * / "has LFO page" / "has track scope" flags), so seq_state is consulted only
 * when BINDING. That also retires s_graph_layer/s_graph_track for scopeless
 * targets. Worth doing when the third non-screen target lands. */
static bool filter_tgt_is_live(void)
{
    return synth_ui_wireless_page_is_open();
}
static bool filter_tgt_is_drone(void)
{
    return !filter_tgt_is_live() && seq_state.ui_mode == UI_MODE_DRONE;
}
static bool filter_tgt_is_drone_std(void)
{
    return !filter_tgt_is_live() && seq_state.ui_mode == UI_MODE_DRONE_STD;
}
static bool filter_tgt_is_arp(void)
{
    return !filter_tgt_is_live() && seq_state.ui_mode == UI_MODE_ARP;
}

/* True when the target plays a feedback wave (KS): the editor then exposes the
 * extra FB cursor (slot 2) for string feedback 0..1. Q stays editable - AMY
 * runs the biquad on KS oscs like any other wave. Covers the melodic and arp
 * KS patch; the drones exclude KS from their cycles. */
static bool filter_target_is_feedback(void)
{
    /* Live voice is always a patch; KS feedback is not one of its cursors. */
    if (filter_tgt_is_live()) return false;
    if (filter_tgt_is_arp()) {
        return arp_get_patch() == SEQ_PATCH_KS;
    }
    if (filter_tgt_is_drone() || filter_tgt_is_drone_std()) {
        return false;
    }
    /* Per-row patch: key on the cursor row, as the drum branches already do. */
    return sequencer_core_get_melodic_track_patch(seq_state.active_layer_idx,
                                                  seq_state.selected_track)
           == SEQ_PATCH_KS;
}

#if CONFIG_FILTER_SCOPE
/* Last list handed to filter_scope_arm(), so the per-frame rebind is a
 * comparison rather than a re-arm. 0xFF = cache invalid, force a rebind. */
static uint16_t s_scope_oscs[FILTER_SCOPE_MAX_OSCS];
static uint8_t  s_scope_n = 0xFF;

/* Last live cutoff, held across short silences. The engine only evaluates a
 * filter while a voice is audible, so there is nothing to read between notes -
 * and snapping to the authored cutoff made slow LFO sweeps read as jumps, since
 * the sweep advances mostly BETWEEN notes. */
#define FILTER_SCOPE_HOLD_FRAMES 40   /* ~2 s at the 20 Hz UI rate */
static float    s_scope_hold_norm;
static uint16_t s_scope_hold_age = UINT16_MAX;   /* MAX = no held value */

/* Disarm and invalidate the cache together - always pair them. A bare disarm
 * leaves the cache describing an unarmed list, and the next rebind would
 * compare equal and decline to re-arm. */
static void filter_scope_drop(void)
{
    filter_scope_disarm();
    s_scope_n = 0xFF;
    s_scope_hold_age = UINT16_MAX;
}

/* Bind the live overlay to the oscillators of the target being edited. Called
 * on open and on anything that can rebuild the target's voices: a cached
 * oscillator index survives a patch change while the slot behind it may not,
 * and the consumer of that index is the render task.
 *
 * The drones are deliberately not armed - the stutter drone's editor shows a
 * sweep MIDPOINT, not a cutoff, so a live band would not match the curve. */
static void filter_scope_bind_target(void)
{
    uint8_t slot = 0;

    if (filter_tgt_is_live()) {
#if CONFIG_SYNTH_WIRELESS
        slot = live_play_synth_slot();
#endif
    } else if (filter_tgt_is_arp()) {
        slot = SEQ_ARP_SYNTH;
    } else if (filter_tgt_is_drone() || filter_tgt_is_drone_std()) {
        filter_scope_drop();
        return;
    } else {
        slot = sequencer_core_get_track_synth(seq_state.active_layer_idx,
                                              seq_state.selected_track);
    }

    if (slot == 0) {
        filter_scope_drop();
        return;
    }

    uint16_t voices[MAX_VOICES_PER_INSTRUMENT];
    int nv = instrument_get_num_voices(slot, voices);
    if (nv <= 0) {
        filter_scope_drop();
        return;
    }

    /* Oscillator 0 of each voice is the audible carrier that owns the filter;
     * the rest are mod/algo sources. A voice's base osc IS its oscillator 0,
     * since event osc numbers are relative to the base. */
    uint16_t oscs[FILTER_SCOPE_MAX_OSCS];
    uint8_t  n = 0;
    for (int i = 0; i < nv && n < FILTER_SCOPE_MAX_OSCS; i++) {
        uint16_t base;
        if (amy_voice_base_osc(voices[i], &base)) {
            oscs[n++] = base;
        }
    }

    /* Re-arm only on an actual change. Running every UI frame keeps a voice
     * rebuild under the open popup (project load, chord-preset reallocation)
     * from leaving the render tap on another target's oscillators, but
     * re-arming unconditionally would clear the armed flag every frame and cost
     * the tap a block each time. */
    if (n == s_scope_n && memcmp(oscs, s_scope_oscs, n * sizeof(oscs[0])) == 0) {
        return;
    }
    memcpy(s_scope_oscs, oscs, n * sizeof(oscs[0]));
    s_scope_n = n;
    filter_scope_arm(oscs, n);
}

/* Drive the graph's cutoff from the live modulated value, so curve and cursor
 * move as EG1/LFO act on it. Frozen only while a value is being input on the
 * Hz/Q fields; elsewhere, or with nothing sounding, the authored value shows.
 *
 * Column quantisation is not cosmetic: filter_view_signature() hashes the whole
 * struct, so an unrounded float would hash differently every frame and redraw
 * continuously while nothing visibly moved. */
static void filter_sync_live_band(void)
{
    float norm = filter_hz_to_norm(s_filter_edit.cutoff_hz);   /* authored */

    const bool frozen = s_fgraph.editing &&
                        (s_fgraph.cursor == 0 || s_fgraph.cursor == 1);

    float lo_lf, hi_lf;
    bool  live = false;
    if (!frozen && filter_scope_read(&lo_lf, &hi_lf)) {
        /* Midpoint of the min/max window since the last frame: tracks EG1 and
         * slow LFOs faithfully, and fast LFOs wobble around their centre
         * instead of aliasing into a strobe. */
        norm = filter_hz_to_norm(freq_of_logfreq(0.5f * (lo_lf + hi_lf)));
        live = true;
        s_scope_hold_norm = norm;
        s_scope_hold_age  = 0;
    } else if (frozen) {
        /* Keep draining so the accumulator window is fresh the frame after the
         * edit ends. Drop the hold: the authored value is now the reference. */
        (void)filter_scope_read(NULL, NULL);
        s_scope_hold_age = UINT16_MAX;
    } else if (s_scope_hold_age < FILTER_SCOPE_HOLD_FRAMES) {
        /* Silence gap: nothing audible, so no cutoff was computed this frame.
         * Show the last live one instead of snapping back. */
        norm = s_scope_hold_norm;
        s_scope_hold_age++;
    }

    /* EG1 can push the live cutoff past the graph's Hz range; clamp so the
     * cursor pegs at the plot edge instead of wrapping. */
    norm = SEQ_CLAMP_F32(norm, 0.0f, 1.0f);
    const float cols = 127.0f;
    s_fgraph.cutoff_norm = (float)(int)(norm * cols + 0.5f) / cols;

    /* ~1 Hz data-flow diagnostic (UI task, debug level only). */
    static uint8_t s_scope_dbg;
    if (++s_scope_dbg >= 20) {
        s_scope_dbg = 0;
        ESP_LOGD(TAG, "scope: live=%d frozen=%d norm=%.3f",
                 (int)live, (int)frozen, (double)s_fgraph.cutoff_norm);
    }
}
#endif /* CONFIG_FILTER_SCOPE */

/* UI-only cycle order for the encoder's filter-type row. The SEQ_FILTER_* /
 * AMY FILTER_* values are ABI (persisted in snapshots, sent to the engine)
 * and stay untouched - this table only reorders how the encoder steps
 * through them: most-used first, kindred types adjacent, and no hazardous
 * neighbor pairs (stepping LPF24<->LPF must never pass through HPF/NOTCH,
 * which would dump full-level highs into the speakers mid-turn). NONE is
 * not a member: it is reached via the separate enable toggle. */
static const uint8_t s_filter_ui_order[] = {
    SEQ_FILTER_LPF24, SEQ_FILTER_LPF, SEQ_FILTER_PHASER,
    SEQ_FILTER_NOTCH, SEQ_FILTER_HPF, SEQ_FILTER_BPF,
};
#define FILTER_UI_ORDER_COUNT \
    ((int)(sizeof(s_filter_ui_order) / sizeof(s_filter_ui_order[0])))

/* Populate s_fgraph from s_filter_edit and the current graph target. */
static void filter_sync_fgraph(void)
{
    s_fgraph.filter_type    = s_filter_edit.filter_type;
    s_fgraph.cutoff_norm    = filter_hz_to_norm(s_filter_edit.cutoff_hz);
    s_fgraph.has_feedback   = filter_target_is_feedback();
    s_fgraph.resonance_norm = filter_q_to_norm(s_filter_edit.resonance);
    s_fgraph.feedback_norm  = SEQ_CLAMP_F32(s_filter_edit.feedback, 0.0f, 1.0f);
    s_fgraph.enabled        = s_filter_edit.enabled;
    /* cursor and editing stay unchanged */
}

/* Read the current filter state from the active target into s_filter_edit. */
static void filter_load_from_target(void)
{
    seq_filter_t f = {0};
#if CONFIG_SYNTH_WIRELESS
    if (synth_ui_wireless_page_is_open()) {
        /* Same never-authored sentinel as the arp/melodic rows. Tested before
         * the ui_mode ladder: the Wireless page is an overlay. */
        live_play_get_filter(&f);
        if (f.cutoff_hz <= 0.0f) {
            f.filter_type = SEQ_FILTER_LPF24;
            f.cutoff_hz   = 800.0f;
            f.resonance   = 1.0f;
            f.enabled     = false;
        }
        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "LIVE");
    } else
#endif
    if (filter_tgt_is_drone()) {
        /* Drone: always LPF24, cutoff = sweep midpoint, Q from drone_get_resonance. */
        float lo = drone_get_sweep_lo();
        float hi = drone_get_sweep_hi();
        f.filter_type = 4;   /* SEQ_FILTER_LPF24 */
        f.cutoff_hz   = SEQ_CLAMP_F32((lo + hi) * 0.5f, FGRAPH_CUTOFF_HZ_MIN, FGRAPH_CUTOFF_HZ_MAX);
        f.resonance   = drone_get_resonance();
        f.enabled     = true;

        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "STUTR");
    } else if (filter_tgt_is_drone_std()) {
        drone_std_get_filter(&f);
        /* Never-authored: seed sensible starting values but read "OFF" until
         * the user enables. */
        if (f.cutoff_hz <= 0.0f) {
            f.filter_type = SEQ_FILTER_LPF24;
            f.cutoff_hz   = 1200.0f;
            f.resonance   = 1.0f;
            f.enabled     = false;
        }
        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "DRONE");
    } else if (filter_tgt_is_arp()) {
        arp_get_filter(&f);
        /* cutoff_hz == 0 (zero-init) means never authored; only then apply
         * display defaults, so an authored-but-disabled filter round-trips.
         * Defaults open disabled - they are just a starting point. */
        if (f.cutoff_hz <= 0.0f) {
            f.filter_type = SEQ_FILTER_LPF24;
            f.cutoff_hz   = 800.0f;
            f.resonance   = 1.0f;
            f.enabled     = false;
        }
        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "ARP");
    } else {
        uint8_t li = seq_state.active_layer_idx;
        uint8_t tr = seq_state.selected_track;
        sequencer_core_get_melodic_filter(li, tr, &f);
        /* Same sentinel: zero cutoff = never authored. */
        if (f.cutoff_hz <= 0.0f) {
            f.filter_type = SEQ_FILTER_LPF24;
            f.cutoff_hz   = 800.0f;
            f.resonance   = 1.0f;
            f.enabled     = false;
        }
        char badge[4];
        editor_src_badge(li, tr, SEQ_VP_GROUP_FILTER, badge);
        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "L%u T%u%s",
                 (unsigned)(li + 1), (unsigned)(tr + 1), badge);
    }
    /* The stutter drone's filter is a fixed always-on LPF24, so it hides the
     * type/enable header controls; melodic + arp expose both. */
    s_fgraph.show_toggles = (!filter_tgt_is_drone());
    /* KS: zero feedback means never authored - seed the engine's build-time
     * default so the FB readout does not open at a silent-string 0%. */
    if (filter_target_is_feedback() && f.feedback <= 0.0f) {
        f.feedback = 0.9f;
    }
    s_filter_edit = f;
}

/* ── Filter live preview ─────────────────────────────────────────────────────
 * Same model as the envelope editor: edits push the scratch filter to AMY only;
 * cancel re-pushes the store (or reloads the layer when a touched melodic row
 * was never authored). The stutter drone has no preview path - its sweep
 * window/resonance ARE live state - so they are snapshotted at open. */
static bool  s_filter_live = false;      /* any live push this session       */
static float s_fdrone_open_lo, s_fdrone_open_hi, s_fdrone_open_res;

static void filter_live_push(void)
{
    if (filter_tgt_is_drone()) {
        /* Same midpoint math as commit: move the sweep window, keep width. */
        float lo   = drone_get_sweep_lo();
        float hi   = drone_get_sweep_hi();
        float half = (hi - lo) * 0.5f;
        float mid  = filter_norm_to_hz(s_fgraph.cutoff_norm);
        drone_set_sweep_lo(SEQ_CLAMP_F32(mid - half, 65.0f, 7900.0f));
        drone_set_sweep_hi(SEQ_CLAMP_F32(mid + half, 100.0f, 8000.0f));
        drone_set_resonance(s_filter_edit.resonance);
    } else if (filter_tgt_is_drone_std()) {
        drone_std_preview_filter(&s_filter_edit);
    } else if (filter_tgt_is_arp()) {
        arp_preview_filter(&s_filter_edit);
#if CONFIG_SYNTH_WIRELESS
    } else if (filter_tgt_is_live()) {
        live_play_preview_filter(&s_filter_edit);
#endif
    } else {
        uint8_t li = seq_state.active_layer_idx;
        uint8_t peers[SEQ_TRACKS];
        uint8_t n = sequencer_core_melodic_vp_peers(li, seq_state.selected_track, peers);
        for (uint8_t i = 0; i < n; ++i)
            sequencer_core_preview_melodic_filter(li, peers[i], &s_filter_edit);
    }
    s_filter_live = true;
}

static void filter_live_cancel_restore(void)
{
    if (!s_filter_live) return;
    s_filter_live = false;

    if (filter_tgt_is_drone()) {
        drone_set_sweep_lo(s_fdrone_open_lo);
        drone_set_sweep_hi(s_fdrone_open_hi);
        drone_set_resonance(s_fdrone_open_res);
        return;
    }
    if (filter_tgt_is_drone_std()) {
        /* Re-push the stored filter (bypass when it was never enabled). */
        seq_filter_t f;
        drone_std_get_filter(&f);
        drone_std_preview_filter(&f);
        return;
    }
    if (filter_tgt_is_arp()) {
        /* Best effort: a never-authored PATCH-mode arp keeps the previewed
         * bypass until its next patch reload. */
        seq_filter_t f;
        arp_get_filter(&f);
        arp_preview_filter(&f);
        return;
    }
#if CONFIG_SYNTH_WIRELESS
    if (filter_tgt_is_live()) {
        /* Same best-effort as the arp: always a patch, so a never-authored
         * filter keeps the previewed bypass until the next patch load. */
        seq_filter_t f;
        live_play_get_filter(&f);
        live_play_preview_filter(&f);
        return;
    }
#endif
    uint8_t li = seq_state.active_layer_idx;
    uint8_t tr = seq_state.selected_track;
    if (!sequencer_core_melodic_filter_authored(li, tr)) {
        sequencer_core_reload_layer_synth(li);
        sequencer_core_preview_melodic_clear();
        return;
    }
    uint8_t peers[SEQ_TRACKS];
    uint8_t n = sequencer_core_melodic_vp_peers(li, tr, peers);
    for (uint8_t i = 0; i < n; ++i) {
        seq_filter_t f;
        if (sequencer_core_get_melodic_filter(li, peers[i], &f))
            sequencer_core_preview_melodic_filter(li, peers[i], &f);
    }
    /* The restore above re-armed the slot with stored values; drop it so the
     * LFO service goes back to reading the store directly. */
    sequencer_core_preview_melodic_clear();
}

bool synth_ui_filter_is_active(void)
{
    return s_filter_active;
}

void synth_ui_filter_open(void)
{
    filter_load_from_target();
    s_fgraph.cursor  = 0;
    s_fgraph.editing = false;
    /* Drone filter is always LPF24 (type fixed) - skip the type cursor. */
    s_fgraph.enabled = s_filter_edit.enabled;
    filter_sync_fgraph();
    s_filter_live  = false;
    s_filter_dirty = false;
    if (filter_tgt_is_drone()) {
        s_fdrone_open_lo  = drone_get_sweep_lo();
        s_fdrone_open_hi  = drone_get_sweep_hi();
        s_fdrone_open_res = drone_get_resonance();
    }
    s_filter_active = true;
    s_force_redraw  = true;
#if CONFIG_FILTER_SCOPE
    filter_scope_drop();            /* force a fresh bind for the new target */
    filter_scope_bind_target();
#endif
    ESP_LOGI(TAG, "filter editor open: %s type%u %.0fHz Q%.2f",
             s_fgraph.label, s_filter_edit.filter_type,
             (double)s_filter_edit.cutoff_hz, (double)s_filter_edit.resonance);
}

bool synth_ui_filter_handle_encoder(long delta)
{
    if (!s_filter_active) return false;
    bool drone = (filter_tgt_is_drone());

    /* Arp takes the same cursor map and edit branches as melodic, so it needs
     * no predicate: its EG1 sweep depth is the fixed ARP_FILTER_EG1_DEPTH_OCT,
     * not an editable field. Kept as the hook if that ever changes. */
    /* bool arp = (filter_tgt_is_arp()); */

    if (!s_fgraph.editing) {
        /* Cursor map. Drone: 0=cutoff 1=resonance (type fixed, no EN).
         * Arp/melodic: 0=cutoff 1=resonance 2=feedback 3=type 4=enable, where
         * slot 2 exists on KS targets only. EG1 depth/polarity live on the
         * envelope editor's EG1 page. */
        uint8_t max_cursor = drone ? 1 : 4;
        int dir = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
        if (dir != 0) {
            uint8_t c = s_fgraph.cursor;
            do {
                c = (uint8_t)((c + (uint8_t)(max_cursor + 1) + dir) % (max_cursor + 1));
            } while (c == 2 && !s_fgraph.has_feedback);
            s_fgraph.cursor = c;
        }
        s_force_redraw = true;
        return true;
    }

    /* Editing: adjust the selected parameter. */
    switch (s_fgraph.cursor) {
        case 0: {   /* cutoff */
            /* One semitone per detent: the norm axis is log2-mapped over
             * FGRAPH_CUTOFF_OCTAVES, so this step multiplies the cutoff by
             * exactly 2^(1/12). Fast spins batch detents into one delta. */
            float step = (1.0f / (12.0f * FGRAPH_CUTOFF_OCTAVES)) * (float)delta;
            s_fgraph.cutoff_norm = SEQ_CLAMP_F32(s_fgraph.cutoff_norm + step, 0.0f, 1.0f);
            s_filter_edit.cutoff_hz = filter_norm_to_hz(s_fgraph.cutoff_norm);
            break;
        }
        case 1: {   /* resonance */
            float step = 0.02f * (float)delta;
            s_fgraph.resonance_norm = SEQ_CLAMP_F32(s_fgraph.resonance_norm + step, 0.0f, 1.0f);
            s_filter_edit.resonance = filter_norm_to_q(s_fgraph.resonance_norm);
            break;
        }
        case 2: {   /* KS string feedback (feedback targets only) */
            float step = 0.02f * (float)delta;
            s_fgraph.feedback_norm = SEQ_CLAMP_F32(s_fgraph.feedback_norm + step, 0.0f, 1.0f);
            s_filter_edit.feedback = s_fgraph.feedback_norm;
            break;
        }
        case 3: {   /* type (melodic/arp only) */
            if (!drone) {
                /* Step through the UI order table, not the raw enum; wrap
                 * both ways. An unknown stored value resolves to slot 0. */
                int idx = 0;
                for (int i = 0; i < FILTER_UI_ORDER_COUNT; i++) {
                    if (s_filter_ui_order[i] == s_filter_edit.filter_type) {
                        idx = i;
                        break;
                    }
                }
                idx = (idx + (int)delta) % FILTER_UI_ORDER_COUNT;
                if (idx < 0) idx += FILTER_UI_ORDER_COUNT;
                s_filter_edit.filter_type = s_filter_ui_order[idx];
                s_fgraph.filter_type      = s_filter_ui_order[idx];
            }
            break;
        }
        case 4: {   /* enable toggle (melodic/arp only; drone filter is always on) */
            if (!drone) {
                s_filter_edit.enabled = !s_filter_edit.enabled;
                s_fgraph.enabled      = s_filter_edit.enabled;
                ESP_LOGI(TAG, "filter enabled -> %d", (int)s_filter_edit.enabled);
            }
            break;
        }
        default: break;
    }
    s_filter_dirty = true;
    filter_sync_fgraph();
    filter_live_push();
    s_force_redraw = true;
    return true;
}

bool synth_ui_filter_handle_button(bool is_long)
{
    if (!s_filter_active) return false;
    if (is_long) {
        /* Long press = cancel: restore the target's state, reload display. */
        filter_live_cancel_restore();
        filter_load_from_target();
        filter_sync_fgraph();
        s_filter_active = false;
        s_force_redraw  = true;
#if CONFIG_FILTER_SCOPE
        filter_scope_drop();
#endif
        ESP_LOGI(TAG, "filter editor cancelled");
        return true;
    }
    /* Short press: toggle editing on/off for the current cursor. */
    s_fgraph.editing = !s_fgraph.editing;
    s_force_redraw = true;
    return true;
}

void synth_ui_filter_toggle_enabled(void)
{
    if (!s_filter_active) return;
    s_filter_edit.enabled = !s_filter_edit.enabled;
    s_fgraph.enabled      = s_filter_edit.enabled;
    s_filter_dirty        = true;
    filter_live_push();
    s_force_redraw = true;
    ESP_LOGI(TAG, "filter enabled -> %d", (int)s_filter_edit.enabled);
}

bool synth_ui_filter_close_commit(void)
{
    if (!s_filter_active) return false;

#if CONFIG_FILTER_SCOPE
    /* Stop the render-side tap first: the commit below may rebuild the target's
     * voices, leaving the armed oscillator list pointing at foreign slots. */
    filter_scope_drop();
#endif

#if CONFIG_SYNTH_WIRELESS
    if (synth_ui_wireless_page_is_open()) {
        live_play_set_filter(&s_filter_edit);
        ESP_LOGI(TAG, "filter commit live: type%u %.0fHz Q%.2f en=%d",
                 s_filter_edit.filter_type,
                 (double)s_filter_edit.cutoff_hz, (double)s_filter_edit.resonance,
                 (int)s_filter_edit.enabled);
    } else
#endif
    if (filter_tgt_is_drone()) {
        /* Drone: update resonance + move sweep range to new midpoint. */
        float lo = drone_get_sweep_lo();
        float hi = drone_get_sweep_hi();
        float half = (hi - lo) * 0.5f;
        float new_mid = filter_norm_to_hz(s_fgraph.cutoff_norm);
        drone_set_sweep_lo(SEQ_CLAMP_F32(new_mid - half, 65.0f, 7900.0f));
        drone_set_sweep_hi(SEQ_CLAMP_F32(new_mid + half, 100.0f, 8000.0f));
        drone_set_resonance(s_filter_edit.resonance);
        ESP_LOGI(TAG, "filter commit drone: sweepMid=%.0f Q=%.2f",
                 (double)new_mid, (double)s_filter_edit.resonance);
    } else if (filter_tgt_is_drone_std()) {
        drone_std_set_filter(&s_filter_edit);
        ESP_LOGI(TAG, "filter commit drone_std: type%u %.0fHz Q%.2f en=%d",
                 s_filter_edit.filter_type,
                 (double)s_filter_edit.cutoff_hz, (double)s_filter_edit.resonance,
                 (int)s_filter_edit.enabled);
    } else if (filter_tgt_is_arp()) {
        arp_set_filter(&s_filter_edit);
        ESP_LOGI(TAG, "filter commit arp: type%u %.0fHz Q%.2f",
                 s_filter_edit.filter_type,
                 (double)s_filter_edit.cutoff_hz, (double)s_filter_edit.resonance);
    } else if (s_filter_dirty) {
        /* Untouched: leave the group's authority where it was (the patch's,
         * or the block's earlier commit). */
        sequencer_core_set_melodic_filter(seq_state.active_layer_idx,
                                          seq_state.selected_track, &s_filter_edit);
    } else if (s_filter_live) {
        /* A previewed-then-reverted edit (e.g. enable toggled twice) still
         * pushed scratch state; put the store back. */
        filter_live_cancel_restore();
    }
    sequencer_core_preview_melodic_clear();
    s_filter_active = false;
    s_force_redraw  = true;
    return true;
}

void synth_ui_editors_live_service(void)
{
    graph_amp_live_flush(false);
    graph_swing_live_flush(false);

#if CONFIG_FILTER_SCOPE
    /* Drain the live band once per UI frame, before the view signature is
     * taken. It cannot live inside filter_view_signature(): signature functions
     * are side-effect-free by contract (synth_ui_internal.h), and hiding a
     * state bump there would tie the scope's read cadence to how often the
     * redraw gate hashes this view. */
    if (s_filter_active) {
        filter_scope_bind_target();   /* cheap no-op unless the voices changed */
        filter_sync_live_band();
    }
#endif
}

/* ── Filter editor: end ──────────────────────────────────────────────────── */

/* ── LFO editor ──────────────────────────────────────────────────────────── */

uint32_t lfo_view_signature(void)
{
    /* The wobble-inert marker is re-derived every frame (one cheap
     * native-layout check) and hashed, so a patch change under the open
     * editor flips the strike and redraws - no open-time snapshot to go
     * stale. WOBBLE only exists on the native carrier (voice_config.c);
     * the software steppers never implemented it. */
    bool native;
#if CONFIG_SYNTH_WIRELESS
    if (s_lfo_live_target)
        native = live_play_lfo_native_eligible();
    else
#endif
    if (seq_state.ui_mode == UI_MODE_ARP)
        /* Same predicate as the arp's own LFO routing (arp_rebuild). */
        native = sequencer_core_lfo_native_layout(arp_get_patch(), NULL, NULL);
    else if (seq_state.ui_mode == UI_MODE_DRONE_STD)
        native = true;   /* drone_std always drives the native applier */
    else
        native = sequencer_core_get_layer_type(s_lfo_view.layer_idx)
                     == SEQ_LAYER_MELODIC
              && sequencer_core_lfo_native_layout(
                     sequencer_core_get_melodic_track_patch(s_lfo_view.layer_idx,
                                                            s_lfo_view.track_idx),
                     NULL, NULL);
    s_lfo_view.wob_native = native;

    /* Distortion drive/mix have COEF_MOD rails now, so every domain can drive
     * them - natively on carrier patches (drone_std included), via the 20 Hz
     * stepper on PATCH-mode tracks. Nothing is inert; the flag stays plumbed
     * for the display struct but is never set. */
    s_lfo_view.dist_inert = false;

    const seq_lfo_t *l = &s_lfo_view.lfo;
    return (uint32_t)l->enabled
         | ((uint32_t)l->wave    <<  1)   /* 3 bits */
         | ((uint32_t)l->rate    <<  4)   /* 4 bits (0..11) */
         | ((uint32_t)l->depth   <<  8)   /* 7 bits (0..100) */
         | ((uint32_t)l->targets << 15)   /* 7 bits */
         | ((uint32_t)s_lfo_view.cursor  << 22)   /* 4 bits (0..14) */
         | ((uint32_t)s_lfo_view.editing << 26)
         | ((uint32_t)(native ? 1u : 0u) << 27)
         /* bit 28 (dist_inert) retired - dist is never inert now */
         | ((uint32_t)s_lfo_view.tgt_tab << 29);
}

bool synth_ui_lfo_is_active(void) { return s_lfo_active; }

void synth_ui_lfo_open(void)
{
#if CONFIG_SYNTH_WIRELESS
    /* Wireless page first, before the mode ladder - it is a menu overlay. The
     * live voice always takes the editor: wave patches drive the native
     * carrier, patch strings the 20 Hz stepper (live_play_lfo_service). */
    s_lfo_live_target = synth_ui_wireless_page_is_open();
    if (!s_lfo_live_target)
#endif
    {
        // TODO: drone LFO = BPM-multiplier stutter only; implement constrained editor when needed
        if (seq_state.ui_mode == UI_MODE_DRONE) return; // drone has no free LFO editor
    }
    uint8_t li = seq_state.active_layer_idx;
    uint8_t tr = seq_state.selected_track;
    seq_lfo_t existing = {
        .enabled = false,
        .mode    = LFO_MODE_FREE,
        .wave    = LFO_WAVE_SINE,
        .rate    = LFO_RATE_1BAR,
        .depth   = 50,
        .targets = LFO_TGT_BIT(LFO_TARGET_FILTER),
    };
#if CONFIG_SYNTH_WIRELESS
    if (s_lfo_live_target)
        live_play_get_lfo(&existing);
    else
#endif
    if (seq_state.ui_mode == UI_MODE_ARP)
        arp_get_lfo(&existing);
    else if (seq_state.ui_mode == UI_MODE_DRONE_STD)
        drone_std_get_lfo(&existing);
    else
        sequencer_core_get_melodic_lfo(li, tr, &existing);
    s_lfo_view.lfo          = existing;
    s_lfo_view.cursor       = 0;
    s_lfo_view.tgt_tab      = 0;
    s_lfo_view.editing      = false;
    s_lfo_view.layer_idx    = li;
    s_lfo_view.track_idx    = tr;
    s_lfo_view.target_label = (seq_state.ui_mode == UI_MODE_ARP)       ? "ARP"
                            : (seq_state.ui_mode == UI_MODE_DRONE_STD) ? "DRONE"
                            : NULL;
#if CONFIG_SYNTH_WIRELESS
    if (s_lfo_live_target) s_lfo_view.target_label = "LIVE";
#endif
    s_lfo_view.src_badge[0] = '\0';
    if (s_lfo_view.target_label == NULL)
        editor_src_badge(li, tr, SEQ_VP_GROUP_LFO, s_lfo_view.src_badge);
    s_lfo_dirty    = false;
    s_lfo_active   = true;
    s_force_redraw = true;
    ESP_LOGI(TAG, "LFO editor open L%u T%u", li + 1u, tr + 1u);
}

/* Live audition for the LFO editor - melodic tracks only (arp/drone/live
 * keep their commit-on-close behavior; their setters ARE their appliers and
 * every exit commits). Single-track even when the row reads the layer block:
 * the selected track is the audition voice, the other readers follow at
 * commit. */
static void lfo_live_push_preview(void)
{
#if CONFIG_SYNTH_WIRELESS
    if (s_lfo_live_target) return;
#endif
    if (seq_state.ui_mode == UI_MODE_ARP || seq_state.ui_mode == UI_MODE_DRONE ||
        seq_state.ui_mode == UI_MODE_DRONE_STD)
        return;
    sequencer_core_preview_melodic_lfo(s_lfo_view.layer_idx,
                                       s_lfo_view.track_idx,
                                       &s_lfo_view.lfo);
}

/* Cancel path: drop the preview slot and re-push the committed state. */
static void lfo_preview_cancel_restore(void)
{
    sequencer_core_preview_melodic_clear();
#if CONFIG_SYNTH_WIRELESS
    if (s_lfo_live_target) return;
#endif
    if (seq_state.ui_mode == UI_MODE_ARP || seq_state.ui_mode == UI_MODE_DRONE ||
        seq_state.ui_mode == UI_MODE_DRONE_STD)
        return;
    sequencer_core_reapply_melodic_lfo(s_lfo_view.layer_idx, s_lfo_view.track_idx);
}

/* True when a cursor field is on screen. Only the shown target tab's checkbox
 * rows are - the shared parameter rows always are, which is what makes the
 * skip-ahead loop below terminate. */
static bool lfo_cursor_visible(uint8_t c)
{
    if (c >= LFO_TARGET_COUNT) return true;
    return (c >= LFO_TARGET_DIST_DRIVE) == (s_lfo_view.tgt_tab != 0);
}

void synth_ui_lfo_toggle_target_tab(void)
{
    if (!s_lfo_active) return;
    s_lfo_view.tgt_tab = s_lfo_view.tgt_tab ? 0u : 1u;
    /* A cursor left on the hidden tab's checkbox would be invisible and would
     * toggle an off-screen target, so land it on the new tab's first row. */
    if (!lfo_cursor_visible(s_lfo_view.cursor))
        s_lfo_view.cursor = s_lfo_view.tgt_tab ? (uint8_t)LFO_TARGET_DIST_DRIVE : 0u;
    s_force_redraw = true;
}

bool synth_ui_lfo_handle_encoder(long delta)
{
    if (!s_lfo_active) return false;
    /* Fields: the shown tab's target checkboxes (< LFO_TARGET_COUNT), then
     * WAVE/RATE/DEPTH/EN. Wraps in both directions over the visible set. */
    const uint8_t N = LFO_FLD_COUNT;
    if (!s_lfo_view.editing) {
        if (delta != 0) {
            uint8_t c = s_lfo_view.cursor;
            do {
                c = (delta > 0) ? (uint8_t)((c + 1u) % N)
                                : (uint8_t)((c + N - 1u) % N);
            } while (!lfo_cursor_visible(c));
            s_lfo_view.cursor = c;
        }
        s_force_redraw = true;
        return true;
    }
    /* Only multi-value fields use adjust mode; checkbox and EN fields toggle on
     * press and never set `editing`. */
    seq_lfo_t *l = &s_lfo_view.lfo;
    int d = (delta > 0) ? 1 : -1;
    switch (s_lfo_view.cursor) {
        case LFO_FLD_WAVE:
            l->wave = (lfo_wave_t)((l->wave + LFO_WAVE_COUNT + d) % LFO_WAVE_COUNT);
            break;
        case LFO_FLD_RATE:
            l->rate = (lfo_rate_t)((l->rate + LFO_RATE_COUNT + d) % LFO_RATE_COUNT);
            break;
        case LFO_FLD_DEPTH: {
            /* Fine 1% steps in 0..10 (on sensitive targets like pitch even
             * 5% is a lot), 5% steps above; clamped 0..100. Stepping down
             * from 10 enters the fine range (10 -> 9), up leaves it
             * (10 -> 15). */
            int cur  = (int)l->depth;
            int next = (d > 0) ? cur + ((cur < 10) ? 1 : 5)
                               : cur - ((cur <= 10) ? 1 : 5);
            if (next < 0)   next = 0;
            if (next > 100) next = 100;
            l->depth = (uint8_t)next;
            break;
        }
        case LFO_FLD_FLT_OCT: {
            /* Quarter-octave steps. A legacy (sentinel) value materialises at
             * its current effective swing first, so the first click nudges the
             * sound instead of jumping it. */
            int q = l->flt_oct_q ? (int)l->flt_oct_q
                                 : (int)(((unsigned)l->depth * 12u + 50u) / 100u);
            q = SEQ_CLAMP_INT(q + d, 1, (int)VOICE_LFO_FLT_OCT_Q_MAX);
            l->flt_oct_q = (uint8_t)q;
            break;
        }
        case LFO_FLD_WOB_RATE:
            l->wob_rate = (uint8_t)((l->wob_rate + LFO_RATE_COUNT + d) % LFO_RATE_COUNT);
            break;
        case LFO_FLD_WOB_DEPTH: {
            /* Authored in whole dB of carrier swing (see voice_config.h);
             * 0 dB is the OFF step at the bottom of the range, not a wrap. */
            int db = (int)voice_wob_depth_to_db(l->wob_depth) + d;
            db = SEQ_CLAMP_INT(db, 0, (int)VOICE_WOB_DB_MAX);
            l->wob_depth = voice_wob_db_to_depth((uint8_t)db);
            break;
        }
        default: break;
    }
    s_lfo_dirty = true;
    lfo_live_push_preview();
    s_force_redraw = true;
    return true;
}

bool synth_ui_lfo_handle_button(bool is_long)
{
    if (!s_lfo_active) return false;
    if (is_long) {
        lfo_preview_cancel_restore();
        s_lfo_active   = false;
        s_force_redraw = true;
        ESP_LOGI(TAG, "LFO editor cancelled");
        return true;
    }
    seq_lfo_t *l = &s_lfo_view.lfo;
    uint8_t c = s_lfo_view.cursor;
    if (c < LFO_TARGET_COUNT) {
        l->targets ^= LFO_TGT_BIT(c);          /* toggle this target in/out of the set */
        s_lfo_view.editing = false;
        s_lfo_dirty = true;
        lfo_live_push_preview();
    } else if (c == LFO_FLD_EN) {
        l->enabled = !l->enabled;              /* boolean: toggle directly */
        s_lfo_view.editing = false;
        s_lfo_dirty = true;
        lfo_live_push_preview();
    } else if (c == LFO_FLD_WOB_MODE) {
        /* 3-way reach cycle: depth+rate -> depth -> rate -> ... */
        l->wob_reach = (uint8_t)((l->wob_reach + 1u) % WOB_REACH_COUNT);
        s_lfo_view.editing = false;
        s_lfo_dirty = true;
        lfo_live_push_preview();
    } else {
        s_lfo_view.editing = !s_lfo_view.editing;  /* WAVE/RATE/DEPTH/WOB: adjust mode */
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_lfo_close_commit(void)
{
    if (!s_lfo_active) return false;
#if CONFIG_SYNTH_WIRELESS
    /* Before the mode ladder: the Wireless page is an overlay. */
    if (s_lfo_live_target) {
        live_play_set_lfo(&s_lfo_view.lfo);
        s_lfo_active   = false;
        s_force_redraw = true;
        ESP_LOGI(TAG, "LFO editor committed (live voice)");
        return true;
    }
#endif
    if (seq_state.ui_mode == UI_MODE_DRONE) return false; // stutter drone has no free LFO editor
    if (seq_state.ui_mode == UI_MODE_ARP) {
        arp_set_lfo(&s_lfo_view.lfo);
    } else if (seq_state.ui_mode == UI_MODE_DRONE_STD) {
        drone_std_set_lfo(&s_lfo_view.lfo);
    } else if (s_lfo_dirty) {
        sequencer_core_set_melodic_lfo(s_lfo_view.layer_idx,
                                       s_lfo_view.track_idx,
                                       &s_lfo_view.lfo);
    } else {
        /* Untouched: no authority taken; drop any preview arming. */
        lfo_preview_cancel_restore();
    }
    sequencer_core_preview_melodic_clear();
    s_lfo_active   = false;
    s_force_redraw = true;
    ESP_LOGI(TAG, "LFO editor committed (dirty=%d)", (int)s_lfo_dirty);
    return true;
}

/* ── LFO editor: end ─────────────────────────────────────────────────────── */

/* ── Distortion editor ───────────────────────────────────────────────────── */

bool s_dist_active;
static dist_view_t s_dist_view;
#if CONFIG_SYNTH_WIRELESS
static bool s_dist_live_target;
#endif

uint32_t dist_view_signature(void)
{
    const seq_dist_t *d = &s_dist_view.dist;
    return (uint32_t)d->type                    /* 2 bits  */
         | ((uint32_t)d->drive           <<  2) /* 5 bits  */
         | ((uint32_t)d->bits            <<  7) /* 5 bits  */
         | ((uint32_t)d->rate            << 12) /* 7 bits  */
         | ((uint32_t)d->mix             << 19) /* 7 bits  */
         | ((uint32_t)s_dist_view.cursor << 26) /* 3 bits  */
         | ((uint32_t)s_dist_view.editing << 29);
}

bool synth_ui_dist_is_active(void) { return s_dist_active; }

void synth_ui_dist_open(void)
{
#if CONFIG_SYNTH_WIRELESS
    /* Wireless page first, before the mode ladder - it is a menu overlay. */
    s_dist_live_target = synth_ui_wireless_page_is_open();
    if (!s_dist_live_target)
#endif
    {
        /* The stutter drone edits its timbre through its own screen and has no
         * filter/LFO tab worth pairing with; keep its cycle short. */
        if (seq_state.ui_mode == UI_MODE_DRONE) return;
    }
    uint8_t li = seq_state.active_layer_idx;
    uint8_t tr = seq_state.selected_track;
    seq_dist_t existing = { .type = 0, .drive = 2, .bits = 8, .rate = 8, .mix = 100 };
#if CONFIG_SYNTH_WIRELESS
    if (s_dist_live_target)
        live_play_get_dist(&existing);
    else
#endif
    if (seq_state.ui_mode == UI_MODE_ARP)
        arp_get_dist(&existing);
    else if (seq_state.ui_mode == UI_MODE_DRONE_STD)
        drone_std_get_dist(&existing);
    else
        sequencer_core_get_melodic_dist(li, tr, &existing);
    s_dist_view.dist         = existing;
    s_dist_view.cursor       = 0;
    s_dist_view.editing      = false;
    s_dist_view.layer_idx    = li;
    s_dist_view.track_idx    = tr;
    s_dist_view.target_label = (seq_state.ui_mode == UI_MODE_ARP)       ? "ARP"
                             : (seq_state.ui_mode == UI_MODE_DRONE_STD) ? "DRONE"
                             : NULL;
#if CONFIG_SYNTH_WIRELESS
    if (s_dist_live_target) s_dist_view.target_label = "LIVE";
#endif
    s_dist_view.src_badge[0] = '\0';
    if (s_dist_view.target_label == NULL)
        editor_src_badge(li, tr, SEQ_VP_GROUP_DIST, s_dist_view.src_badge);
    s_dist_dirty   = false;
    s_dist_active  = true;
    s_force_redraw = true;
    ESP_LOGI(TAG, "DIST editor open L%u T%u", li + 1u, tr + 1u);
}

/* Live audition. Unlike the LFO page every target previews: distortion has no
 * software-stepper path to arbitrate, so a preview is just a push that skips
 * the store. Single-track even when the row reads the layer block - the
 * selected track is the audition voice, the other readers follow at commit. */
static void dist_live_push_preview(void)
{
#if CONFIG_SYNTH_WIRELESS
    if (s_dist_live_target) { live_play_preview_dist(&s_dist_view.dist); return; }
#endif
    if (seq_state.ui_mode == UI_MODE_ARP)
        arp_preview_dist(&s_dist_view.dist);
    else if (seq_state.ui_mode == UI_MODE_DRONE_STD)
        drone_std_preview_dist(&s_dist_view.dist);
    else
        sequencer_core_preview_melodic_dist(s_dist_view.layer_idx,
                                            s_dist_view.track_idx,
                                            &s_dist_view.dist);
}

/* Cancel path: re-push the committed state over the auditioned one. */
static void dist_preview_cancel_restore(void)
{
#if CONFIG_SYNTH_WIRELESS
    if (s_dist_live_target) { live_play_reapply_dist(); return; }
#endif
    if (seq_state.ui_mode == UI_MODE_ARP)
        arp_reapply_dist();
    else if (seq_state.ui_mode == UI_MODE_DRONE_STD)
        drone_std_reapply_dist();
    else
        sequencer_core_reapply_melodic_dist(s_dist_view.layer_idx,
                                            s_dist_view.track_idx);
}

bool synth_ui_dist_handle_encoder(long delta)
{
    if (!s_dist_active) return false;
    const uint8_t N = DIST_FLD_COUNT;
    if (!s_dist_view.editing) {
        if (delta > 0)      s_dist_view.cursor = (s_dist_view.cursor + 1) % N;
        else if (delta < 0) s_dist_view.cursor = (s_dist_view.cursor + N - 1) % N;
        s_force_redraw = true;
        return true;
    }
    seq_dist_t *d = &s_dist_view.dist;
    int step = (delta > 0) ? 1 : -1;
    switch (s_dist_view.cursor) {
        case DIST_FLD_TYPE:
            /* 8-state stage-set cycle (singles first, then the stacks); OFF
             * is a value in the cycle, not a separate toggle. */
            d->type = seq_dist_stage_step(d->type, step);
            break;
        case DIST_FLD_DRIVE:
            d->drive = (uint8_t)SEQ_CLAMP_INT((int)d->drive + step,
                                              (int)DIST_DRIVE_MIN, (int)DIST_DRIVE_MAX);
            break;
        case DIST_FLD_BITS:
            d->bits = (uint8_t)SEQ_CLAMP_INT((int)d->bits + step,
                                             (int)DIST_BITS_MIN, (int)DIST_BITS_MAX);
            break;
        case DIST_FLD_RATE:
            d->rate = (uint8_t)SEQ_CLAMP_INT((int)d->rate + step,
                                             (int)DIST_RATE_MIN, (int)DIST_RATE_MAX);
            break;
        case DIST_FLD_MIX:
            d->mix = (uint8_t)SEQ_CLAMP_INT((int)d->mix + step * (int)DIST_MIX_STEP,
                                            0, 100);
            break;
        default: break;
    }
    s_dist_dirty = true;
    dist_live_push_preview();
    s_force_redraw = true;
    return true;
}

bool synth_ui_dist_handle_button(bool is_long)
{
    if (!s_dist_active) return false;
    if (is_long) {
        dist_preview_cancel_restore();
        s_dist_active  = false;
        s_force_redraw = true;
        ESP_LOGI(TAG, "DIST editor cancelled");
        return true;
    }
    /* Every field is multi-value, so a short press only toggles adjust mode. */
    s_dist_view.editing = !s_dist_view.editing;
    s_force_redraw = true;
    return true;
}

bool synth_ui_dist_close_commit(void)
{
    if (!s_dist_active) return false;
#if CONFIG_SYNTH_WIRELESS
    /* Before the mode ladder: the Wireless page is an overlay. */
    if (s_dist_live_target) {
        live_play_set_dist(&s_dist_view.dist);
        s_dist_active  = false;
        s_force_redraw = true;
        ESP_LOGI(TAG, "DIST editor committed (live voice)");
        return true;
    }
#endif
    if (seq_state.ui_mode == UI_MODE_DRONE) return false;
    if (seq_state.ui_mode == UI_MODE_ARP) {
        arp_set_dist(&s_dist_view.dist);
    } else if (seq_state.ui_mode == UI_MODE_DRONE_STD) {
        drone_std_set_dist(&s_dist_view.dist);
    } else if (s_dist_dirty) {
        sequencer_core_set_melodic_dist(s_dist_view.layer_idx,
                                        s_dist_view.track_idx,
                                        &s_dist_view.dist);
    } else {
        /* Untouched: no authority taken; undo any preview push. */
        dist_preview_cancel_restore();
    }
    s_dist_active  = false;
    s_force_redraw = true;
    ESP_LOGI(TAG, "DIST editor committed (dirty=%d)", (int)s_dist_dirty);
    return true;
}

/* ── Distortion editor: end ──────────────────────────────────────────────── */

/* ── Voice-block source flip / release to patch ─────────────────────────────
 * Both act on the open editor's melodic row. The pattern is the one
 * graph_toggle_eg_index() uses for the EG page switch: commit the departing
 * state through the editor's own close path (dirty-gated, so an untouched
 * tab takes no authority), change the engine state, then reopen the same
 * editor so its working copy re-seeds from the block now in effect. The
 * reopen resets cursors; the LFO/DIST tab and cursor are carried across. */

typedef enum { EDITOR_NONE, EDITOR_GRAPH, EDITOR_FILTER, EDITOR_LFO, EDITOR_DIST } editor_id_t;

static editor_id_t editor_open_melodic(void)
{
    /* The live voice has no track scope, and must be tested before the
     * ui_mode ladder: its editor runs over the menu overlay, so the mode
     * underneath would otherwise pass as MELODIC. */
    if (synth_ui_wireless_page_is_open()) return EDITOR_NONE;
    if (seq_state.ui_mode == UI_MODE_ARP || seq_state.ui_mode == UI_MODE_DRONE ||
        seq_state.ui_mode == UI_MODE_DRONE_STD)
        return EDITOR_NONE;
    if (graph_popup_is_active(&s_graph_popup))
        return (s_graph_target == GRAPH_TGT_MELODIC) ? EDITOR_GRAPH : EDITOR_NONE;
    if (s_filter_active) return EDITOR_FILTER;
    if (s_lfo_active)    return EDITOR_LFO;
    if (s_dist_active)   return EDITOR_DIST;
    return EDITOR_NONE;
}

/* Commit-and-reopen around an engine-state change on the open editor. */
static void editor_reseed_around(editor_id_t ed, void (*change)(void))
{
    switch (ed) {
        case EDITOR_GRAPH: {
            uint8_t eg = s_graph_eg_index;
            synth_ui_graph_close_commit();
            change();
            synth_ui_graph_open_envelope();
            if (eg == 1) graph_toggle_eg_index();   /* clean reseed: nothing dirty */
            break;
        }
        case EDITOR_FILTER: {
            uint8_t cursor = s_fgraph.cursor;
            synth_ui_filter_close_commit();
            change();
            synth_ui_filter_open();
            s_fgraph.cursor = cursor;
            break;
        }
        case EDITOR_LFO: {
            uint8_t cursor = s_lfo_view.cursor, tab = s_lfo_view.tgt_tab;
            synth_ui_lfo_close_commit();
            change();
            synth_ui_lfo_open();
            s_lfo_view.tgt_tab = tab;
            s_lfo_view.cursor  = cursor;
            break;
        }
        case EDITOR_DIST: {
            uint8_t cursor = s_dist_view.cursor;
            synth_ui_dist_close_commit();
            change();
            synth_ui_dist_open();
            s_dist_view.cursor = cursor;
            break;
        }
        default: break;
    }
    s_force_redraw = true;
}

static void editor_flip_source(void)
{
    uint8_t li = seq_state.active_layer_idx;
    uint8_t tr = seq_state.selected_track;
    seq_vp_src_t next = editor_src_is_layer(li, tr) ? SEQ_VP_SRC_TRACK
                                                    : SEQ_VP_SRC_LAYER;
    sequencer_core_set_melodic_vp_source(li, tr, next);
}

static seq_vp_group_t s_release_group;   /* set by the release entry point */

static void editor_release_group(void)
{
    sequencer_core_release_melodic_group(seq_state.active_layer_idx,
                                         seq_state.selected_track,
                                         s_release_group);
}

bool synth_ui_toggle_editor_source(void)
{
    editor_id_t ed = editor_open_melodic();
    if (ed == EDITOR_NONE) return false;
    if (sequencer_core_get_layer_type(seq_state.active_layer_idx) != SEQ_LAYER_MELODIC)
        return false;   /* drum layers have no shared block */
    editor_reseed_around(ed, editor_flip_source);
    ESP_LOGI(TAG, "editor source -> %s",
             editor_src_is_layer(seq_state.active_layer_idx,
                                 seq_state.selected_track) ? "LAYER" : "TRACK");
    return true;
}

bool synth_ui_editor_release_to_patch(void)
{
    editor_id_t ed = editor_open_melodic();
    if (ed == EDITOR_NONE) return false;
    if (sequencer_core_get_layer_type(seq_state.active_layer_idx) != SEQ_LAYER_MELODIC)
        return false;
    switch (ed) {
        case EDITOR_GRAPH:  s_release_group = (s_graph_eg_index == 1) ? SEQ_VP_GROUP_ENV1
                                                                      : SEQ_VP_GROUP_ENV; break;
        case EDITOR_FILTER: s_release_group = SEQ_VP_GROUP_FILTER; break;
        case EDITOR_LFO:    s_release_group = SEQ_VP_GROUP_LFO;    break;
        case EDITOR_DIST:   s_release_group = SEQ_VP_GROUP_DIST;   break;
        default: return false;
    }
    editor_reseed_around(ed, editor_release_group);
    ESP_LOGI(TAG, "editor tab released to patch (group %d)", (int)s_release_group);
    return true;
}

/* Cycle ADSR → Filter → LFO → DIST → ADSR (MY_BUTTON_3 while any editor is
 * open). UI_MODE_DRONE has neither an LFO nor a DIST tab and wraps early. */
void synth_ui_cycle_editor(void)
{
    if (graph_popup_is_active(&s_graph_popup)) {
        if (s_graph_target == GRAPH_TGT_FM_OP) {
            /* An operator has no filter/LFO/DIST of its own: the cycle is
             * one page long, so Next re-opens the same editor (committed). */
            synth_ui_graph_close_commit();
            synth_ui_graph_open_envelope();
            return;
        }
        if (s_graph_eg_index == 0 && graph_target_has_eg1()) {
            /* EG0 -> EG1: same widget, next page. Targets without an EG1 page
             * fall straight through to the filter tab. */
            graph_toggle_eg_index();
            return;
        }
        synth_ui_graph_close_commit();
        synth_ui_filter_open();
    } else if (s_filter_active) {
        synth_ui_filter_close_commit();
        bool no_lfo_tab = (seq_state.ui_mode == UI_MODE_DRONE);
#if CONFIG_SYNTH_WIRELESS
        /* The wireless overlay overrides the mode underneath and always has an
         * LFO tab (see synth_ui_lfo_open). */
        if (synth_ui_wireless_page_is_open())
            no_lfo_tab = false;
#endif
        if (no_lfo_tab)
            synth_ui_graph_open_envelope(); // skip LFO tab, cycle back to ADSR
        else
            synth_ui_lfo_open();
    } else if (s_lfo_active) {
        synth_ui_lfo_close_commit();
        /* dist_open self-gates (see there); if it declines, wrap rather than
         * leaving the cycle with no editor open. */
        synth_ui_dist_open();
        if (!s_dist_active) synth_ui_graph_open_envelope();
    } else if (s_dist_active) {
        synth_ui_dist_close_commit();
        synth_ui_graph_open_envelope();
    }
}

/* ── LFO editor: end ─────────────────────────────────────────────────────── */

/* ── View signatures ─────────────────────────────────────────────────────── */

uint32_t filter_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &s_fgraph, sizeof(s_fgraph));
    return h;
}

uint32_t graph_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &s_graph_popup.cursor, sizeof(s_graph_popup.cursor));
    h = fnv1a_bytes(h, &s_graph_popup.editing_value, sizeof(s_graph_popup.editing_value));
    h = fnv1a_bytes(h, &s_graph_popup.adjust_axis_y, sizeof(s_graph_popup.adjust_axis_y));
    h = fnv1a_bytes(h, &s_graph_long_range, sizeof(s_graph_long_range));
    h = fnv1a_bytes(h, &s_graph_layer, sizeof(s_graph_layer));
    h = fnv1a_bytes(h, &s_graph_track, sizeof(s_graph_track));
    h = fnv1a_bytes(h, &s_graph_amp_mode, sizeof(s_graph_amp_mode));
    h = fnv1a_bytes(h, &s_graph_amp_edit, sizeof(s_graph_amp_edit));
    h = fnv1a_bytes(h, &s_graph_swing_mode, sizeof(s_graph_swing_mode));
    h = fnv1a_bytes(h, &s_graph_swing_edit, sizeof(s_graph_swing_edit));
    h = fnv1a_bytes(h, &s_graph_fenv_edit, sizeof(s_graph_fenv_edit));
    h = fnv1a_bytes(h, &s_graph_eg_index, sizeof(s_graph_eg_index));
    h = fnv1a_bytes(h, &s_graph_eg_type_disp, sizeof(s_graph_eg_type_disp));
    /* Tick-derived: flips when the type-flash window closes, so the top bar
     * reverts without any other state change. */
    uint8_t type_flash = graph_type_flash_active() ? 1u : 0u;
    h = fnv1a_bytes(h, &type_flash, sizeof(type_flash));
    h = fnv1a_bytes(h, s_graph_popup.points,
                    s_graph_popup.num_points * sizeof(gpopup_point_t));
    return h;
}

/* ── Draw wrappers (encapsulate private editor state from synth_ui_task) ── */

/* Draw the yellow context top bar (rows 0..15) for the envelope editor. */
static void graph_draw_topbar(u8g2_t *u8g2)
{
    char buf[24];

    /* Left: target label plus which breakpoint generator (EG0/EG1) is shown. */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    const char *eg_tag = (s_graph_eg_index == 1) ? "EG1" : "EG0";
    if (s_graph_target == GRAPH_TGT_ARP) {
        snprintf(buf, sizeof(buf), "ARP %s", eg_tag);
    } else if (s_graph_target == GRAPH_TGT_DRONE) {
        snprintf(buf, sizeof(buf), "STUTR %s", eg_tag);
    } else if (s_graph_target == GRAPH_TGT_DRONE_STD) {
        snprintf(buf, sizeof(buf), "DRONE %s", eg_tag);
#if CONFIG_SYNTH_WIRELESS
    } else if (s_graph_target == GRAPH_TGT_LIVE) {
        snprintf(buf, sizeof(buf), "LIVE %s", eg_tag);
#endif
#if CONFIG_SYNTH_CUSTOM_FM
    } else if (s_graph_target == GRAPH_TGT_FM_OP) {
        snprintf(buf, sizeof(buf), "FM OP%u %s",
                 (unsigned)(FM_NUM_OPS - s_graph_fm_op), eg_tag);
#endif
    } else {
        char badge[4];
        editor_src_badge(s_graph_layer, s_graph_track,
                         (s_graph_eg_index == 1) ? SEQ_VP_GROUP_ENV1 : SEQ_VP_GROUP_ENV,
                         badge);
        snprintf(buf, sizeof(buf), "L%u T%u %s%s",
                 s_graph_layer + 1, s_graph_track + 1, eg_tag, badge);
    }
    u8g2_DrawStr(u8g2, 2, 8, buf);

    /* The middle point readout and the right-side trim readout share the
     * 60..126 px band, so only one may draw per frame or they overlap into
     * doubled text. Point selection is resolved first. */
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    uint8_t c = s_graph_popup.cursor;
    /* During the flash window the full type name takes the shared band. */
    bool type_flash = graph_type_flash_active();
    bool mid_shown = (!type_flash && !s_graph_amp_mode && !s_graph_swing_mode &&
                      n >= 4 && c >= 1 && c <= 3);

    /* Right: amp indicator in amp mode, layer swing in swing mode. The melodic
     * EG1 page shows the signed sweep depth instead whenever the middle readout
     * is idle, so the shoulder-button polarity flip has a visible readout. */
    uint8_t rw = 0;
    bool eg1_fenv = (s_graph_eg_index == 1 && graph_target_has_eg1_depth());
    if (type_flash) {
        /* Inverted pad so it reads as an event, not a label. */
        const char *tname = graph_eg_type_name(s_graph_eg_type_disp);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        rw = (uint8_t)u8g2_GetStrWidth(u8g2, tname);
        u8g2_DrawBox(u8g2, (uint8_t)(128 - rw - 4), 0, (uint8_t)(rw + 4), 11);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, (uint8_t)(128 - rw - 2), 8, tname);
        u8g2_SetDrawColor(u8g2, 1);
    } else if (s_graph_swing_mode) {
        char swg_buf[10];
        snprintf(swg_buf, sizeof(swg_buf), "SWG%u%%", (unsigned)s_graph_swing_edit);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        rw = (uint8_t)u8g2_GetStrWidth(u8g2, swg_buf);
        u8g2_DrawStr(u8g2, (uint8_t)(128 - rw - 2), 8, swg_buf);
    } else if (s_graph_amp_mode || (eg1_fenv && !mid_shown)) {
        char amp_buf[10];
        if (eg1_fenv) {
            snprintf(amp_buf, sizeof(amp_buf), "ENV%+.2f", (double)s_graph_fenv_edit);
        } else {
            snprintf(amp_buf, sizeof(amp_buf), "AMP%d%%",
                     (int)(s_graph_amp_edit * 100.0f + 0.5f));
        }
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        rw = (uint8_t)u8g2_GetStrWidth(u8g2, amp_buf);
        u8g2_DrawStr(u8g2, (uint8_t)(128 - rw - 2), 8, amp_buf);
    }
    /* No idle-slot fallback: the point readout owns this band whenever the
     * cursor sits on a point (i.e. always), so the persistent curve-type code
     * lives in the plot corner instead - see synth_ui_graph_view_draw. */

    /* Middle: live readout of the selected point's real value (ms / %). */
    if (mid_shown) {
        uint32_t cum_a = graph_x_to_ms(pts[1].x);
        uint32_t cum_d = graph_x_to_ms(pts[2].x);
        uint32_t cum_r = graph_x_to_ms(pts[3].x);
        if (c == 1) {
            snprintf(buf, sizeof(buf), "A %lums", (unsigned long)cum_a);
        } else if (c == 2) {
            uint32_t d = (cum_d > cum_a) ? (cum_d - cum_a) : 0;
            snprintf(buf, sizeof(buf), "D %lums S %u%%",
                     (unsigned long)d, (unsigned)(pts[2].y * 100.0f + 0.5f));
        } else {
            uint32_t r = (cum_r > cum_d) ? (cum_r - cum_d) : 0;
            snprintf(buf, sizeof(buf), "R %lums", (unsigned long)r);
        }
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, buf);
        /* Between the left label (~x=56) and the right indicator. */
        int mx = 60 + (int)((128 - 60 - (int)rw - 4 - (int)tw) / 2);
        if (mx < 60) mx = 60;
        u8g2_DrawStr(u8g2, (uint8_t)mx, 8, buf);
    }

    /* Divider at the yellow/blue boundary. */
    u8g2_DrawHLine(u8g2, 0, GRAPH_TOPBAR_H - 1, 128);
}

void synth_ui_graph_view_draw(u8g2_t *u8g2)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    graph_draw_topbar(u8g2);
    graph_popup_draw(u8g2, &s_graph_popup);

    /* Persistent curve-type code, top-right of the plot: the top bar's right
     * slot is owned by the point readout, so this is the one spot where the
     * type stays visible while editing. The cleared pad keeps it legible when
     * the curve passes underneath. */
    const char *tcode = graph_eg_type_code(s_graph_eg_type_disp);
    u8g2_SetFont(u8g2, u8g2_font_4x6_tr);
    uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, tcode);
    uint8_t tx = (uint8_t)(128 - tw - 2);
    uint8_t ty = (uint8_t)(GRAPH_TOPBAR_H + 8);
    u8g2_SetDrawColor(u8g2, 0);
    u8g2_DrawBox(u8g2, (uint8_t)(tx - 1), (uint8_t)(ty - 6), (uint8_t)(tw + 3), 8);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_DrawStr(u8g2, tx, ty, tcode);
}

void synth_ui_filter_view_draw(u8g2_t *u8g2)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    filter_graph_draw(u8g2, &s_fgraph);
}

void synth_ui_lfo_view_draw(u8g2_t *u8g2)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    lfo_view_draw(u8g2, &s_lfo_view);
}

void synth_ui_dist_view_draw(u8g2_t *u8g2)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    dist_view_draw(u8g2, &s_dist_view);
}
