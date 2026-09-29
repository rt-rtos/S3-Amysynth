#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "custompatches/fm_voice.h"
#include "synth_ui/eg_shape.h"
#include "amy.h"           /* ENVELOPE_DX7 */
#include "voice_config.h"  /* VOICE_ENV_* clamps */
#include "seq_clamp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  FM/ALGO voice editor - two pages over the selected operator
 * ════════════════════════════════════════════════════════════════════════
 * Edits the single live SEQ_PATCH_FM_CUSTOM voice (s_fm_voice, owned by
 * custompatches/fm_voice.c). Rendered by display_fm.c.
 *
 * Page 0: one flat cursor walks the six operator boxes (selecting as it goes)
 * and then the panel rows RATIO / LEVEL / TO / FB / ALGO for the selected
 * operator. Page 1: the selected operator's frequency mode, coarse and fine
 * frequency, and its 4-level envelope. Each page keeps its own cursor and
 * adjust flag; both share the selected operator. The page is UI state only.
 *
 * Link mode (page 0 only; API in synth_ui.h): s_fm_link_src is the source
 * operator, FM_OP_NONE when off. A separate link cursor walks the boxes, and
 * each Button 1 press edits the source's target set by the click rule
 * (fm_link_click_mask) through fm_voice_set_op_targets(). The selection and
 * the page 0 cursor stay put. s_fm_link_bad (boxes whose click would be
 * refused) is recomputed only when linking starts and after a successful
 * click, never in the per-tick view build: failing compiles are the costly
 * ones. Controls: CONTROLS.md. */

/* Curated DX7-style harmonic ratios plus a few inharmonic ones, kept short
 * enough to encoder through. A coarse step moves to the neighbour of the
 * nearest entry and keeps the fine offset from it as a factor. */
static const float s_fm_ratio_steps[] = {
    0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 5.0f,
    6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 14.0f, 16.0f,
};
#define FM_RATIO_STEP_COUNT ((int)(sizeof(s_fm_ratio_steps) / sizeof(s_fm_ratio_steps[0])))

#define FM_RATIO_MIN     0.25f
#define FM_RATIO_MAX     20.0f
#define FM_FIXED_HZ_MIN  1.0f
#define FM_FIXED_HZ_MAX  9772.0f
#define FM_A4_HZ         440.0f
#define FM_FINE_HZ       0.1f       /* fine step per detent */
#define FM_EG_LEVEL_MAX  99

static uint8_t s_fm_page     = 0;
static uint8_t s_fm_cursor   = FM_CUR_OP_BASE + 5;   /* start on OP1 */
static uint8_t s_fm_selected = 5;
static bool    s_fm_editing  = false;
static uint8_t s_fm2_cursor  = FM2_CUR_OP;
static bool    s_fm2_editing = false;

enum { FM_LINK_MSG_NONE, FM_LINK_MSG_LOOP, FM_LINK_MSG_NO_BUS };
static uint8_t s_fm_link_src    = FM_OP_NONE;
static uint8_t s_fm_link_cursor = 0;      /* box index 0..5 */
static uint8_t s_fm_link_count  = 0;      /* successful clicks since linking started */
static uint8_t s_fm_link_msg    = FM_LINK_MSG_NONE;
static uint8_t s_fm_link_bad    = 0;      /* bit t: a click on box t would be refused */

