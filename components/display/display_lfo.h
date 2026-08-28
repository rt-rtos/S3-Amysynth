#pragma once
#include "u8g2.h"
#include "display_seq.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Editor cursor fields. The target checkboxes come first with indices equal to
 * their lfo_target_t value, so `cursor < LFO_TARGET_COUNT` means "cursor is on
 * a target checkbox and its enum value is the cursor". More targets exist than
 * the left panel can show at once, so they are split across two tabs (see
 * lfo_view_t.tgt_tab) and only the shown tab's checkbox indices are reachable.
 * The shared LFO parameters follow. */
enum {
    LFO_FLD_WAVE = LFO_TARGET_COUNT,
    LFO_FLD_RATE,
    LFO_FLD_DEPTH,
    LFO_FLD_FLT_OCT,          /* FILTER swing in +/-octaves (quarter steps)   */
    LFO_FLD_WOB_RATE,         /* WOBBLE (second-order LFO) rate               */
    LFO_FLD_WOB_DEPTH,        /* WOBBLE amount in dB of swing, OFF = off      */
    LFO_FLD_WOB_MODE,         /* WOBBLE reach: depth only vs depth + rate     */
    LFO_FLD_EN,               /* drawn as a header chip, not a panel row      */
    LFO_FLD_COUNT,            /* total navigable fields */
};

/* Right-panel geometry. The parameter rows (WAVE..WOB_MODE) outnumber the
 * slots that fit above the hint strip, so the panel scrolls by one when the
 * cursor reaches the bottom row. A new parameter field only has to sit in this
 * range. */
#define LFO_PANEL_SLOTS  5
#define LFO_PANEL_ROWS   (LFO_FLD_WOB_MODE - LFO_FLD_WAVE + 1)

/* View state for the LFO editor overlay: the working copy being edited plus
 * display metadata (cursor, editing flag, target track). */
typedef struct {
    seq_lfo_t   lfo;
    uint8_t     cursor;       /* 0..LFO_TARGET_COUNT-1 = target checkbox;
                                 then WAVE/RATE/DEPTH/EN (see LFO_FLD_*)   */
    uint8_t     tgt_tab;      /* which half of the target checklist the left
                                 panel shows: 0 = the five carrier targets,
                                 1 = the distortion targets. The shoulder
                                 button flips it; the cursor never rests on a
                                 hidden tab's checkbox row.                */
    bool        editing;      /* WAVE/RATE/DEPTH adjust mode (never for
                                 checkbox or EN toggle fields)             */
    uint8_t     layer_idx;
    uint8_t     track_idx;
    char        src_badge[4]; /* header tag for the row's voice-block source
                                 and this tab's authority (">L", ">T", "PL",
                                 "PT"; "" for non-track targets)            */
    const char *target_label; /* "ARP"/"DRONE" label override, or NULL      */
    bool        wob_native;   /* target runs the native LFO carrier; false =
                                 software stepper, where the WOBBLE rows are
                                 inert and drawn struck-through             */
    bool        dist_inert;   /* domain has no software stepper for the DIST
                                 targets (drone): their checkboxes drawn
                                 struck-through                             */
} lfo_view_t;

void lfo_view_draw(u8g2_t *u8g2, const lfo_view_t *v);

#ifdef __cplusplus
}
#endif
