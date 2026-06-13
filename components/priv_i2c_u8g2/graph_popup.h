#pragma once
/*
 * graph_popup — reusable pop-up graph box for the U8g2 (SSD1315 128x64) UI.
 *
 * Purpose:
 *   A single, self-contained widget for drawing and (optionally) editing a
 *   curve inside a framed pop-up box that overlays the main screen. It is used
 *   for AD/ADSR envelopes, filter curves, and waveform visualisation without
 *   re-implementing the graphing/editing logic each time.
 *
 * Design:
 *   - The widget is AMY-agnostic and sequencer-agnostic. It operates purely on
 *     a caller-owned array of *normalised* points (x and y each in 0.0..1.0).
 *   - The host decides WHEN to open it, seeds the points, routes input to it,
 *     and reads the (possibly edited) points back. Mapping domain values
 *     (ms, levels, log-freq) to/from the normalised 0..1 space lives in
 *     adapters outside this file (e.g. graph_popup_amy.c in sequencer_ui).
 *
 * Rendering contract:
 *   graph_popup_draw() draws ONLY the pop-up box (background, frame, axes,
 *   curve and — in edit mode — point markers/cursor). The host is responsible
 *   for u8g2_ClearBuffer(), any context frame around the box, and
 *   u8g2_SendBuffer().
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

/* Optional rendering / editing style. PLAIN keeps the original generic widget
 * behaviour. ADSR turns on envelope-specific behaviour: the 4 points are treated
 * as [origin, Attack-peak, Decay/Sustain, Release-end], which enables
 *   - role-aware clamping (monotonic time; A.y pinned to 1.0, R.y to 0.0; only
 *     the sustain level is freely movable in Y) so only valid, musical envelopes
 *     are reachable;
 *   - filled area under the curve;
 *   - A/D/S/R letter labels over the points;
 *   - minimal time tick marks in the bottom margin.
 * The widget stays AMY-agnostic: it only knows the ADSR point ROLES, not ms. */
typedef enum {
    GPOPUP_STYLE_PLAIN = 0,
    GPOPUP_STYLE_ADSR  = 1,
} gpopup_style_t;

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
    /* Normalised X tick positions drawn in the bottom margin (ADSR style only).
     * Host fills these via graph_popup_set_ticks() since the time->X mapping
     * lives outside this widget. */
    float          ticks[GPOPUP_MAX_POINTS];
    uint8_t        num_ticks;
} gpopup_t;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/* Initialise the widget state and box rectangle. Clears points, inactive. */
void graph_popup_init(gpopup_t *p, uint8_t x, uint8_t y, uint8_t w, uint8_t h);

/* Replace the curve points. Coordinates are clamped to 0..1. n is clamped to
 * GPOPUP_MAX_POINTS. Safe to call before or after open(). */
void graph_popup_set_points(gpopup_t *p, const gpopup_point_t *pts, uint8_t n);

/* Open the pop-up in the given mode with an optional title. Resets the edit
 * cursor/state. The caller should have seeded points via set_points() first. */
void graph_popup_open(gpopup_t *p, gpopup_mode_t mode, const char *title);

/* Select the rendering/editing style (PLAIN or ADSR). Safe to call any time;
 * takes effect on the next draw/edit. Default after init is PLAIN. */
void graph_popup_set_style(gpopup_t *p, gpopup_style_t style);

/* Set the normalised X positions (0..1) of bottom-margin tick marks. Only drawn
 * in ADSR style. n is clamped to GPOPUP_MAX_POINTS. Pass n=0 to clear. */
void graph_popup_set_ticks(gpopup_t *p, const float *xs, uint8_t n);

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

/* Toggle which axis the encoder edits while adjusting a point: Y (vertical /
 * level) <-> X (horizontal / time). No-op unless the pop-up is open in EDIT
 * mode. Returns true when the pop-up consumed the event. */
bool graph_popup_toggle_axis(gpopup_t *p);

/* True while the active adjust axis is Y (vertical). False = X (horizontal). */
bool graph_popup_axis_is_y(const gpopup_t *p);

/* ───────────────────────────────────────────────────────────────────────────
 * Optional AMY adapter (implemented in components/sequencer_ui/graph_popup_amy.c).
 * Declared here for convenience; the core widget above has NO AMY dependency.
 * These helpers convert between AMY envelope arrays (times in ms, values 0..1)
 * and the normalised point model used by the widget.
 * ───────────────────────────────────────────────────────────────────────── */

/* Build normalised points from an AMY EG (times in ms, values 0..1).
 * x = cumulative time / total time; y = value. An implicit origin point at
 * (0,0) is prepended so the curve starts at the bottom-left. Returns the
 * number of points written (<= max_out). */
uint8_t gpopup_points_from_envelope(gpopup_point_t *out, uint8_t max_out,
                                    const uint32_t *times_ms,
                                    const float    *values,
                                    uint8_t         num_bps);

/* Convert normalised (edited) points back to ms/value arrays sized for an AMY
 * EG. total_ms scales the x axis back to milliseconds. The implicit origin
 * point at index 0 is skipped. out_times_ms/out_values must hold at least
 * (n-1) entries. Returns the number of breakpoints written. */
uint8_t gpopup_points_to_envelope(const gpopup_point_t *pts, uint8_t n,
                                  uint32_t  total_ms,
                                  uint32_t *out_times_ms,
                                  float    *out_values);

#ifdef __cplusplus
}
#endif
