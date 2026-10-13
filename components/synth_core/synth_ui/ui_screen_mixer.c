#include "synth_ui/synth_ui_internal.h"
#include "synth_ui/ui_mixer_quick.h"
#include "synth_ui.h"
#include "mixer.h"
#include "sequencer_core.h"
#include "arp_core.h"
#include "custompatches/clip_player.h"
#include "synth_slots.h"
#include "seq_clamp.h"
#include "sdkconfig.h"
#if CONFIG_SYNTH_WIRELESS
#include "live_play.h"
#endif
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  Mixer page
 * ════════════════════════════════════════════════════════════════════════
 * Rows over the mixer model (mixer.h): the overview lists master, the drum
 * layer, each melodic layer, the arp, both drones, and the live voice and
 * clip slots while they exist, then the FADE settings row (the fade
 * length); a layer row dives into its track rows. Overview source rows
 * carry up to two quick cells (ui_mixer_quick.h). The row list is rebuilt from live state on every view build and input call,
 * so rows appear and vanish with their sources. Rendered by display_mixer.c.
 * Controls: CONTROLS.md. */

/* Overview ceiling: MAST, every layer, ARP/DRON/STUT, LIVE, the clips, FADE. */
#define MIX_PAGE_ROWS  (1u + MAX_LAYERS + 3u + 1u + CLIP_SLOT_COUNT + 1u)
#define MIX_LEVEL_STEP 5
#define MIX_NO_DIVE    0xFFu
#define MIX_SRC_NONE   0xFFu   /* src.kind of a settings row: no mixer source */

/* mix_row_t.setting */
enum { MIX_SETTING_NONE = 0, MIX_SETTING_FADE };

_Static_assert(MIX_PAGE_ROWS >= SEQ_TRACKS, "dive rows fit the row list");

typedef struct {
    mix_src_t src;
    char      name[5];
    uint8_t   drum_track;   /* MIXER_NO_DRUM unless a drum-layer track row */
    uint8_t   setting;      /* MIX_SETTING_*: a settings row, src unused */
} mix_row_t;

static uint8_t s_cursor          = 0;            /* into the shown row list */
static uint8_t s_top             = 0;            /* first visible row */
static uint8_t s_dive            = MIX_NO_DIVE;  /* layer whose tracks are shown */
static uint8_t s_overview_cursor = 0;            /* restored on leaving the dive */
/* LIST: the turn scrolls the rows. PARAM: the turn stays inside the cursor
 * row's quick cells, selecting one (s_cell) or, while s_editing, stepping it. */
enum { MIX_MODE_LIST = 0, MIX_MODE_PARAM };
static uint8_t s_mode            = MIX_MODE_LIST;
static uint8_t s_cell            = 0;            /* into the row's available cells */
static bool    s_editing         = false;
static bool    s_shift_held      = false;        /* SHIFT held: the bar inverts */

bool synth_ui_mixer_is_active(void)
{
    return seq_state.ui_mode == UI_MODE_MIXER && !seq_state.menu_open;
}

static void add_row(mix_row_t *rows, uint8_t *n, uint8_t kind, uint8_t a, uint8_t b,
                    const char *name, uint8_t drum_track)
{
    if (*n >= MIX_PAGE_ROWS) return;
    mix_row_t *r = &rows[(*n)++];
    r->src = (mix_src_t){ .kind = kind, .a = a, .b = b };
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->drum_track = drum_track;
    r->setting    = MIX_SETTING_NONE;
}

/* The dive layer, or MIX_NO_DIVE when none is open or it no longer exists. */
static uint8_t dive_layer(void)
{
    return (s_dive < sequencer_core_get_num_layers()) ? s_dive : (uint8_t)MIX_NO_DIVE;
}

