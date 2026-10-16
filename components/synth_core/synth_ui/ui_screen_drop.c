#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "drop_core.h"
#include "sequencer_core.h"
#include "sequencer.h"     /* sequencer_ticks (AMY) */
#include "patch_names.h"
#include "seq_clamp.h"
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  BALL DROP SCREEN
 * ════════════════════════════════════════════════════════════════════════
 * Gestures (CONTROLS.md): the encoder moves the cursor over the header
 * fields and then the cells; the encoder click toggles editing (a field's
 * value, or the angle of the segment under the cursor). Button 0 tap
 * places or removes a plain segment, held + turn rotates it; SHIFT + turn
 * cycles its type, SHIFT + Button 0 held + turn sets a splitter's ratio.
 * Button 1 tap toggles the focus (DROP / SUB), held + turn cycles the
 * focused patch (the dispatcher's patch hold). Button 2 tap places or
 * removes a jackpot cell at the cursor. SHIFT + Button 0 rerolls the seed,
 * held it freezes or unfreezes it (both on the release, and neither when a
 * turn set a ratio during the hold); SHIFT + Button 1 opens the editors on
 * the focused synth and SHIFT + Button 3 the Layer page's entry for it, as
 * on the arp screen. */

_Static_assert(DROP_VIEW_COLS == DROP_COLS && DROP_VIEW_ROWS == DROP_ROWS &&
               DROP_VIEW_SEGS == DROP_SEG_MAX && DROP_VIEW_BALLS == DROP_BALL_MAX,
               "display_drop.h mirrors drop_core.h");

bool synth_ui_drop_is_active(void)
{
    return synth_ui_active_view() == UI_VIEW_DROP;
}

static uint8_t s_cursor  = DROP_CUR_ENABLE;     /* the screen opens on its on/off field */
static bool    s_editing = false;
/* Hold state of buttons 0..2 and whether a turn landed during the hold; a
 * held button's release is a tap only when nothing turned. */
static bool    s_held[3];
static bool    s_turned[3];
static bool    s_long[3];

static inline bool    on_cell(void) { return s_cursor >= DROP_CUR_CELL0; }
static inline uint8_t cur_col(void) { return (uint8_t)((s_cursor - DROP_CUR_CELL0) % DROP_COLS); }
static inline uint8_t cur_row(void) { return (uint8_t)((s_cursor - DROP_CUR_CELL0) / DROP_COLS); }

void drop_build_view(drop_view_t *out)
{
    memset(out, 0, sizeof(*out));
    drop_which_t focus = drop_get_focus();
    out->enabled   = drop_get_enabled();
    out->muted     = drop_get_muted(focus);
    out->focus     = (uint8_t)focus;
    out->patch     = drop_get_patch(focus);
    out->patch_select = seq_state.patch_select_mode;
    out->patch_name   = patch_name_for(out->patch);
    ui_note_name(focus == DROP_VOICE_SUB ? drop_get_sub_root_note() : drop_get_root_note(),
                 out->root_name);
    out->rate_str  = note_div_label(drop_get_rate());
    out->loop_bars = drop_get_loop_bars();
    out->seed16    = (uint16_t)(drop_get_seed() & 0xFFFFu);
    out->seed_frozen = drop_get_seed_frozen();
    out->seg_count = drop_get_seg_count();
    for (uint8_t i = 0; i < out->seg_count && i < DROP_VIEW_SEGS; i++) {
        drop_seg_t s;
        if (!drop_get_seg(i, &s)) break;
        out->segs[i].col = s.col;  out->segs[i].row = s.row;
        out->segs[i].angle = s.angle; out->segs[i].type = s.type;
        out->segs[i].ratio = s.ratio;
    }
    drop_jackpot_get_map(out->jackpot);
    out->spawn_col = DROP_SPAWN_COL;
    /* Balls: the deterministic sim replayed to the playhead's loop tick; the
     * incremental replay only memoises, so the view stays a pure function
     * of the loop state and the tick. Stopped: an empty field. */
    if (sequencer_core_get_playing() && out->enabled) {
        drop_ball_pos_t balls[DROP_BALL_MAX];
        uint32_t loop = drop_loop_ticks();
        uint8_t n = drop_view_balls(sequencer_ticks() % loop, balls);
        out->ball_count = n;
        for (uint8_t i = 0; i < n; i++) {
            out->balls[i].x = balls[i].x;
            out->balls[i].y = balls[i].y;
        }
    }
    out->cursor  = s_cursor;
    out->editing = s_editing;
    out->banner = NULL;
    if (seq_state.algo_banner_ticks > 0) {
        switch (seq_state.algo_banner_value) {
            case DISPLAY_SEQ_BANNER_SOLO_ON:  out->banner = "SOLO";     break;
            case DISPLAY_SEQ_BANNER_SOLO_OFF: out->banner = "SOLO OFF"; break;
            case DISPLAY_SEQ_BANNER_SOLO_CLR: out->banner = "SOLO CLR"; break;
            default: break;
        }
    }
}

uint32_t drop_view_signature(drop_view_t *out)
{
    drop_build_view(out);
    /* The whole flat view, pointers included (static strings). */
    return fnv1a_bytes(FNV1A_OFFSET, out, sizeof(*out));
}

/* ── Edits ─────────────────────────────────────────────────────────── */

static void drop_rotate_seg(int dir)
{
    if (!on_cell()) return;
    int idx = drop_find_seg(cur_col(), cur_row());
    drop_seg_t s;
    if (idx < 0 || !drop_get_seg((uint8_t)idx, &s)) return;
    s.angle = (uint8_t)(((int)s.angle + dir + DROP_SEG_ANGLES) % DROP_SEG_ANGLES);
    drop_set_seg(&s);
}

static void drop_cycle_seg_type(int dir)
{
    if (!on_cell()) return;
    int idx = drop_find_seg(cur_col(), cur_row());
    drop_seg_t s;
    if (idx < 0 || !drop_get_seg((uint8_t)idx, &s)) return;
    s.type = (uint8_t)(((int)s.type + dir + DROP_SEG_TYPE_COUNT) % DROP_SEG_TYPE_COUNT);
    if (s.type == DROP_SEG_SPLIT && s.ratio == 0) s.ratio = 50;
    drop_set_seg(&s);
}

static void drop_adjust_ratio(int dir)
{
    if (!on_cell()) return;
    int idx = drop_find_seg(cur_col(), cur_row());
    drop_seg_t s;
    if (idx < 0 || !drop_get_seg((uint8_t)idx, &s)) return;
    if (s.type != DROP_SEG_SPLIT) return;
    s.ratio = (uint8_t)SEQ_CLAMP_INT((int)s.ratio + dir * 5, 0, 100);
    drop_set_seg(&s);
}

static void drop_toggle_seg(void)
{
    if (!on_cell()) return;
    if (drop_remove_seg(cur_col(), cur_row())) return;
    drop_seg_t s = { .col = cur_col(), .row = cur_row(), .angle = 0,
                     .type = DROP_SEG_PLAIN, .ratio = 50 };
    (void)drop_set_seg(&s);
}

/* Button 2 tap: a jackpot cell on the cursor, or none. */
static void drop_toggle_jackpot(void)
{
    if (!on_cell()) return;
    drop_jackpot_set(cur_col(), cur_row(), !drop_jackpot_at(cur_col(), cur_row()));
}

static void drop_edit_field(uint8_t cursor, int dir)
{
    if (dir == 0) return;
    switch (cursor) {
        case DROP_CUR_ENABLE:
            drop_set_enabled(!drop_get_enabled());
            break;
        case DROP_CUR_FOCUS:
            drop_set_focus(drop_get_focus() == DROP_VOICE_SUB ? DROP_VOICE_MAIN : DROP_VOICE_SUB);
            break;
        case DROP_CUR_ROOT:
            if (drop_get_focus() == DROP_VOICE_SUB)
                drop_set_sub_root_note((uint8_t)SEQ_CLAMP_INT((int)drop_get_sub_root_note() + dir, 0, 127));
            else
                drop_set_root_note((uint8_t)SEQ_CLAMP_INT((int)drop_get_root_note() + dir, 0, 127));
            break;
        case DROP_CUR_BARS:
            drop_set_loop_bars((uint8_t)SEQ_CLAMP_INT((int)drop_get_loop_bars() + dir, 1, DROP_LOOP_BARS_MAX));
            break;
        case DROP_CUR_SEED:
            drop_reroll_seed();
            break;
        case DROP_CUR_RATE: {
            int r = SEQ_CLAMP_INT((int)drop_get_rate() + dir, 0, NOTE_DIV_STD_COUNT - 1);
            drop_set_rate((note_div_t)r);
            break;
        }
        default:
            drop_rotate_seg(dir);
            break;
    }
}

/* ── Input ─────────────────────────────────────────────────────────── */

void synth_ui_drop_handle_encoder(long delta, bool shift)
{
    if (delta == 0) return;
    int dir = (delta > 0) ? 1 : -1;
    if (s_held[0]) {
        s_turned[0] = true;
        if (shift) drop_adjust_ratio(dir);
        else       drop_rotate_seg(dir);
    } else if (s_held[2]) {
        s_turned[2] = true;   /* a held Button 2 only parks the cursor */
    } else if (shift) {
        drop_cycle_seg_type(dir);
    } else if (s_editing) {
        drop_edit_field(s_cursor, dir);
    } else {
        int c = (int)s_cursor + (int)delta;
        s_cursor = (uint8_t)SEQ_CLAMP_INT(c, 0, DROP_CUR_COUNT - 1);
    }
    s_force_redraw = true;
}

void synth_ui_drop_handle_button(void)
{
    s_editing = !s_editing;
    s_force_redraw = true;
}

/* The patch hold+turn (ui_patch_cycle.c) landed: Button 1's release is no
 * longer a focus tap. */
void synth_ui_drop_patch_turned(void)
{
    s_turned[1] = true;
    s_force_redraw = true;
}

void synth_ui_drop_button_edge(uint8_t btn, bool down)
{
    if (btn > 2) return;
    if (down) {
        s_held[btn] = true;
        s_turned[btn] = false;
        s_long[btn] = false;
        return;
    }
    bool tap = s_held[btn] && !s_turned[btn] && !s_long[btn];
    s_held[btn] = false;
    if (!tap) return;
    switch (btn) {
        case 0: drop_toggle_seg(); break;
        case 1: drop_set_focus(drop_get_focus() == DROP_VOICE_SUB ? DROP_VOICE_MAIN
                                                                   : DROP_VOICE_SUB); break;
        default: drop_toggle_jackpot(); break;
    }
    s_force_redraw = true;
}

bool synth_ui_drop_button_long(uint8_t btn)
{
    if (btn > 2) return false;
    s_long[btn] = true;
    /* A hold that already turned something is a modifier, not a long press. */
    return s_turned[btn];
}

void synth_ui_drop_shift0_edge(bool down)
{
    if (down) {
        s_held[0] = true;
        s_turned[0] = false;
        s_long[0] = false;
        return;
    }
    bool spend = s_held[0] && !s_turned[0];
    s_held[0] = false;
    if (!spend) return;
    if (s_long[0]) drop_set_seed_frozen(!drop_get_seed_frozen());
    else           drop_reroll_seed();
    s_force_redraw = true;
}