static int fm_ratio_nearest_index(float ratio)
{
    int best = 0;
    float best_d = 1e9f;
    for (int i = 0; i < FM_RATIO_STEP_COUNT; i++) {
        float d = ratio - s_fm_ratio_steps[i];
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static bool fm_op_is_fixed(uint8_t op)
{
    return (s_fm_voice.op_fixed & (1u << op)) != 0u;
}

bool synth_ui_fm_is_active(void)
{
    return seq_state.ui_mode == UI_MODE_FM && !seq_state.menu_open;
}

uint8_t synth_ui_fm_page(void)
{
    return s_fm_page;
}

/* TO row: "TO  OUT", "TO  OP1" for one target; for several, the target
 * labels ascending, '+'-joined up to three ("TO 1+2+3") and packed above
 * that ("TO 1234"). */
static void fm_format_to_row(char *out, size_t n, uint8_t mask)
{
    uint8_t count = 0, only = 0;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        if (mask & (1u << i)) { count++; only = i; }
    }
    if (count == 0u) { snprintf(out, n, "TO  OUT"); return; }
    if (count == 1u) { snprintf(out, n, "TO  OP%u", (unsigned)(FM_NUM_OPS - only)); return; }
    char buf[FM_ROW_LEN] = "TO ";
    size_t len = 3;
    for (int i = FM_NUM_OPS - 1; i >= 0; i--) {     /* label 6-i ascending */
        if (!(mask & (1u << i))) continue;
        if (count <= 3u && len > 3u) buf[len++] = '+';
        buf[len++] = (char)('0' + (FM_NUM_OPS - i));
    }
    buf[len] = '\0';
    snprintf(out, n, "%s", buf);
}

/* The coarse frequency readout, shared by page 0's RAT row and page 1. */
static void fm_format_coarse(char *out, size_t n, uint8_t op)
{
    if (fm_op_is_fixed(op)) snprintf(out, n, "FIX %u", (unsigned)(s_fm_voice.op_fixed_hz[op] + 0.5f));
    else                    snprintf(out, n, "RAT %.2f", (double)s_fm_voice.op_ratio[op]);
}

/* Fine readout in Hz: the fixed frequency, or in ratio mode what the operator
 * sounds for A4. One decimal below 1 kHz. */
static void fm_format_fine(char *out, size_t n, uint8_t op)
{
    bool fixed = fm_op_is_fixed(op);
    float hz = fixed ? s_fm_voice.op_fixed_hz[op] : FM_A4_HZ * s_fm_voice.op_ratio[op];
    const char *pre = fixed ? "HZ " : "A4:";
    const char *suf = fixed ? "" : "Hz";
    if (hz < 1000.0f) snprintf(out, n, "%s%.1f%s", pre, (double)hz, suf);
    else              snprintf(out, n, "%s%.0f%s", pre, (double)hz, suf);
}

/* Inverse of fm_voice_level_to_amp() for the plot: L = 99 + 8 * log2(amp). */
static uint8_t fm_plot_amp_to_level(float amp)
{
    if (amp <= 0.0f) return 0u;
    float l = 99.0f + 8.0f * log2f(amp);
    return (uint8_t)SEQ_CLAMP_INT((int)floorf(l + 0.5f), 0, FM_EG_LEVEL_MAX);
}

/* The page 1 envelope trace. Segment widths are log-compressed,
 * log(1 + T / 10 ms), sharing the plot width left after the sustain stub;
 * a nonzero segment gets at least one column. Rising segments follow the DX7
 * attack law (evaluated in amplitude), falling ones are straight in level. */
static void fm_build_plot(fm_view_t *out, const fm_op_env_t *env)
{
    const int span = FM_PLOT_W - 1 - FM_PLOT_STUB_W;
    float w[4], total = 0.0f;
    for (uint8_t s = 0; s < 4u; s++) {
        w[s] = logf(1.0f + (float)env->time_ms[s] / 10.0f);
        total += w[s];
    }
    int px[4], used = 0, big = 0;
    for (uint8_t s = 0; s < 4u; s++) {
        px[s] = 0;
        if (env->time_ms[s] != 0u && total > 0.0f) {
            px[s] = (int)((float)span * w[s] / total + 0.5f);
            if (px[s] < 1) px[s] = 1;
        }
        used += px[s];
        if (px[s] > px[big]) big = (int)s;
    }
    px[big] += span - used;   /* rounding drift goes to the widest segment */

    uint8_t *bp = out->plot_bp_x;
    bp[0] = 0u;
    bp[1] = (uint8_t)px[0];
    bp[2] = (uint8_t)(bp[1] + px[1]);
    bp[3] = (uint8_t)(bp[2] + px[2]);
    bp[4] = (uint8_t)(bp[3] + FM_PLOT_STUB_W);
    bp[5] = (uint8_t)(FM_PLOT_W - 1);

    const uint8_t *L = env->level;
    const uint8_t from[5] = { L[3], L[0], L[1], L[2], L[2] };
    const uint8_t to[5]   = { L[0], L[1], L[2], L[2], L[3] };
    /* In order, so a shared column ends up with the later segment's start,
     * which is the earlier one's end. */
    for (uint8_t g = 0; g < 5u; g++) {
        uint8_t xa = bp[g], xb = bp[g + 1u];
        float a0 = fm_voice_level_to_amp(from[g]), a1 = fm_voice_level_to_amp(to[g]);
        for (uint8_t x = xa; x <= xb; x++) {
            float t = (xb > xa) ? (float)(x - xa) / (float)(xb - xa) : 1.0f;
            uint8_t lv;
            if (to[g] > from[g]) {
                lv = fm_plot_amp_to_level(eg_shape_eval(ENVELOPE_DX7, a0, a1, t));
            } else {
                float l = (float)from[g] + ((float)to[g] - (float)from[g]) * t;
                lv = (uint8_t)SEQ_CLAMP_INT((int)floorf(l + 0.5f), 0, FM_EG_LEVEL_MAX);
            }
            out->plot_level[x] = lv;
        }
    }
    memcpy(out->eg_level, env->level, sizeof(out->eg_level));
}

static void fm_build_page_freq_eg(fm_view_t *out)
{
    const fm_voice_t *v = &s_fm_voice;
    uint8_t op = s_fm_selected;
    const fm_op_env_t *env = &v->op_env[op];

    snprintf(out->title, sizeof(out->title), "OP%u FREQ/EG", (unsigned)(FM_NUM_OPS - op));
    snprintf(out->cells[FM2_CUR_OP], FM_CELL_LEN, "OP  %u", (unsigned)(FM_NUM_OPS - op));
    snprintf(out->cells[FM2_CUR_FRQ], FM_CELL_LEN, "FRQ %s", fm_op_is_fixed(op) ? "FIX" : "RAT");
    fm_format_coarse(out->cells[FM2_CUR_COARSE], FM_CELL_LEN, op);
    fm_format_fine(out->cells[FM2_CUR_FINE], FM_CELL_LEN, op);
    /* Compact column: "T1 300" then "L99" on the same line. */
    for (uint8_t s = 0; s < 4u; s++) {
        snprintf(out->cells[FM2_CUR_T1 + 2u * s], FM_CELL_LEN, "T%u %u",
                 (unsigned)(s + 1u), (unsigned)env->time_ms[s]);
        snprintf(out->cells[FM2_CUR_L1 + 2u * s], FM_CELL_LEN, "L%2u",
                 (unsigned)env->level[s]);
    }
    fm_build_plot(out, env);
}

void fm_build_view(fm_view_t *out)
{
    const fm_voice_t *v = &s_fm_voice;
    /* Zeroed first: the signature hashes the whole struct. */
    memset(out, 0, sizeof(*out));
    fm_voice_graph(v, &out->graph);
    out->page        = s_fm_page;
    out->selected_op = s_fm_selected;
    out->muted       = v->op_mute;
    out->cursor      = s_fm_page ? s_fm2_cursor  : s_fm_cursor;
    out->editing     = s_fm_page ? s_fm2_editing : s_fm_editing;
    out->fb_applies  = (out->graph.fb_op == s_fm_selected);
    if (synth_ui_fm_link_active()) {
        out->linking     = true;
        out->link_src    = s_fm_link_src;
        out->link_cursor = s_fm_link_cursor;
        out->link_bad    = s_fm_link_bad;
    }

    if (s_fm_page) {
        fm_build_page_freq_eg(out);
        return;
    }

    if (out->linking) {
        if (s_fm_link_msg == FM_LINK_MSG_LOOP)        snprintf(out->title, sizeof(out->title), "LINK: LOOP");
        else if (s_fm_link_msg == FM_LINK_MSG_NO_BUS) snprintf(out->title, sizeof(out->title), "LINK: NO BUS");
        else snprintf(out->title, sizeof(out->title), "LINK OP%u", (unsigned)(FM_NUM_OPS - s_fm_link_src));
    } else if (v->algorithm == FM_ALGO_CUSTOM) {
        snprintf(out->title, sizeof(out->title), "FM CUSTOM");
    } else {
        snprintf(out->title, sizeof(out->title), "FM ALG %u", (unsigned)v->algorithm);
    }

    uint8_t op = s_fm_selected;
    fm_format_coarse(out->rows[FM_CUR_RATIO - FM_CUR_RATIO], FM_ROW_LEN, op);
    snprintf(out->rows[FM_CUR_LEVEL - FM_CUR_RATIO], FM_ROW_LEN, "LVL %3u%%",
             (unsigned)(v->op_level[op] * 100.0f + 0.5f));
    fm_format_to_row(out->rows[FM_CUR_TO - FM_CUR_RATIO], FM_ROW_LEN, out->graph.out_mask[op]);
    snprintf(out->rows[FM_CUR_FB - FM_CUR_RATIO], FM_ROW_LEN, "FB  %3u%%",
             (unsigned)(v->feedback * 100.0f + 0.5f));
    if (v->algorithm == FM_ALGO_CUSTOM) snprintf(out->rows[FM_CUR_ALGO - FM_CUR_RATIO], FM_ROW_LEN, "ALG CUST");
    else snprintf(out->rows[FM_CUR_ALGO - FM_CUR_RATIO], FM_ROW_LEN, "ALG %u", (unsigned)v->algorithm);
}

uint32_t fm_view_signature(fm_view_t *out)
{
    fm_build_view(out);
    return fnv1a_bytes(FNV1A_OFFSET, out, sizeof(*out));
}

/* Walk the TO row's candidates (OUT, then the other operators) from the
 * current (lowest) target in `delta`'s direction until one the compiler
 * accepts. Each candidate is a single target and replaces the whole set. */
static void fm_edit_target(uint8_t op, int delta)
{
    fm_graph_view_t g;
    fm_voice_graph(&s_fm_voice, &g);
    /* Candidate ring: index 0 = OUT, 1..6 = operator 0..5 (self skipped). */
    int cur = 0;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) { if (g.out_mask[op] & (1u << i)) { cur = i + 1; break; } }
    int step = (delta > 0) ? 1 : -1;
    int n = FM_NUM_OPS + 1;
    int c = cur;
    for (int tries = 0; tries < n; tries++) {
        c = ((c + step) % n + n) % n;
        uint8_t target = (c == 0) ? FM_TO_OUT : (uint8_t)(c - 1);
        if (target == op) continue;
        uint8_t mask = (target == FM_TO_OUT) ? FM_OUT_BIT : (uint8_t)(1u << target);
        if (fm_voice_set_op_targets(&s_fm_voice, op, mask, NULL)) {
            sequencer_core_fm_voice_changed(FM_PUSH_ROUTING);
            return;
        }
    }
}

