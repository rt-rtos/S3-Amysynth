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
 * the left panel can show at once, so the checklist scrolls with the cursor
 * (lfo_view_t.tgt_first). The shared LFO parameters follow. */
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

/* Left-panel geometry: LFO_TGT_SLOTS of the LFO_TARGET_COUNT target rows
 * show at once, from lfo_view_t.tgt_first. */
#define LFO_TGT_SLOTS    5

/* View state for the LFO editor overlay: the working copy being edited plus
 * display metadata (cursor, editing flag, target track). */
typedef struct {
    seq_lfo_t   lfo;
    uint8_t     cursor;       /* 0..LFO_TARGET_COUNT-1 = target checkbox;
                                 then WAVE/RATE/DEPTH/EN (see LFO_FLD_*)   */
    uint8_t     tgt_first;    /* target row drawn at the top of the left
                                 panel (0..LFO_TARGET_COUNT-LFO_TGT_SLOTS);
                                 the editor moves it so a cursor on a target
                                 row is always shown                       */
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
    float       flt_lo_hz;    /* the FILTER swing's low and high cutoff, Hz,
                                 drawn in the header while FILTER is a
                                 checked target; 0 = no readout (the
                                 surface's filter is off)                  */
    float       flt_hi_hz;
} lfo_view_t;

void lfo_view_draw(u8g2_t *u8g2, const lfo_view_t *v);

#ifdef __cplusplus
}
#endif
