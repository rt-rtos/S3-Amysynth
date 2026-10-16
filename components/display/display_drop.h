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
 * Layout:
 *   ON D 138 C3 2b 1A2F*   1/4     <- status row, y 0..7 (cursor fields)
 *   [16 x 8 cells, 8 x 7 px each]  <- the field, y 8..63: segments as lines
 *                                     at their angle, the jackpot as a
 *                                     hatched box, balls as 2 x 2 dots, the
 *                                     spawner as a chevron on the top edge
 *
 * Cursor index space: 0..DROP_VIEW_CELLS-1 are the cells (row-major), then
 * the status fields DROP_CUR_*. The patch number is display-only; it changes
 * with the hold+turn gesture. */

#define DROP_VIEW_COLS   16
#define DROP_VIEW_ROWS   8
#define DROP_VIEW_CELL_W 8
#define DROP_VIEW_CELL_H 7
#define DROP_VIEW_Y0     8       /* first field row's top edge */
#define DROP_VIEW_CELLS  (DROP_VIEW_COLS * DROP_VIEW_ROWS)
#define DROP_VIEW_SEGS   16
#define DROP_VIEW_BALLS  8

#define DROP_CUR_ENABLE  (DROP_VIEW_CELLS + 0)
#define DROP_CUR_FOCUS   (DROP_VIEW_CELLS + 1)
#define DROP_CUR_ROOT    (DROP_VIEW_CELLS + 2)
#define DROP_CUR_BARS    (DROP_VIEW_CELLS + 3)
#define DROP_CUR_SEED    (DROP_VIEW_CELLS + 4)
#define DROP_CUR_RATE    (DROP_VIEW_CELLS + 5)
#define DROP_CUR_COUNT   (DROP_VIEW_CELLS + 6)

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
    struct { uint8_t col0, col1, row; bool enabled; } jackpot;
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