static uint8_t build_rows(mix_row_t *rows, uint8_t dive)
{
    uint8_t n = 0;
    char name[5];
    if (dive != MIX_NO_DIVE) {
        bool drum = sequencer_core_get_layer_type(dive) == SEQ_LAYER_DRUM;
        uint8_t tracks = sequencer_core_get_layer_tracks(dive);
        for (uint8_t t = 0; t < tracks && t < SEQ_TRACKS; t++) {
            snprintf(name, sizeof(name), "T%u", (unsigned)(t + 1u));
            add_row(rows, &n, MIX_SRC_TRACK, dive, t, drum ? "" : name,
                    drum ? t : (uint8_t)MIXER_NO_DRUM);
        }
        return n;
    }
    add_row(rows, &n, MIX_SRC_MASTER, 0, 0, "MAST", MIXER_NO_DRUM);
    add_row(rows, &n, MIX_SRC_LAYER, 0, 0, "DRUM", MIXER_NO_DRUM);
    uint8_t layers = sequencer_core_get_num_layers();
    for (uint8_t L = 1; L < layers && L < MAX_LAYERS; L++) {
        /* The Layer page's numbering: index + 1. */
        snprintf(name, sizeof(name), "L%u", (unsigned)(L + 1u));
        add_row(rows, &n, MIX_SRC_LAYER, L, 0, name, MIXER_NO_DRUM);
    }
    add_row(rows, &n, MIX_SRC_ARP, 0, 0, "ARP", MIXER_NO_DRUM);
    add_row(rows, &n, MIX_SRC_DRONE_STD, 0, 0, "DRON", MIXER_NO_DRUM);
    add_row(rows, &n, MIX_SRC_DRONE_STUT, 0, 0, "STUT", MIXER_NO_DRUM);
#if CONFIG_SYNTH_WIRELESS
    if (live_play_is_ready()) add_row(rows, &n, MIX_SRC_LIVE, 0, 0, "LIVE", MIXER_NO_DRUM);
#endif
    for (uint8_t c = 0; c < CLIP_SLOT_COUNT; c++) {
        if (clip_player_slot_state(c) != CLIP_SLOT_LOADED) continue;
        snprintf(name, sizeof(name), "CLP%u", (unsigned)(c + 1u));
        add_row(rows, &n, MIX_SRC_CLIP, c, 0, name, MIXER_NO_DRUM);
    }
    add_row(rows, &n, MIX_SRC_NONE, 0, 0, "FADE", MIXER_NO_DRUM);
    if (n > 0) rows[n - 1u].setting = MIX_SETTING_FADE;
    return n;
}

/* Cursor and scroll clamped onto a row list of n rows. */
typedef struct { uint8_t cursor, top; } mix_nav_t;

static mix_nav_t clamp_nav(uint8_t n)
{
    mix_nav_t v = { s_cursor, s_top };
    if (n == 0) return (mix_nav_t){ MIXER_NO_CURSOR, 0 };
    if (v.cursor >= n) v.cursor = (uint8_t)(n - 1u);
    if (v.cursor < v.top) v.top = v.cursor;
    if (v.cursor >= v.top + MIXER_VIS_ROWS) v.top = (uint8_t)(v.cursor - (MIXER_VIS_ROWS - 1u));
    uint8_t max_top = (n > MIXER_VIS_ROWS) ? (uint8_t)(n - MIXER_VIS_ROWS) : 0u;
    if (v.top > max_top) v.top = max_top;
    return v;
}

/* Quick-cell slots row r carries, in order; returns how many (0..2). */
static uint8_t row_cells(const mix_row_t *r, uint8_t slots[2])
{
    uint8_t c = 0;
    if (r->setting != MIX_SETTING_NONE) return 0;
    for (uint8_t q = 0; q < 2u; q++) {
        if (mixer_quick_has(r->src, q)) slots[c++] = q;
    }
    return c;
}

static void enter_list(void)
{
    s_mode    = MIX_MODE_LIST;
    s_cell    = 0;
    s_editing = false;
}

/* Input-side: rebuild the rows and commit the clamped navigation. A dive
 * whose layer went away returns to the overview; PARAM mode drops to LIST
 * when the cursor row no longer has the selected cell. */
static uint8_t sync_rows(mix_row_t *rows)
{
    if (s_dive != MIX_NO_DIVE && dive_layer() == MIX_NO_DIVE) {
        s_dive   = MIX_NO_DIVE;
        s_cursor = s_overview_cursor;
    }
    uint8_t n = build_rows(rows, dive_layer());
    mix_nav_t v = clamp_nav(n);
    if (n > 0) {
        s_cursor = v.cursor;
        s_top    = v.top;
    }
    uint8_t slots[2];
    if (s_mode == MIX_MODE_PARAM && (n == 0 || s_cell >= row_cells(&rows[s_cursor], slots)))
        enter_list();
    return n;
}

