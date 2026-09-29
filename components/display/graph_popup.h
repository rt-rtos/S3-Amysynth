#pragma once
/*
 * graph_popup — reusable pop-up graph box for the U8g2 (SSD1315 128x64) UI.
 *
 * Draws and optionally edits a curve inside a framed pop-up box overlaying the
 * main screen (AD/ADSR envelopes, filter curves, waveform previews).
 *
 * Design:
 *   - AMY- and sequencer-agnostic: operates purely on caller-owned *normalised*
 *     points (x and y each in 0.0..1.0).
 *   - The host opens it, seeds the points, routes input, and reads them back.
 *     Domain mapping (ms, levels, log-freq) lives in adapters outside this file.
 *
 * Rendering contract:
 *   graph_popup_draw() draws ONLY the pop-up box. The host owns
 *   u8g2_ClearBuffer(), any surrounding frame, and u8g2_SendBuffer().
 *
 * Threading:
 *   Plain state, no internal locking. If input and render run on different
 *   tasks, the host must serialise access to gpopup_t the same way it does for
 *   its other shared UI state.
 */

#include <stdint.h>
#include <stdbool.h>

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum points a single curve can hold. ADSR uses ~3; 16 is ample. */
#define GPOPUP_MAX_POINTS 16

/* A single curve point. Both coordinates are normalised to 0.0..1.0,
 * where x=0 is the left edge of the plot, y=0 is the bottom of the plot. */
typedef struct {
    float x;
    float y;
} gpopup_point_t;

typedef enum {
    GPOPUP_MODE_VIEW = 0,   /* read-only visualisation (waveforms, previews) */
    GPOPUP_MODE_EDIT = 1,   /* interactive point editing                    */
} gpopup_mode_t;

typedef enum {
    GPOPUP_RESULT_NONE = 0,  /* still open / no terminal event this tick      */
    GPOPUP_RESULT_CONFIRMED, /* user confirmed (caller should read points)    */
    GPOPUP_RESULT_CANCELLED, /* user cancelled (caller should discard)        */
} gpopup_result_t;

/* Rendering/editing style. PLAIN is the generic widget. ADSR treats the 4
 * points as [origin, Attack-peak, Decay/Sustain, Release-end] and adds
 * role-aware clamping (monotonic time; A.y pinned to 1.0, R.y to 0.0; only the
 * sustain level free in Y), an area fill, A/D/S/R labels, and bottom-margin
 * time ticks. Still AMY-agnostic: it knows the point ROLES, not ms. */
typedef enum {
    GPOPUP_STYLE_PLAIN = 0,
    GPOPUP_STYLE_ADSR  = 1,
} gpopup_style_t;

/* Optional per-segment curve shaping. Called per plot column with the segment's
 * endpoint levels v0/v1 (normalised 0..1) and the position t (0..1) within the
 * segment; returns the curve level at t. NULL = straight-line segments. Lets
 * the host render a non-linear envelope's true shape; points stay the editable
 * anchors, only the drawn path between them changes. */
typedef float (*gpopup_shape_fn)(float v0, float v1, float t);

/* Optional X-axis stepping override for point editing. Called with the point
 * index, its current normalised X and the encoder detent count; returns the new
 * normalised X (clamped to 0..1 by the widget). Lets the host step in its own
 * domain units (e.g. audio-taper time). NULL = fixed linear step. */
typedef float (*gpopup_xstep_fn)(uint8_t idx, float x, long delta);