/* Coarse frequency: ratio mode walks the curated list, fixed mode moves in
 * semitones. */
static void fm_edit_coarse(uint8_t op, int delta)
{
    if (fm_op_is_fixed(op)) {
        float hz = s_fm_voice.op_fixed_hz[op] * exp2f((float)delta / 12.0f);
        s_fm_voice.op_fixed_hz[op] = SEQ_CLAMP_F32(hz, FM_FIXED_HZ_MIN, FM_FIXED_HZ_MAX);
    } else {
        float ratio = s_fm_voice.op_ratio[op];
        int cur = fm_ratio_nearest_index(ratio);
        int next = SEQ_CLAMP_INT(cur + delta, 0, FM_RATIO_STEP_COUNT - 1);
        float r = s_fm_ratio_steps[next] * (ratio / s_fm_ratio_steps[cur]);
        s_fm_voice.op_ratio[op] = SEQ_CLAMP_F32(r, FM_RATIO_MIN, FM_RATIO_MAX);
    }
    sequencer_core_fm_voice_changed(op);
}

/* Fine frequency: FM_FINE_HZ per detent, measured at A4 in ratio mode. */
static void fm_edit_fine(uint8_t op, int delta)
{
    if (fm_op_is_fixed(op)) {
        float hz = s_fm_voice.op_fixed_hz[op] + FM_FINE_HZ * (float)delta;
        s_fm_voice.op_fixed_hz[op] = SEQ_CLAMP_F32(hz, FM_FIXED_HZ_MIN, FM_FIXED_HZ_MAX);
    } else {
        float r = s_fm_voice.op_ratio[op] + FM_FINE_HZ * (float)delta / FM_A4_HZ;
        s_fm_voice.op_ratio[op] = SEQ_CLAMP_F32(r, FM_RATIO_MIN, FM_RATIO_MAX);
    }
    sequencer_core_fm_voice_changed(op);
}