bool mixer_page_in_dive(void)
{
    return dive_layer() != MIX_NO_DIVE;
}

bool mixer_page_mode_param(void)
{
    return s_mode == MIX_MODE_PARAM;
}

void mixer_build_view(mixer_view_t *out)
{
    mix_row_t rows[MIX_PAGE_ROWS];
    /* Zeroed first: the signature hashes the whole struct. */
    memset(out, 0, sizeof(*out));
    uint8_t dive = dive_layer();
    uint8_t n = build_rows(rows, dive);
    mix_nav_t v = clamp_nav(n);
    /* PARAM mode's selected cell, if the cursor row still has it. */
    uint8_t slots[2];
    bool    param = s_mode == MIX_MODE_PARAM && n > 0 &&
                    s_cell < row_cells(&rows[v.cursor], slots);
    uint8_t slot  = param ? slots[s_cell] : 0u;
    uint8_t focus = MIXER_FOCUS_NONE;
    if (n > 0 && s_shift_held) focus = MIXER_FOCUS_LEVEL;
    else if (param)            focus = (uint8_t)(MIXER_FOCUS_Q1 + slot);

    if (param) {
        /* The selected quick cell shows only its value; its label goes here. */
        snprintf(out->title, sizeof(out->title), "MIXER  %s",
                 mixer_quick_label(rows[v.cursor].src, slot));
    } else if (dive == MIX_NO_DIVE) {
        snprintf(out->title, sizeof(out->title), "MIXER");
    } else if (sequencer_core_get_layer_type(dive) == SEQ_LAYER_DRUM) {
        snprintf(out->title, sizeof(out->title), "MIXER DRUM");
    } else {
        snprintf(out->title, sizeof(out->title), "MIXER L%u", (unsigned)(dive + 1u));
    }

    uint8_t shown = (uint8_t)(n - v.top);
    if (shown > MIXER_VIS_ROWS) shown = MIXER_VIS_ROWS;
    out->row_count  = shown;
    out->cursor     = (v.cursor == MIXER_NO_CURSOR) ? (uint8_t)MIXER_NO_CURSOR
                                                    : (uint8_t)(v.cursor - v.top);
    out->focus      = focus;
    out->editing    = (focus == MIXER_FOCUS_Q1 || focus == MIXER_FOCUS_Q2) && s_editing;
    out->more_above = v.top > 0u;
    out->more_below = (uint8_t)(v.top + shown) < n;

    for (uint8_t i = 0; i < shown; i++) {
        const mix_row_t *r = &rows[v.top + i];
        mixer_view_row_t *o = &out->rows[i];
        memcpy(o->name, r->name, sizeof(o->name));
        o->drum_track = r->drum_track;
        o->fade_to    = MIXER_NO_FADE;
        if (r->setting == MIX_SETTING_FADE) {
            snprintf(o->text, sizeof(o->text), "%u bar",
                     (unsigned)mixer_fade_bars(mixer_get_fade_bars_idx()));
            continue;   /* no level, no mute */
        }
        o->pos        = mixer_get_pos(r->src);
        uint8_t to;
        if (mixer_fade_active(r->src, &to)) o->fade_to = to;
        o->has_level  = 1;
        o->has_mute   = mixer_has_mute(r->src);
        o->mute       = o->has_mute && mixer_get_mute(r->src);
        if (r->src.kind == MIX_SRC_TRACK) o->hard_mute = sequencer_core_get_track_mute(r->src.a, r->src.b);
        else if (r->src.kind == MIX_SRC_ARP) o->hard_mute = arp_get_muted();
        mixer_quick_text(r->src, 0, o->q1);
        mixer_quick_text(r->src, 1, o->q2);
    }
}

uint32_t mixer_view_signature(mixer_view_t *out)
{
    mixer_build_view(out);
    return fnv1a_bytes(FNV1A_OFFSET, out, sizeof(*out));
}

