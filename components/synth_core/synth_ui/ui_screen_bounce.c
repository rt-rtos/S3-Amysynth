#include "sdkconfig.h"
#include "synth_ui/synth_ui_internal.h"
#include "sequencer_core.h"
#include "custompatches/clip_bounce.h"
#include "custompatches/clip_player.h"
#include "custompatches/sample_rec.h"
#include "synth_slots.h"
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  BOUNCE PAGE (Main Menu -> "Bounce")
 * ════════════════════════════════════════════════════════════════════════
 * Item model for the loop-bounce recorder; page state and input routing live
 * in ui_screen_menu.c (same split as the FX/NoteFX/Projects/Chords pages).
 *
 * Top block is the shape of the next bounce (target slot, max bars, mono or
 * stereo, tail, what becomes of the sources), a read-only memory line, the
 * Rec row that starts and stops
 * (the same transport as the SHIFT+0 chord, with the page's slot instead of
 * the first empty one), a Cancel row and a one-level Undo. Below it, four
 * rows per clip slot: play/mute, level, tempo mode, clear. Last, the runtime
 * sampler (sample_rec): the same mix capture into one drum pad instead of a
 * clip, driven by a Sample row and its cancel.
 *
 * The shape survives leaving the page, so a second bounce after one
 * recording is one click on Rec. Only the failure text is per-visit.
 *
 * Arm failures have nowhere to go - this UI has no toast overlay - so the
 * reason is shown inline in the Rec row's value until the next arm, exactly
 * how the Projects page reports its action results. */

/* Row indices. The clip block is four rows per slot, so the sampler rows, the
 * Back row and the total all derive from CLIP_SLOT_COUNT. */
enum {
    BOUNCE_ROW_SLOT = 0,
    BOUNCE_ROW_LEN,
    BOUNCE_ROW_FMT,
    BOUNCE_ROW_TAIL,
    BOUNCE_ROW_AFTER,
    BOUNCE_ROW_MEM,
    BOUNCE_ROW_REC,
    BOUNCE_ROW_CANCEL,
    BOUNCE_ROW_UNDO,
    BOUNCE_ROW_CLIP0,
};
#define BOUNCE_CLIP_ROWS  4u    /* play/mute, level, tempo mode, clear */
#define BOUNCE_ROW_SAMPLE     (BOUNCE_ROW_CLIP0 + BOUNCE_CLIP_ROWS * CLIP_SLOT_COUNT)
#define BOUNCE_ROW_SMP_CANCEL (BOUNCE_ROW_SAMPLE + 1u)
#define BOUNCE_ROW_BACK       (BOUNCE_ROW_SMP_CANCEL + 1u)
#define BOUNCE_ROW_COUNT      (BOUNCE_ROW_BACK + 1u)

/* Max bar counts on offer. Powers of two up to CLIP_BOUNCE_MAX_BARS; the
 * real length is set by the stop, rounded to whole pattern periods.
 * clip_bounce_set_shape enforces the same list. */
static const uint8_t BOUNCE_BAR_STEPS[] = { 1, 2, 4, 8, 16 };
#define BOUNCE_BAR_STEP_COUNT \
    (sizeof(BOUNCE_BAR_STEPS) / sizeof(BOUNCE_BAR_STEPS[0]))

/* The bounce shape (bars, format, tail) lives in clip_bounce so the project
 * snapshot can persist it; only the slot cursor is the page's own. */
static uint8_t s_slot = 0;

/* Why the last arm was refused; empty once it has been shown its use. */
static char s_fail[MENU_VALUE_LEN];
/* Same, for the sampler's arm. */
static char s_smp_fail[MENU_VALUE_LEN];

static menu_item_view_t s_items[BOUNCE_ROW_COUNT];

/* Snapshot of everything the page shows that the engine can change under it,
 * so the service redraws on a bar tick or a finished bounce instead of the
 * 50 ms loop redrawing forever. */
static clip_bounce_state_t s_snap_state     = CLIP_BOUNCE_IDLE;
static uint8_t             s_snap_bars_done = 0;
static bool                s_snap_stop_set  = false;
static clip_slot_state_t   s_snap_slot[CLIP_SLOT_COUNT];
static bool                s_snap_playing[CLIP_SLOT_COUNT];
static sample_rec_state_t  s_snap_smp_state = SAMPLE_REC_IDLE;
static uint8_t             s_snap_smp_pct   = 0;

