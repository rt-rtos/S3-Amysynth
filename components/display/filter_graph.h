#pragma once
#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Filter type indices — same numeric values as AMY's FILTER_* defines. */
#define FGRAPH_FILTER_NONE  0
#define FGRAPH_FILTER_LPF   1
#define FGRAPH_FILTER_BPF   2
#define FGRAPH_FILTER_HPF   3
#define FGRAPH_FILTER_LPF24 4
#define FGRAPH_FILTER_NOTCH 5
#define FGRAPH_FILTER_PHASER 6
#define FGRAPH_FILTER_COUNT 7

/* Frequency + resonance mapping constants (used by synth_ui for en/de-normalise). */
#define FGRAPH_CUTOFF_HZ_MIN   20.0f
#define FGRAPH_CUTOFF_HZ_MAX 8000.0f
/* Octave span of the log2-mapped cutoff axis = log2(MAX/MIN). Keep in sync
 * with the two values above; the editor derives its semitone step from it. */
#define FGRAPH_CUTOFF_OCTAVES 8.6438562f
#define FGRAPH_RES_MIN  0.51f
#define FGRAPH_RES_MAX  8.0f

/* Data passed from synth_ui (which owns synth_core access) to the renderer.
 * All values are normalised or primitive — the display component stays free
 * of any synth_core or AMY dependency. */
typedef struct {
    uint8_t filter_type;     /* FGRAPH_FILTER_* */
    float   cutoff_norm;     /* 0..1 log-mapped over [FGRAPH_CUTOFF_HZ_MIN, MAX] */
    float   resonance_norm;  /* 0..1 mapped from [FGRAPH_RES_MIN, FGRAPH_RES_MAX] */
    bool    has_feedback;    /* feedback wave (KS): the FB and DT cursors exist
                              * and "FB:xx%" / "DT:x.xx" readouts hang under the
                              * Q readout. Q stays editable - AMY applies the
                              * biquad to KS oscs like any other wave. */
    float   feedback_norm;   /* KS string feedback 0..1; only drawn/edited when
                              * has_feedback */
    float   ks_duty;         /* KS pluck duty 0..1 as AMY holds it (0.5 =
                              * default burst); only drawn/edited when
                              * has_feedback */
    uint8_t cursor;          /* 0=cutoff, 1=resonance, 2=feedback, 3=duty (2 and
                              * 3 on KS targets only — skipped otherwise),
                              * 4=type, 5=enable */
    bool    editing;         /* cursor is currently being adjusted */
    bool    enabled;         /* false → draw flat line + "OFF" */
    bool    show_toggles;    /* target exposes the type/enable cursors: true for
                              * melodic/arp, false for the drone (fixed LPF24,
                              * always on) */
    char    label[16];       /* left side of top bar, e.g. "L1 T2" or "ARP" */

    /* Liveness (CONFIG_FILTER_SCOPE): when the scope is armed the caller drives
     * cutoff_norm from the live modulated cutoff; this struct stays a pure
     * value snapshot, the renderer cannot tell authored from live. The caller
     * MUST quantise cutoff_norm to pixel columns, or the view signature hashes
     * differently every frame and the screen redraws continuously. Bottom rows
     * 57-63 are overwritten by the hint strip after this draws - never put
     * content there. */
} filter_graph_t;

/* Draw the full-screen filter editor (clears + sends the buffer).
 * Top bar Y 0-15, divider, frequency response plot Y 16-63. */
void filter_graph_draw(u8g2_t *u8g2, const filter_graph_t *fg);

#ifdef __cplusplus
}
#endif