bool synth_ui_mixer_handle_encoder(int steps, bool shift)
{
    if (!synth_ui_mixer_is_active()) return false;
    s_shift_held = shift;
    if (steps == 0) return true;
    mix_row_t rows[MIX_PAGE_ROWS];
    uint8_t n = sync_rows(rows);
    if (n == 0) return true;
    const mix_row_t *r = &rows[s_cursor];
    uint8_t slots[2];
    uint8_t cells = row_cells(r, slots);
    if (shift && r->setting == MIX_SETTING_FADE) {
        int idx = (int)mixer_get_fade_bars_idx() + steps;
        mixer_set_fade_bars_idx((uint8_t)SEQ_CLAMP_INT(idx, 0, MIXER_FADE_BARS_COUNT - 1));
    } else if (shift) {
        int pos = (int)mixer_get_pos(r->src) + MIX_LEVEL_STEP * steps;
        mixer_set_pos(r->src, (uint8_t)SEQ_CLAMP_INT(pos, 0, (int)MIXER_POS_MAX));
    } else if (s_mode == MIX_MODE_PARAM && s_editing) {
        mixer_quick_step(r->src, slots[s_cell], steps);
    } else if (s_mode == MIX_MODE_PARAM) {
        /* sync_rows() left PARAM mode only on a row with s_cell, so cells > 0. */
        s_cell = (uint8_t)SEQ_CLAMP_INT((int)s_cell + steps, 0, (int)cells - 1);
    } else {
        s_cursor = (uint8_t)SEQ_CLAMP_INT((int)s_cursor + steps, 0, (int)n - 1);
        s_cell   = 0;
        mix_nav_t v = clamp_nav(n);
        s_top = v.top;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_mixer_handle_button(void)
{
    if (!synth_ui_mixer_is_active()) return false;
    mix_row_t rows[MIX_PAGE_ROWS];
    uint8_t n = sync_rows(rows);
    if (n == 0) return true;
    if (s_mode == MIX_MODE_PARAM) {
        s_editing = !s_editing;
        s_force_redraw = true;
    } else if (rows[s_cursor].src.kind == MIX_SRC_LAYER) {
        s_overview_cursor = s_cursor;
        s_dive   = rows[s_cursor].src.a;
        s_cursor = 0;
        s_top    = 0;
        s_force_redraw = true;
    }
    return true;
}

bool synth_ui_mixer_button1(void)
{
    if (!synth_ui_mixer_is_active()) return false;
    mix_row_t rows[MIX_PAGE_ROWS];
    uint8_t n = sync_rows(rows);
    if (n == 0) return true;
    mix_src_t src = rows[s_cursor].src;
    if (rows[s_cursor].setting == MIX_SETTING_NONE && mixer_has_mute(src)) {
        mixer_set_mute(src, !mixer_get_mute(src));
        s_force_redraw = true;
    }
    return true;
}

bool synth_ui_mixer_button2(void)
{
    if (!synth_ui_mixer_is_active()) return false;
    mix_row_t rows[MIX_PAGE_ROWS];
    uint8_t n = sync_rows(rows);
    if (n == 0) return true;
    uint8_t slots[2];
    if (s_mode == MIX_MODE_PARAM) {
        enter_list();
        s_force_redraw = true;
    } else if (row_cells(&rows[s_cursor], slots) > 0u) {
        s_mode    = MIX_MODE_PARAM;
        s_cell    = 0;
        s_editing = false;
        s_force_redraw = true;
    }
    return true;
}

bool synth_ui_mixer_fade(void)
{
    if (!synth_ui_mixer_is_active()) return false;
    mix_row_t rows[MIX_PAGE_ROWS];
    uint8_t n = sync_rows(rows);
    if (n == 0) return true;
    if (rows[s_cursor].setting == MIX_SETTING_NONE) {
        mixer_fade_toggle(rows[s_cursor].src);
        s_force_redraw = true;
    }
    return true;
}

bool synth_ui_mixer_back(void)
{
    if (!synth_ui_mixer_is_active() || dive_layer() == MIX_NO_DIVE) return false;
    s_dive   = MIX_NO_DIVE;
    s_cursor = s_overview_cursor;
    enter_list();
    mix_row_t rows[MIX_PAGE_ROWS];
    (void)sync_rows(rows);   /* clamps the restored cursor and scroll */
    s_force_redraw = true;
    return true;
}

bool synth_ui_mixer_shift(bool held)
{
    s_shift_held = held;
    if (!synth_ui_mixer_is_active()) return false;
    s_force_redraw = true;
    return true;
}

bool synth_ui_mixer_shoulder(void)
{
    if (!synth_ui_mixer_is_active()) return false;
    /* The dive stays, so coming back lands where the user was. */
    synth_ui_menu_toggle();
    synth_ui_menu_go_main();
    return true;
}