static const char *slot_state_word(uint8_t slot)
{
    switch (clip_player_slot_state(slot)) {
        case CLIP_SLOT_LOADED:   return "used";
        case CLIP_SLOT_CLEARING: return "...";
        case CLIP_SLOT_EMPTY:
        default:                 return "empty";
    }
}

/* Index of a bar count in the step list; 0 for a value that is not on it,
 * which only a future default could produce. */
static uint8_t bars_step_index(uint8_t bars)
{
    for (uint8_t i = 0; i < BOUNCE_BAR_STEP_COUNT; i++) {
        if (BOUNCE_BAR_STEPS[i] == bars) return i;
    }
    return 0;
}

/* Which clip slot a row in the clip block belongs to, and which of its three
 * rows it is. */
static uint8_t clip_row_slot(uint8_t idx)
{
    return (uint8_t)((idx - BOUNCE_ROW_CLIP0) / BOUNCE_CLIP_ROWS);
}

static uint8_t clip_row_kind(uint8_t idx)
{
    return (uint8_t)((idx - BOUNCE_ROW_CLIP0) % BOUNCE_CLIP_ROWS);
}

static bool is_clip_row(uint8_t idx)
{
    return idx >= BOUNCE_ROW_CLIP0 && idx < BOUNCE_ROW_SAMPLE;
}

const char *bounce_menu_title(void)
{
    return "BOUNCE";
}

