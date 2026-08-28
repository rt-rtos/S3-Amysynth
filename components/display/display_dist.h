#pragma once
#include "u8g2.h"
#include "display_seq.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Editor cursor fields. Unlike the LFO page there are no checkbox fields and
 * no separate enable: DIST_FLD_TYPE's OFF value IS the bypass, so the five
 * parameter rows are the whole cursor range and the panel never scrolls. */
enum {
    DIST_FLD_TYPE = 0,   /* OFF / CLIP / FOLD / CRUSH                       */
    DIST_FLD_DRIVE,      /* pre-gain, fold depth for FOLD                   */
    DIST_FLD_BITS,       /* CRUSH bit depth; 24 = no-op (s8.23 samples)     */
    DIST_FLD_RATE,       /* CRUSH sample-hold length in samples             */
    DIST_FLD_MIX,        /* wet %                                           */
    DIST_FLD_COUNT,
};

/* Field ranges, shared with the editor's adjust handler so the renderer and the
 * input path cannot disagree about what a row can hold. */
#define DIST_DRIVE_MIN   1u
#define DIST_DRIVE_MAX  16u
#define DIST_BITS_MIN    1u
#define DIST_BITS_MAX   24u    /* AMY samples are s8.23: 24 bits = no-op */
#define DIST_RATE_MIN    1u
#define DIST_RATE_MAX   64u
#define DIST_MIX_STEP    5u

/* View state for the distortion editor overlay: the working copy being edited
 * plus display metadata. Mirrors lfo_view_t field for field where the two pages
 * share behaviour, so the editors read as siblings. */
typedef struct {
    seq_dist_t  dist;
    uint8_t     cursor;       /* DIST_FLD_*                                 */
    bool        editing;      /* encoder adjusts the value rather than the
                                 cursor                                     */
    uint8_t     layer_idx;
    uint8_t     track_idx;
    char        src_badge[4]; /* header tag for the row's voice-block source
                                 and this tab's authority (see lfo_view_t)  */
    const char *target_label; /* "ARP"/"DRONE"/"LIVE" override, or NULL     */
} dist_view_t;

void dist_view_draw(u8g2_t *u8g2, const dist_view_t *v);

#ifdef __cplusplus
}
#endif