/* RAT <-> FIX, seeding the new mode from what the operator sounds at A4.
 * Leaving ratio mode needs the osc reset (FM_PUSH_OP_RESET, fm_voice.h). */
static void fm_toggle_fixed(uint8_t op)
{
    uint8_t bit = (uint8_t)(1u << op);
    if (fm_op_is_fixed(op)) {
        float r = s_fm_voice.op_fixed_hz[op] / FM_A4_HZ;
        s_fm_voice.op_ratio[op] = SEQ_CLAMP_F32(r, FM_RATIO_MIN, FM_RATIO_MAX);
        s_fm_voice.op_fixed = (uint8_t)(s_fm_voice.op_fixed & ~bit);
        sequencer_core_fm_voice_changed(op);
    } else {
        float hz = FM_A4_HZ * s_fm_voice.op_ratio[op];
        s_fm_voice.op_fixed_hz[op] = SEQ_CLAMP_F32(hz, FM_FIXED_HZ_MIN, FM_FIXED_HZ_MAX);
        s_fm_voice.op_fixed = (uint8_t)(s_fm_voice.op_fixed | bit);
        sequencer_core_fm_voice_changed(FM_PUSH_OP_RESET(op));
    }
}

/* Segment time: max(1, round(t * 0.08)) ms per detent, recomputed each
 * detent, clamped to what fm_voice pushes so the display stays honest. */