const menu_item_view_t *bounce_menu_build_items(void)
{
    clip_bounce_shape_t sh;
    clip_bounce_get_shape(&sh);

    snprintf(s_items[BOUNCE_ROW_SLOT].label, MENU_LABEL_LEN, "Slot");
    snprintf(s_items[BOUNCE_ROW_SLOT].value, MENU_VALUE_LEN, "%u %s",
             (unsigned)(s_slot + 1u), slot_state_word(s_slot));

    snprintf(s_items[BOUNCE_ROW_LEN].label, MENU_LABEL_LEN, "Max");
    snprintf(s_items[BOUNCE_ROW_LEN].value, MENU_VALUE_LEN, "%u %s",
             (unsigned)sh.max_bars, (sh.max_bars == 1u) ? "bar" : "bars");

    snprintf(s_items[BOUNCE_ROW_FMT].label, MENU_LABEL_LEN, "Fmt");
    snprintf(s_items[BOUNCE_ROW_FMT].value, MENU_VALUE_LEN, "%s",
             sh.stereo ? "Stereo" : "Mono");

    /* Ring-out folded into the loop head after the last bar. */
    snprintf(s_items[BOUNCE_ROW_TAIL].label, MENU_LABEL_LEN, "Tail");
    snprintf(s_items[BOUNCE_ROW_TAIL].value, MENU_VALUE_LEN, "%s",
             (sh.tail == CLIP_TAIL_NONE) ? "Off" :
             (sh.tail == CLIP_TAIL_BEAT) ? "Beat" : "Bar");

    /* Mute keeps the source patterns behind their mute flags; Clear empties
     * them so the layers are ready for the next part. Undo covers both. */
    snprintf(s_items[BOUNCE_ROW_AFTER].label, MENU_LABEL_LEN, "After");
    snprintf(s_items[BOUNCE_ROW_AFTER].value, MENU_VALUE_LEN, "%s",
             (sh.after == CLIP_AFTER_CLEAR) ? "Clear" : "Mute");

    /* Read-only: what this bounce would cost and whether the pool holds it.
     * The size is capped at four digits so the row cannot be truncated: the
     * pool is CLIP_BOUNCE_POOL_KB, orders below the cap. */
    {
        unsigned long kb = (unsigned long)(clip_bounce_bytes_for(&sh) / 1024u);
        if (kb > 9999ul) kb = 9999ul;
        snprintf(s_items[BOUNCE_ROW_MEM].label, MENU_LABEL_LEN, "Mem");
        snprintf(s_items[BOUNCE_ROW_MEM].value, MENU_VALUE_LEN, "%lu KB %s",
                 kb, clip_bounce_fits(&sh) ? "ok" : "full");
    }

    snprintf(s_items[BOUNCE_ROW_REC].label, MENU_LABEL_LEN, "Rec");
    switch (clip_bounce_get_state()) {
        case CLIP_BOUNCE_ARMED:
            snprintf(s_items[BOUNCE_ROW_REC].value, MENU_VALUE_LEN, "Wait bar");
            break;
        case CLIP_BOUNCE_RECORDING:
            /* "Rec" runs to the reservation; "End" once a stop fixed the
             * length. Either way a click stops. */
            snprintf(s_items[BOUNCE_ROW_REC].value, MENU_VALUE_LEN, "%s %u/%u",
                     clip_bounce_stop_set() ? "End" : "Rec",
                     (unsigned)clip_bounce_bars_done(),
                     (unsigned)clip_bounce_bars_total());
            break;
        case CLIP_BOUNCE_TAIL:
            snprintf(s_items[BOUNCE_ROW_REC].value, MENU_VALUE_LEN, "Tail");
            break;
        case CLIP_BOUNCE_READY:
            snprintf(s_items[BOUNCE_ROW_REC].value, MENU_VALUE_LEN, "Done");
            break;
        case CLIP_BOUNCE_IDLE:
        default:
            snprintf(s_items[BOUNCE_ROW_REC].value, MENU_VALUE_LEN, "%s",
                     (s_fail[0] != '\0') ? s_fail : "Start");
            break;
    }

    snprintf(s_items[BOUNCE_ROW_CANCEL].label, MENU_LABEL_LEN, "Cancel");
    s_items[BOUNCE_ROW_CANCEL].value[0] = '\0';

    /* One level: what the last commit replaced (the flags, or the cleared
     * patterns) comes back and its clip goes; a no-op once used or before
     * any bounce. */
    snprintf(s_items[BOUNCE_ROW_UNDO].label, MENU_LABEL_LEN, "Undo bounce");
    s_items[BOUNCE_ROW_UNDO].value[0] = '\0';

    for (uint8_t s = 0; s < CLIP_SLOT_COUNT; s++) {
        uint8_t base   = (uint8_t)(BOUNCE_ROW_CLIP0 + s * BOUNCE_CLIP_ROWS);
        bool    loaded = clip_player_slot_state(s) == CLIP_SLOT_LOADED;

        snprintf(s_items[base].label, MENU_LABEL_LEN, "Clip %u",
                 (unsigned)(s + 1u));
        if (loaded) {
            snprintf(s_items[base].value, MENU_VALUE_LEN, "%s",
                     clip_player_get_playing(s) ? "Play" : "Mute");
        } else {
            snprintf(s_items[base].value, MENU_VALUE_LEN, "empty");
        }

        /* The level is the slot's, not the clip's: it survives a clear so the
         * next bounce into the slot comes back at the same balance. */
        snprintf(s_items[base + 1].label, MENU_LABEL_LEN, "Clip %u Lvl",
                 (unsigned)(s + 1u));
        snprintf(s_items[base + 1].value, MENU_VALUE_LEN, "%u%%",
                 (unsigned)clip_player_get_level(s));

        /* Tempo regime away from the recorded tempo: stretch keeps the pitch,
         * vari keeps the transients and lets the pitch follow the rate. */
        snprintf(s_items[base + 2].label, MENU_LABEL_LEN, "Clip %u Tempo",
                 (unsigned)(s + 1u));
        snprintf(s_items[base + 2].value, MENU_VALUE_LEN, "%s",
                 clip_player_get_tempo_mode(s) == CLIP_TEMPO_VARI ? "Vari" : "Stretch");

        snprintf(s_items[base + 3].label, MENU_LABEL_LEN, "Clip %u Clr",
                 (unsigned)(s + 1u));
        s_items[base + 3].value[0] = '\0';
    }

    /* Idle, the value previews what Arm will target: the track is taken from
     * the grid cursor at the moment of arming. */
    snprintf(s_items[BOUNCE_ROW_SAMPLE].label, MENU_LABEL_LEN, "Sample");
    switch (sample_rec_get_state()) {
        case SAMPLE_REC_ARMED:
            snprintf(s_items[BOUNCE_ROW_SAMPLE].value, MENU_VALUE_LEN, "Rec!");
            break;
        case SAMPLE_REC_RECORDING:
            snprintf(s_items[BOUNCE_ROW_SAMPLE].value, MENU_VALUE_LEN, "Rec %u%%",
                     (unsigned)sample_rec_get_progress_pct());
            break;
        case SAMPLE_REC_READY:
            snprintf(s_items[BOUNCE_ROW_SAMPLE].value, MENU_VALUE_LEN, "Assign?");
            break;
        case SAMPLE_REC_IDLE:
        default:
            if (s_smp_fail[0] != '\0') {
                snprintf(s_items[BOUNCE_ROW_SAMPLE].value, MENU_VALUE_LEN, "%s", s_smp_fail);
            } else {
                snprintf(s_items[BOUNCE_ROW_SAMPLE].value, MENU_VALUE_LEN, "Arm T%u",
                         (unsigned)(seq_state.selected_track + 1));
            }
            break;
    }
    snprintf(s_items[BOUNCE_ROW_SMP_CANCEL].label, MENU_LABEL_LEN, "Smp Cancel");
    s_items[BOUNCE_ROW_SMP_CANCEL].value[0] = '\0';

    snprintf(s_items[BOUNCE_ROW_BACK].label, MENU_LABEL_LEN, "< Back");
    s_items[BOUNCE_ROW_BACK].value[0] = '\0';
    return s_items;
}

