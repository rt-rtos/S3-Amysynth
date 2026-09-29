#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "custompatches/fm_voice.h"
#include "seq_clamp.h"
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════════
 *  FM/ALGO voice editor - operator graph + selected-operator panel
 * ════════════════════════════════════════════════════════════════════════
 * Edits the single live SEQ_PATCH_FM_CUSTOM voice (s_fm_voice, owned by
 * custompatches/fm_voice.c). Rendered by display_fm.c.
 *
 * One flat cursor walks the six operator boxes (selecting as it goes) and
 * then the panel rows RATIO / LEVEL / TO / FB / ALGO for the selected
 * operator. Controls: CONTROLS.md. */

/* Curated DX7-style harmonic ratios plus a few inharmonic ones, kept short
 * enough to encoder through. Editing snaps to the nearest step, then walks. */
static const float s_fm_ratio_steps[] = {
    0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 5.0f,
    6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 14.0f, 16.0f,
};
#define FM_RATIO_STEP_COUNT ((int)(sizeof(s_fm_ratio_steps) / sizeof(s_fm_ratio_steps[0])))

static uint8_t s_fm_cursor   = FM_CUR_OP_BASE + 5;   /* start on OP1 */
static uint8_t s_fm_selected = 5;
static bool    s_fm_editing  = false;

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

static float fm_ratio_step(float current, int delta)
{
    int idx = fm_ratio_nearest_index(current) + delta;
    idx = SEQ_CLAMP_INT(idx, 0, FM_RATIO_STEP_COUNT - 1);
    return s_fm_ratio_steps[idx];
}

bool synth_ui_fm_is_active(void)
{
    return seq_state.ui_mode == UI_MODE_FM && !seq_state.menu_open;
}

uint8_t synth_ui_fm_selected_op(void)
{
    return s_fm_selected;
}

static void fm_format_target(char *out, size_t n, uint8_t target)
{
    if (target < FM_NUM_OPS) snprintf(out, n, "OP%u", (unsigned)(FM_NUM_OPS - target));
    else                     snprintf(out, n, "OUT");
}

void fm_build_view(fm_view_t *out)
{
    const fm_voice_t *v = &s_fm_voice;
    fm_voice_graph(v, &out->graph);
    out->selected_op = s_fm_selected;
    out->cursor      = s_fm_cursor;
    out->editing     = s_fm_editing;
    out->fb_applies  = (out->graph.fb_op == s_fm_selected);

    if (v->algorithm == FM_ALGO_CUSTOM) snprintf(out->title, sizeof(out->title), "FM CUSTOM");
    else snprintf(out->title, sizeof(out->title), "FM ALG %u", (unsigned)v->algorithm);

    uint8_t op = s_fm_selected;
    snprintf(out->rows[FM_CUR_RATIO - FM_CUR_RATIO], FM_ROW_LEN, "RAT %.2f",
             (double)v->op_ratio[op]);
    snprintf(out->rows[FM_CUR_LEVEL - FM_CUR_RATIO], FM_ROW_LEN, "LVL %3u%%",
             (unsigned)(v->op_level[op] * 100.0f + 0.5f));
    /* Target from the live routing (table rows decode to their first target). */
    char tgt[6];
    uint8_t m = out->graph.out_mask[op], t = FM_TO_OUT;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) { if (m & (1u << i)) { t = i; break; } }
    fm_format_target(tgt, sizeof(tgt), t);
    snprintf(out->rows[FM_CUR_TO - FM_CUR_RATIO], FM_ROW_LEN, "TO  %s", tgt);
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
 * current target in `delta`'s direction until one the compiler accepts. */
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
        if (fm_voice_set_op_target(&s_fm_voice, op, target)) {
            sequencer_core_fm_voice_changed(FM_PUSH_ROUTING);
            return;
        }
    }
}

bool synth_ui_fm_handle_encoder(int delta)
{
    if (!synth_ui_fm_is_active()) return false;
    if (delta == 0) return true;

    if (s_fm_editing) {
        uint8_t op = s_fm_selected;
        switch (s_fm_cursor) {
            case FM_CUR_RATIO:
                s_fm_voice.op_ratio[op] = fm_ratio_step(s_fm_voice.op_ratio[op], delta);
                sequencer_core_fm_voice_changed(op);
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
    if (s_fm_cursor < FM_CUR_RATIO) {
        s_fm_cursor  = FM_CUR_RATIO;
        s_fm_editing = false;
    } else {
        s_fm_editing = !s_fm_editing;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_fm_toggle_feedback(void)
{
    if (!synth_ui_fm_is_active()) return false;
    fm_graph_view_t g;
    fm_voice_graph(&s_fm_voice, &g);
    uint8_t want = (g.fb_op == s_fm_selected) ? FM_OP_NONE : s_fm_selected;
    if (fm_voice_set_fb_op(&s_fm_voice, want)) {
        sequencer_core_fm_voice_changed(FM_PUSH_ROUTING);
    }
    s_force_redraw = true;
    return true;
}
