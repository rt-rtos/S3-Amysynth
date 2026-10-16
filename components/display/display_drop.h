#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Ball drop screen renderer ────────────────────────────────────────────
 * The engine lives in synth_core (drop_core); synth_ui builds this flat view
 * each frame and the renderer draws it.
 *
 * Layout, on the panel's yellow (rows 0..15) / blue (16..63) split:
 *   ON  D  138  C3                 <- header row 1 (yellow): enable, focus,
 *                                     patch (display only), root
 *   2b  1A2F*  1/4                 <- header row 2 (yellow): bars, seed, rate
 *   [16 x 8 cells, 8 x 6 px each]  <- the field (blue), y 16..63: segments as
 *                                     lines at their angle, jackpot cells as
 *                                     hatched boxes, balls as 2 x 2 dots, the
 *                                     spawner as a chevron on the top edge
 *
 * Cursor index space: the header fields DROP_CUR_* first (the screen opens
 * on ENABLE), then the cells from DROP_CUR_CELL0, row-major. The patch
 * number is display-only; it changes with the hold+turn gesture. */

#define DROP_VIEW_COLS   16
#define DROP_VIEW_ROWS   8
#define DROP_VIEW_CELL_W 8
#define DROP_VIEW_CELL_H 6
#define DROP_VIEW_Y0     16      /* first field row's top edge: the blue band */
#define DROP_VIEW_CELLS  (DROP_VIEW_COLS * DROP_VIEW_ROWS)
#define DROP_VIEW_SEGS   16
#define DROP_VIEW_BALLS  8

#define DROP_CUR_ENABLE  0
#define DROP_CUR_FOCUS   1
#define DROP_CUR_ROOT    2
#define DROP_CUR_BARS    3
#define DROP_CUR_SEED    4
#define DROP_CUR_RATE    5
#define DROP_CUR_CELL0   6
#define DROP_CUR_COUNT   (DROP_CUR_CELL0 + DROP_VIEW_CELLS)

typedef struct {
    uint8_t col, row;
    uint8_t angle;    /* 0..7, 22.5 degree steps from horizontal */
    uint8_t type;     /* 0 plain, 1 split (notched), 2 damp (dotted) */
    uint8_t ratio;    /* split: percent sent left */
} drop_view_seg_t;

typedef struct {
    bool        enabled;
    bool        muted;
    uint8_t     focus;          /* 0 = DROP synth, 1 = SUB synth */
    uint16_t    patch;          /* the focused synth's */
    bool        patch_select;   /* hold+turn gesture active: name banner */
    const char *patch_name;     /* or NULL */
    char        root_name[4];   /* the focused synth's root */
    const char *rate_str;
    uint8_t     loop_bars;
    uint16_t    seed16;         /* low 16 bits of the seed */
    bool        seed_frozen;
    uint8_t     seg_count;
    drop_view_seg_t segs[DROP_VIEW_SEGS];
    uint8_t     jackpot[(DROP_VIEW_CELLS + 7) / 8];   /* one bit per cell, row-major */
    uint8_t     ball_count;
    struct { uint8_t x, y; } balls[DROP_VIEW_BALLS];   /* field pixels */
    uint8_t     spawn_col;
    uint8_t     cursor;         /* 0..DROP_CUR_COUNT-1 */
    bool        editing;
    const char *banner;         /* timed banner (solo) or NULL */
} drop_view_t;

void display_drop_draw_frame(u8g2_t *u8g2, const drop_view_t *view);

#ifdef __cplusplus
}
#endif