uint8_t bounce_menu_item_count(void)
{
    return (uint8_t)BOUNCE_ROW_COUNT;
}

bool bounce_menu_item_is_back(uint8_t idx)
{
    return idx == BOUNCE_ROW_BACK;
}

bool bounce_menu_item_is_value(uint8_t idx)
{
    if (idx == BOUNCE_ROW_SLOT || idx == BOUNCE_ROW_LEN ||
        idx == BOUNCE_ROW_FMT || idx == BOUNCE_ROW_TAIL ||
        idx == BOUNCE_ROW_AFTER) {
        return true;
    }
    /* Of the clip rows, play/mute, level and tempo hold a value; Clr is an
     * action. */
    return is_clip_row(idx) && clip_row_kind(idx) != 3;
}

/* Returns the new menu_editing state (mirrors chords_menu_handle_click). */
bool bounce_menu_handle_click(uint8_t idx)
{
    if (bounce_menu_item_is_value(idx)) {
        return !seq_state.menu_editing;   /* toggle value editing */
    }

    if (idx == BOUNCE_ROW_REC) {
        switch (clip_bounce_get_state()) {
            case CLIP_BOUNCE_ARMED:     clip_bounce_cancel(); return false;
            case CLIP_BOUNCE_RECORDING: clip_bounce_stop();   return false;
            case CLIP_BOUNCE_IDLE:      break;
            default:                    return false;
        }
        s_fail[0] = '\0';
        clip_bounce_shape_t sh;
        clip_bounce_get_shape(&sh);
        if (!clip_bounce_start(s_slot)) {
            /* Name the obligation that was violated, in the order start
             * checks them; "failed" is left for an OOM. */
            const char *why = "failed";
            if (clip_player_slot_state(s_slot) != CLIP_SLOT_EMPTY) {
                why = "slot used";
            } else if (sequencer_core_next_bar_tick() == 0) {
                why = "play first";
            } else if (!clip_bounce_fits(&sh)) {
                why = "no room";
            }
            snprintf(s_fail, sizeof(s_fail), "%s", why);
        }
        return false;
    }

    if (idx == BOUNCE_ROW_CANCEL) {
        clip_bounce_cancel();
        return false;
    }

    if (idx == BOUNCE_ROW_UNDO) {
        clip_bounce_undo();
        return false;
    }

    if (is_clip_row(idx) && clip_row_kind(idx) == 3) {
        uint8_t s = clip_row_slot(idx);
        if (clip_player_slot_state(s) != CLIP_SLOT_EMPTY) clip_player_clear(s);
        return false;
    }

    /* One row walks the sampler: arm -> start -> (auto stop) -> assign. The
     * page stays open and the service redraws the progress. */
    if (idx == BOUNCE_ROW_SAMPLE) {
        s_smp_fail[0] = '\0';
        switch (sample_rec_get_state()) {
            case SAMPLE_REC_IDLE: {
                uint8_t li = seq_state.active_layer_idx;
                if (seq_state.layers[li].type != SEQ_LAYER_DRUM) {
                    snprintf(s_smp_fail, sizeof(s_smp_fail), "drum layer?");
                } else if (!sample_rec_arm(li, seq_state.selected_track)) {
                    snprintf(s_smp_fail, sizeof(s_smp_fail), "failed");
                }
                break;
            }
            case SAMPLE_REC_ARMED:     sample_rec_start();  break;
            case SAMPLE_REC_READY:     sample_rec_assign(); break;
            case SAMPLE_REC_RECORDING:
            default:                   break;   /* runs to its own stop */
        }
        return false;
    }

    if (idx == BOUNCE_ROW_SMP_CANCEL) {
        s_smp_fail[0] = '\0';
        sample_rec_cancel();
        return false;
    }

    return false;
}

