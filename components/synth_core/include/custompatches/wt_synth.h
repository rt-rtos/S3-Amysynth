#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Custom wavetable synthesis (the in-app builder's math) ────────────────
 * Turns nine parameter bytes into a 64-frame x 256-sample AMY wavetable.
 * Each frame is a sum of harmonics 1..H, so it is band-limited, periodic and
 * DC-free by construction. This file is the single home of the spectrum
 * model; wt_builder.h owns the parameters, the buffers and the slicing.
 *
 * Two keyframes, index 0 = A (frame 0) and 1 = B (frame 63). Per frame,
 * t = f/63: shape, bright and the peak amount interpolate linearly, the sync
 * ratio and the peak position geometrically. An Off peak counts as amount 0
 * at the other keyframe's position, so a peak fades in place; both Off is no
 * peak. Parameter domains:
 *   shape  0..100   the synced waveform: saw at 0 to square at 100 (a blend
 *                   of the two hard-synced spectra)
 *   bright 0..10    slope above the sync harmonic: factor
 *                   min(1, (n/r)^-(2.5 * (1 - bright/10))); 10 leaves the
 *                   slave's own spectrum
 *   sync   10..80   hard-sync ratio x10 (1.0..8.0); 10 = the plain saw. The
 *                   ratio used is capped at H, so harmonic r always survives
 *   peak   0, 2..63 0 = Off, else a +12 dB Gaussian bump (sigma 1/6 octave)
 *                   centred on that harmonic
 *   range  0..3     harmonic cap H = 63, 31, 15, 7
 *
 * Per frame: the exact spectrum of the hard-synced slave (saw/square blend)
 * for the effective ratio, the Bright slope, a levelling gain on that
 * spectrum (Parseval) to the RMS of a plain falling saw at peak 32000
 * (32000/sqrt(3)), capped at 4x (a frame under 1e-3 of that reference takes
 * the cap, so silent frames fade instead of boosting a residue), then the
 * Peak, then a 256-point inverse real FFT. Convention:
 * frame(x) = sum_n Re(c[n] e^{i n x}).
 *
 * Contract: stateless apart from a twiddle table filled once on first use
 * (idempotent); no AMY, no FreeRTOS, no allocation; float only; compiles on
 * the host. Callable from any task, never from the render path or an ISR. */

#define WT_FRAMES          64
#define WT_CYCLE           256     /* WAVETABLE_SAMPLES_PER_CYCLE, oscillators.c */
#define WT_PREVIEW_POINTS  64

typedef struct {
    uint8_t shape[2];
    uint8_t bright[2];
    uint8_t sync[2];
    uint8_t peak[2];
    uint8_t range;
} wt_params_t;

/* Every 4th sample of frames 0 and 63 of the finished table, scaled to
 * -127..127 relative to 32000. */
typedef struct {
    int8_t frame0[WT_PREVIEW_POINTS];
    int8_t frame63[WT_PREVIEW_POINTS];
} wt_preview_t;

/* A = B = saw (shape 0, bright 10, sync 10, peak Off); range 1 (H = 31). */
void wt_params_default(wt_params_t *p);

/* Clamp every field into its domain; a peak of 1 becomes 0 (Off). */
void wt_params_clamp(wt_params_t *p);

/* Harmonic cap H for a range index (out-of-domain ranges clamp to 3). */
uint8_t wt_synth_harmonics(uint8_t range);

/* Highest MIDI note whose fundamental f satisfies f <= 24000 / H, i.e. the
 * top note that plays without aliasing: 66 (F#4), 78, 91, 104 (G#7). */
uint8_t wt_synth_clean_note(uint8_t range);

/* Write frames first .. first+count-1 into `frames`, the base of the whole
 * WT_FRAMES x WT_CYCLE float table (frame f at frames + f * WT_CYCLE). Each
 * frame's slot is also the FFT workspace. `p` must already be clamped. Frames
 * past WT_FRAMES are not written. Values are on the int16 scale, levelled but
 * not yet peak-limited. */
void wt_synth_build_frames(const wt_params_t *p, uint8_t first, uint8_t count, float *frames);

/* Finish a fully built table: if its peak exceeds 32000, scale the whole
 * table down to it; fill `pv` from frames 0 and 63; then convert every
 * sample to int16 into `out`. `out` may alias `frames`: the conversion runs
 * forward, and int16[i] at byte 2i never overtakes float[i] at byte 4i. */
void wt_synth_finish(float *frames, int16_t *out, wt_preview_t *pv);

#ifdef __cplusplus
}
#endif
