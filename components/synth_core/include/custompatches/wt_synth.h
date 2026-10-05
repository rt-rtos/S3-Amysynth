#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Custom wavetable synthesis (the in-app builder's math) ────────────────
 * Turns sixteen parameter bytes into a 64-frame x 256-sample AMY wavetable.
 * Each frame is a sum of harmonics 1..H, so it is band-limited, periodic and
 * DC-free by construction. This file is the single home of the spectrum
 * model; wt_builder.h owns the parameters, the buffers and the slicing.
 *
 * Three keyframes, index 0 = A (frame 0), 1 = M (frame WT_MID_FRAME = 32)
 * and 2 = B (frame 63). Frame f <= 32 interpolates A -> M with u = f/32, a
 * later frame M -> B with u = (f - 32)/31, so frame 32 is exactly M. Within
 * a segment shape, width, bright and the peak amount interpolate linearly,
 * the sync ratio and the peak position geometrically.
 *
 * Peak: a keyframe's amount is 1 if its peak is set, else 0. A set peak sits
 * on its own harmonic. An Off A takes M's position if M is set, else B's; an
 * Off B takes M's, else A's; an Off M takes sqrt(A * B) if both are set,
 * else the one that is set. So a peak fades in place; all three Off is no
 * peak. Parameter domains (each per keyframe except range):
 *   shape  0..100   the synced waveform: saw at 0 to square at 100 (a blend
 *                   of the two hard-synced spectra)
 *   width  10..90   pulse width of the square part in percent; 50 is the
 *                   symmetric square, no effect at shape 0
 *   bright 0..10    slope above the sync harmonic: factor
 *                   min(1, (n/r)^-(2.5 * (1 - bright/10))); 10 leaves the
 *                   slave's own spectrum
 *   sync   10..80   hard-sync ratio x10 (1.0..8.0); 10 = the plain saw. Every
 *                   H is above 8, so harmonic r and some above it survive
 *   peak   0, 2..63 0 = Off, else a +12 dB Gaussian bump (sigma 1/6 octave)
 *                   centred on that harmonic
 *   range  0..2     harmonic cap H = 63, 31, 15
 *
 * Per frame: the exact spectrum of the hard-synced slave (saw/square blend)
 * for the effective ratio, the Bright slope, a levelling gain on that
 * spectrum (Parseval) to the RMS of a plain falling saw at peak 32000
 * (32000/sqrt(3)), capped at 4x (a frame under 1e-3 of that reference takes
 * the cap, so silent frames fade instead of boosting a residue), then the
 * Peak, then a 256-point inverse real FFT. Convention:
 * frame(x) = sum_n Re(c[n] e^{i n x}).
 *
 * Level: a narrow pulse has a high crest factor, and since wt_synth_finish()
 * scales the whole table to its peak, a table containing a 10 % pulse comes
 * out several dB quieter overall.
 *
 * Contract: stateless apart from a twiddle table filled once on first use
 * (idempotent); no AMY, no FreeRTOS, no allocation; float only; compiles on
 * the host. Callable from any task, never from the render path or an ISR. */

#define WT_FRAMES          64
#define WT_CYCLE           256     /* WAVETABLE_SAMPLES_PER_CYCLE, oscillators.c */
#define WT_PREVIEW_POINTS  128
#define WT_KEYS            3
#define WT_MID_FRAME       32
#define WT_RANGES          3

typedef struct {
    uint8_t shape[WT_KEYS];
    uint8_t width[WT_KEYS];
    uint8_t bright[WT_KEYS];
    uint8_t sync[WT_KEYS];
    uint8_t peak[WT_KEYS];
    uint8_t range;
} wt_params_t;                      /* index 0 = A, 1 = M, 2 = B */

/* Every 2nd sample of frames 0, WT_MID_FRAME and 63 of the finished table,
 * scaled to -127..127 relative to 32000. */
typedef struct {
    int8_t frame[WT_KEYS][WT_PREVIEW_POINTS];   /* frames 0, WT_MID_FRAME, 63 */
} wt_preview_t;

/* A = M = B = saw (shape 0, width 50, bright 10, sync 10, peak Off); range 1
 * (H = 31). */
void wt_params_default(wt_params_t *p);

/* Clamp every field into its domain; a peak of 1 becomes 0 (Off). */
void wt_params_clamp(wt_params_t *p);

/* Write keyframe 1 (M) as the halfway point of 0 and 2: rounded means of
 * shape, width and bright, rounded sqrt(a * b) of sync, and of peak (at
 * least 2) if both peaks are set, else peak Off. Pure; `p` clamped on entry
 * stays clamped. */
void wt_params_blend_mid(wt_params_t *p);

/* Harmonic cap H for a range index (out-of-domain ranges clamp to the last). */
uint8_t wt_synth_harmonics(uint8_t range);

/* Highest MIDI note whose fundamental f satisfies f <= 24000 / H, i.e. the
 * top note that plays without aliasing: 66 (F#4), 78 (F#5), 91 (G6). */
uint8_t wt_synth_clean_note(uint8_t range);

/* Write frames first .. first+count-1 into `frames`, the base of the whole
 * WT_FRAMES x WT_CYCLE float table (frame f at frames + f * WT_CYCLE). Each
 * frame's slot is also the FFT workspace. `p` must already be clamped. Frames
 * past WT_FRAMES are not written. Values are on the int16 scale, levelled but
 * not yet peak-limited. */
void wt_synth_build_frames(const wt_params_t *p, uint8_t first, uint8_t count, float *frames);

/* Finish a fully built table: if its peak exceeds 32000, scale the whole
 * table down to it; fill `pv` from frames 0, WT_MID_FRAME and 63; then
 * convert every sample to int16 into `out`. `out` may alias `frames`: the
 * conversion runs forward, and int16[i] at byte 2i never overtakes float[i]
 * at byte 4i. */
void wt_synth_finish(float *frames, int16_t *out, wt_preview_t *pv);

/* Preview of one frame of a finished int16 table into out[WT_PREVIEW_POINTS],
 * on wt_preview_t's scale and sample spacing. */
void wt_synth_preview_frame(const int16_t *frame, int8_t *out);

/* Harmonic levels of one frame of a finished int16 table. out[n - 1], for
 * n = 1..count (count <= 63), is harmonic n's attenuation below full scale
 * (amplitude 32000) in half-dB steps: 0 = full scale or above, 255 = 127.5 dB
 * down or silent. */
void wt_synth_frame_harmonics(const int16_t *frame, uint8_t count, uint8_t *out);

#ifdef __cplusplus
}
#endif