static uint16_t fm_step_time(uint16_t t, int delta, uint32_t lo, uint32_t hi)
{
    int32_t v = (int32_t)t;
    int32_t dir = (delta > 0) ? 1 : -1;
    int n = (delta > 0) ? delta : -delta;
    for (int i = 0; i < n; i++) {
        int32_t s = (int32_t)((float)v * 0.08f + 0.5f);
        if (s < 1) s = 1;
        v += dir * s;
        v = SEQ_CLAMP_INT(v, (int32_t)lo, (int32_t)hi);
    }
    return (uint16_t)v;
}

static void fm_edit_page_freq_eg(int delta)
{
    uint8_t op = s_fm_selected;
    fm_op_env_t *env = &s_fm_voice.op_env[op];
    switch (s_fm2_cursor) {
        case FM2_CUR_OP:
            /* Label order OP1..OP6 is index 5..0. */
            s_fm_selected = (uint8_t)SEQ_CLAMP_INT((int)s_fm_selected - delta, 0, FM_NUM_OPS - 1);
            break;
        case FM2_CUR_COARSE:
            fm_edit_coarse(op, delta);
            break;
        case FM2_CUR_FINE:
            fm_edit_fine(op, delta);
            break;
        case FM2_CUR_T1: case FM2_CUR_T2: case FM2_CUR_T3: case FM2_CUR_T4: {
            uint8_t s = (uint8_t)((s_fm2_cursor - FM2_CUR_T1) / 2u);
            uint32_t lo = (s == 0u) ? VOICE_ENV_ATTACK_MIN_MS
                        : (s == 3u) ? VOICE_ENV_RELEASE_MIN_MS : 0u;
            env->time_ms[s] = fm_step_time(env->time_ms[s], delta, lo, VOICE_ENV_TIME_MAX_MS);
            sequencer_core_fm_voice_changed(op);
            break;
        }
        case FM2_CUR_L1: case FM2_CUR_L2: case FM2_CUR_L3: case FM2_CUR_L4: {
            uint8_t s = (uint8_t)((s_fm2_cursor - FM2_CUR_L1) / 2u);
            env->level[s] = (uint8_t)SEQ_CLAMP_INT((int)env->level[s] + delta, 0, FM_EG_LEVEL_MAX);
            sequencer_core_fm_voice_changed(op);
            break;
        }
        default:
            break;
    }
}