typedef struct {
    bool           active;
    gpopup_mode_t  mode;
    gpopup_style_t style;                        /* PLAIN or ADSR (see above)   */
    const char    *title;                       /* optional; NULL = none       */
    uint8_t        x, y, w, h;                   /* box rect on the 128x64 OLED */
    gpopup_point_t points[GPOPUP_MAX_POINTS];    /* the curve (caller-seeded)   */
    uint8_t        num_points;
    uint8_t        cursor;                       /* selected point index (edit) */
    bool           editing_value;                /* false=selecting, true=adjusting */
    bool           adjust_axis_y;                /* true=encoder edits Y, false=X   */
    /* Normalised X tick positions for the bottom margin (ADSR style only).
     * Host-filled via graph_popup_set_ticks(): the time->X mapping lives
     * outside this widget. */
    float          ticks[GPOPUP_MAX_POINTS];
    uint8_t        num_ticks;
    /* ADSR style: when true the sustain point (index 2) is Y-only - its X
     * (decay time) is host-derived, never user-draggable. */
    bool           adsr_lock_sx;
    /* Optional segment shaping for the drawn curve (see gpopup_shape_fn). */
    gpopup_shape_fn shape;
    /* Optional X-axis stepping override for edits (see gpopup_xstep_fn). */
    gpopup_xstep_fn xstep;
    /* Minimum normalised X separation between consecutive points. Host-settable
     * because the real floor is in the host's units (ms) and its normalised
     * equivalent shifts with the time axis. Default GPOPUP_DEFAULT_MIN_X_GAP. */
    float           min_x_gap;
} gpopup_t;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/* Initialise the widget state and box rectangle. Clears points, inactive. */
void graph_popup_init(gpopup_t *p, uint8_t x, uint8_t y, uint8_t w, uint8_t h);

/* Replace the curve points. Coordinates are clamped to 0..1. n is clamped to
 * GPOPUP_MAX_POINTS. Safe to call before or after open(). */
void graph_popup_set_points(gpopup_t *p, const gpopup_point_t *pts, uint8_t n);

/* Set the minimum normalised X separation between consecutive points: pass the
 * normalised equivalent of the host's own floor so the plot is never the
 * stricter constraint. Clamped; re-call after any host axis rescale. */
void graph_popup_set_min_x_gap(gpopup_t *p, float gap);

/* Open the pop-up in the given mode with an optional title. Resets the edit
 * cursor/state. The caller should have seeded points via set_points() first. */
void graph_popup_open(gpopup_t *p, gpopup_mode_t mode, const char *title);

/* Select the rendering/editing style (PLAIN or ADSR). Safe to call any time;
 * takes effect on the next draw/edit. Default after init is PLAIN. */
void graph_popup_set_style(gpopup_t *p, gpopup_style_t style);

/* Set the normalised X positions (0..1) of bottom-margin tick marks. Only drawn
 * in ADSR style. n is clamped to GPOPUP_MAX_POINTS. Pass n=0 to clear. */
void graph_popup_set_ticks(gpopup_t *p, const float *xs, uint8_t n);

/* ADSR style: lock the sustain point's X (decay time); the host derives and
 * writes S.x. The point stays Y-editable. No-op outside ADSR style. */
void graph_popup_set_adsr_lock_sx(gpopup_t *p, bool lock);

/* Install (or clear, with NULL) the segment-shaping callback used when drawing
 * the curve and its under-fill. Editing/points are unaffected. */
void graph_popup_set_shape(gpopup_t *p, gpopup_shape_fn fn);

/* Install (or clear, with NULL) the X-axis stepping override used when the
 * encoder adjusts a point's X. Y stepping keeps the fixed normalised stride. */
void graph_popup_set_xstep(gpopup_t *p, gpopup_xstep_fn fn);

/* Close the pop-up (sets active = false). Points are retained. */
void graph_popup_close(gpopup_t *p);

/* True while the pop-up is open. */
bool graph_popup_is_active(const gpopup_t *p);

/* Read back the current points after a CONFIRMED result. Returns the number of
 * points copied (<= max_out). out may be NULL to just query num_points. */
uint8_t graph_popup_get_points(const gpopup_t *p, gpopup_point_t *out, uint8_t max_out);

/* ── Render ─────────────────────────────────────────────────────────────── */

/* Draw the pop-up box (frame, axes, curve, cursor). Host owns ClearBuffer,
 * any context frame, and SendBuffer. No-op if the pop-up is not active. */
void graph_popup_draw(u8g2_t *u8g2, const gpopup_t *p);

/* ── Input ──────────────────────────────────────────────────────────────── */
/* Call these from the host input handlers while the pop-up is active. They
 * return a gpopup_result_t telling the host when to close/read back. In VIEW
 * mode, encoder/short-press are inert; long-press still cancels. */

gpopup_result_t graph_popup_handle_encoder(gpopup_t *p, long delta);
gpopup_result_t graph_popup_handle_button(gpopup_t *p);       /* short press   */
gpopup_result_t graph_popup_handle_button_long(gpopup_t *p);  /* long = cancel */

#ifdef __cplusplus
}
#endif