void bounce_menu_edit_value(uint8_t idx, int delta)
{
    if (delta == 0) return;

    int dir   = (delta > 0) ? 1 : -1;
    int steps = (delta > 0) ? delta : -delta;

    clip_bounce_shape_t sh;
    clip_bounce_get_shape(&sh);

    if (idx == BOUNCE_ROW_SLOT) {
        int n = (int)s_slot + dir * steps;
        if (n < 0) n = 0;
        if (n > (int)CLIP_SLOT_COUNT - 1) n = (int)CLIP_SLOT_COUNT - 1;
        s_slot = (uint8_t)n;
    } else if (idx == BOUNCE_ROW_LEN) {
        int i = (int)bars_step_index(sh.max_bars) + dir * steps;
        if (i < 0) i = 0;
        if (i > (int)BOUNCE_BAR_STEP_COUNT - 1) i = (int)BOUNCE_BAR_STEP_COUNT - 1;
        sh.max_bars = BOUNCE_BAR_STEPS[i];
        clip_bounce_set_shape(&sh);
    } else if (idx == BOUNCE_ROW_FMT) {
        sh.stereo = !sh.stereo;
        clip_bounce_set_shape(&sh);
    } else if (idx == BOUNCE_ROW_TAIL) {
        int t = (int)sh.tail + dir * steps;
        if (t < (int)CLIP_TAIL_NONE) t = (int)CLIP_TAIL_NONE;
        if (t > (int)CLIP_TAIL_BAR)  t = (int)CLIP_TAIL_BAR;
        sh.tail = (clip_tail_t)t;
        clip_bounce_set_shape(&sh);
    } else if (idx == BOUNCE_ROW_AFTER) {
        sh.after = (sh.after == CLIP_AFTER_CLEAR) ? CLIP_AFTER_MUTE : CLIP_AFTER_CLEAR;
        clip_bounce_set_shape(&sh);
    } else if (is_clip_row(idx)) {
        uint8_t s = clip_row_slot(idx);
        if (clip_row_kind(idx) == 0) {
            if (clip_player_slot_state(s) == CLIP_SLOT_LOADED)
                clip_player_set_playing(s, !clip_player_get_playing(s));
        } else if (clip_row_kind(idx) == 1) {
            /* Editable on an empty slot too - the level is the slot's. */
            int lvl = (int)clip_player_get_level(s) + dir * steps * 5;
            if (lvl < 0)   lvl = 0;
            if (lvl > 100) lvl = 100;
            clip_player_set_level(s, (uint8_t)lvl);
        } else if (clip_row_kind(idx) == 2) {
            clip_tempo_mode_t m = clip_player_get_tempo_mode(s) == CLIP_TEMPO_VARI
                                  ? CLIP_TEMPO_STRETCH : CLIP_TEMPO_VARI;
            clip_player_set_tempo_mode(s, m);
        }
    }
}

void bounce_menu_reset(void)
{
    s_fail[0] = '\0';
    s_smp_fail[0] = '\0';
}

/* The engine moves the bounce state and the slot states on the render task,
 * so nothing in the input path knows to redraw. Poll them and ask for a frame
 * only when something the page shows actually moved. */
void bounce_menu_service(void)
{
    if (!menu_bounce_page_open()) return;

    bool changed = false;

    clip_bounce_state_t st   = clip_bounce_get_state();
    uint8_t             done = clip_bounce_bars_done();
    bool                stop = clip_bounce_stop_set();
    if (st != s_snap_state || done != s_snap_bars_done || stop != s_snap_stop_set) {
        s_snap_state     = st;
        s_snap_bars_done = done;
        s_snap_stop_set  = stop;
        changed = true;
    }

    for (uint8_t s = 0; s < CLIP_SLOT_COUNT; s++) {
        clip_slot_state_t ss = clip_player_slot_state(s);
        bool              pl = clip_player_get_playing(s);
        if (ss != s_snap_slot[s] || pl != s_snap_playing[s]) {
            s_snap_slot[s]    = ss;
            s_snap_playing[s] = pl;
            changed = true;
        }
    }

    sample_rec_state_t ss  = sample_rec_get_state();
    uint8_t            pct = sample_rec_get_progress_pct();
    if (ss != s_snap_smp_state || pct != s_snap_smp_pct) {
        s_snap_smp_state = ss;
        s_snap_smp_pct   = pct;
        changed = true;
    }

    if (changed) s_force_redraw = true;
}

/* SHIFT+0 from any screen (main.c button dispatch). The bounce module does
 * the transport; the redraw request is for the REC badge and, if open, the
 * page. */
void synth_ui_bounce_chord(bool long_press)
{
    clip_bounce_chord(long_press);
    s_force_redraw = true;
}