bool synth_ui_fm_handle_encoder(int delta)
{
    if (!synth_ui_fm_is_active()) return false;
    if (delta == 0) return true;

    if (synth_ui_fm_link_active()) {
        int c = (int)s_fm_link_cursor + delta;
        s_fm_link_cursor = (uint8_t)SEQ_CLAMP_INT(c, 0, FM_NUM_OPS - 1);
        s_fm_link_msg    = FM_LINK_MSG_NONE;
        s_force_redraw   = true;
        return true;
    }

    if (s_fm_page) {
        if (s_fm2_editing) {
            fm_edit_page_freq_eg(delta);
        } else {
            int c = (int)s_fm2_cursor + delta;
            s_fm2_cursor = (uint8_t)SEQ_CLAMP_INT(c, 0, (int)FM2_CUR_COUNT - 1);
        }
    } else if (s_fm_editing) {
        uint8_t op = s_fm_selected;
        switch (s_fm_cursor) {
            case FM_CUR_RATIO:
                fm_edit_coarse(op, delta);
                break;
            case FM_CUR_LEVEL: {
                float lvl = s_fm_voice.op_level[op] + (float)delta * 0.05f;
                s_fm_voice.op_level[op] = SEQ_CLAMP_F32(lvl, 0.0f, 1.0f);
                sequencer_core_fm_voice_changed(op);
                break;
            }
            case FM_CUR_TO:
                fm_edit_target(op, delta);
                break;
            case FM_CUR_FB: {
                float fb = s_fm_voice.feedback + (float)delta * 0.05f;
                s_fm_voice.feedback = SEQ_CLAMP_F32(fb, 0.0f, 1.2f);
                sequencer_core_fm_voice_changed(FM_PUSH_ROUTING);
                break;
            }
            case FM_CUR_ALGO:
                fm_voice_step_algorithm(&s_fm_voice, delta);
                sequencer_core_fm_voice_changed(FM_PUSH_ROUTING);
                break;
            default:
                break;
        }
    } else {
        int c = (int)s_fm_cursor + delta;
        c = SEQ_CLAMP_INT(c, 0, (int)FM_CUR_COUNT - 1);
        s_fm_cursor = (uint8_t)c;
        if (s_fm_cursor < FM_CUR_RATIO) s_fm_selected = (uint8_t)(s_fm_cursor - FM_CUR_OP_BASE);
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_fm_handle_button(void)
{
    if (!synth_ui_fm_is_active()) return false;
    if (synth_ui_fm_link_active()) {
        synth_ui_fm_link_end();
        return true;
    }
    if (s_fm_page) {
        /* FRQ has no adjust phase: the click is the toggle. */
        if (s_fm2_cursor == FM2_CUR_FRQ) fm_toggle_fixed(s_fm_selected);
        else                             s_fm2_editing = !s_fm2_editing;
    } else if (s_fm_cursor < FM_CUR_RATIO) {
        s_fm_cursor  = FM_CUR_RATIO;
        s_fm_editing = false;
    } else if (s_fm_cursor == FM_CUR_FB && !s_fm_editing) {
        /* On an operator without the loop the click moves the loop there;
         * on the loop's operator it enters adjust of the amount. */
        fm_graph_view_t g;
        fm_voice_graph(&s_fm_voice, &g);
        if (g.fb_op != s_fm_selected) {
            if (fm_voice_set_fb_op(&s_fm_voice, s_fm_selected)) {
                sequencer_core_fm_voice_changed(FM_PUSH_ROUTING);
            }
        } else {
            s_fm_editing = true;
        }
    } else {
        s_fm_editing = !s_fm_editing;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_fm_toggle_mute(void)
{
    if (!synth_ui_fm_is_active()) return false;
    s_fm_voice.op_mute = (uint8_t)(s_fm_voice.op_mute ^ (1u << s_fm_selected));
    sequencer_core_fm_voice_changed(s_fm_selected);
    s_force_redraw = true;
    return true;
}

bool synth_ui_fm_toggle_page(void)
{
    if (!synth_ui_fm_is_active()) return false;
    synth_ui_fm_link_end();
    s_fm_page = s_fm_page ? 0u : 1u;
    /* Page 1 may have changed the selection: a page 0 cursor parked on an
     * operator box follows it. */
    if (s_fm_page == 0u && s_fm_cursor < FM_CUR_RATIO) {
        s_fm_cursor = (uint8_t)(FM_CUR_OP_BASE + s_fm_selected);
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_fm_step_algorithm(int delta)
{
    if (!synth_ui_fm_is_active()) return false;
    if (delta == 0 || synth_ui_fm_link_active()) return true;
    fm_voice_step_algorithm(&s_fm_voice, delta);
    sequencer_core_fm_voice_changed(FM_PUSH_ROUTING);
    s_force_redraw = true;
    return true;
}

/* ── Link mode ─────────────────────────────────────────────────────────── */

bool synth_ui_fm_link_active(void)
{
    return s_fm_link_src != FM_OP_NONE && synth_ui_fm_is_active() && s_fm_page == 0u;
}

void synth_ui_fm_link_end(void)
{
    s_fm_link_src  = FM_OP_NONE;
    s_fm_link_msg  = FM_LINK_MSG_NONE;
    s_fm_link_bad  = 0;
    s_force_redraw = true;
}

/* The click rule for box t: false when the click is a no-op, else the
 * source's new target mask. The first click of a session replaces the set;
 * later ones toggle t in or out, never emptying it. */
static bool fm_link_click_mask(uint8_t t, uint8_t *mask)
{
    if (t == s_fm_link_src) return false;
    fm_graph_view_t g;
    fm_voice_graph(&s_fm_voice, &g);
    uint8_t cur = g.out_mask[s_fm_link_src];
    uint8_t bit = (uint8_t)(1u << t);
    if (s_fm_link_count == 0u) {
        *mask = bit;
    } else if (cur & bit) {
        if (cur == bit) return false;
        *mask = (uint8_t)(cur & ~bit);
    } else {
        *mask = (uint8_t)(cur | bit);
    }
    return true;
}

/* Try every box's click on a copy of the voice. */
static void fm_link_refresh_bad(void)
{
    s_fm_link_bad = 0;
    for (uint8_t t = 0; t < FM_NUM_OPS; t++) {
        uint8_t mask;
        if (!fm_link_click_mask(t, &mask)) continue;
        fm_voice_t trial = s_fm_voice;
        if (!fm_voice_set_op_targets(&trial, s_fm_link_src, mask, NULL)) {
            s_fm_link_bad = (uint8_t)(s_fm_link_bad | (1u << t));
        }
    }
}

bool synth_ui_fm_link_button(void)
{
    if (!synth_ui_fm_link_active()) s_fm_link_src = FM_OP_NONE;   /* drop stale state */
    if (!synth_ui_fm_is_active() || s_fm_page != 0u) return false;

    if (s_fm_link_src == FM_OP_NONE) {
        s_fm_link_src    = s_fm_selected;
        s_fm_link_cursor = s_fm_selected;
        s_fm_link_count  = 0;
        s_fm_link_msg    = FM_LINK_MSG_NONE;
        s_fm_editing     = false;
        fm_link_refresh_bad();
    } else {
        uint8_t mask;
        s_fm_link_msg = FM_LINK_MSG_NONE;
        if (fm_link_click_mask(s_fm_link_cursor, &mask)) {
            fm_route_err_t err;
            if (fm_voice_set_op_targets(&s_fm_voice, s_fm_link_src, mask, &err)) {
                if (s_fm_link_count < UINT8_MAX) s_fm_link_count++;
                sequencer_core_fm_voice_changed(FM_PUSH_ROUTING);
                fm_link_refresh_bad();
            } else if (err == FM_ROUTE_LOOP) {
                s_fm_link_msg = FM_LINK_MSG_LOOP;
            } else if (err == FM_ROUTE_NO_BUS) {
                s_fm_link_msg = FM_LINK_MSG_NO_BUS;
            }
        }
    }
    s_force_redraw = true;
    return true;
}
