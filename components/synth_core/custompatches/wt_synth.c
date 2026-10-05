#include "custompatches/wt_synth.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#define WT_PI          3.14159265358979f
#define WT_FULL_SCALE  32000.0f
#define WT_SAW_SCALE   32000.0f               /* base spectrum: the r = 1 saw peaks here */
#define WT_REF_RMS     18475.0f               /* 32000 / sqrt(3), that saw's RMS */
#define WT_SILENT_RMS  (1e-3f * WT_REF_RMS)
#define WT_GAIN_MAX    4.0f
#define WT_PEAK_BOOST  3.0f                   /* 1 + 3 = x4 = +12 dB at the centre */
#define WT_PEAK_K      18.0f                  /* 1 / (2 sigma^2), sigma = 1/6 octave */
#define WT_FFT_M       (WT_CYCLE / 2)         /* complex points of the half-length FFT */
#define WT_TW_LEN      (WT_CYCLE * 3 / 4)

static const uint8_t s_harmonics[4] = { 63, 31, 15, 7 };

/* sin(2 pi k / WT_CYCLE) over three quarters of a cycle, so cos(2 pi k /
 * WT_CYCLE) is entry k + WT_CYCLE/4. Filled once on first use; every caller
 * writes the same values, and the ready flag is published with release
 * order, so a reader that sees it set sees the whole table. */
static float s_sin[WT_TW_LEN];
static bool  s_sin_ready;

#define TW_SIN(k)  s_sin[(k)]
#define TW_COS(k)  s_sin[(k) + WT_CYCLE / 4]

static void twiddles_init(void)
{
    if (__atomic_load_n(&s_sin_ready, __ATOMIC_ACQUIRE)) return;
    for (int k = 0; k < WT_TW_LEN; k++) {
        s_sin[k] = sinf(2.0f * WT_PI * (float)k / (float)WT_CYCLE);
    }
    __atomic_store_n(&s_sin_ready, true, __ATOMIC_RELEASE);
}

void wt_params_default(wt_params_t *p)
{
    for (int k = 0; k < WT_KEYS; k++) {
        p->shape[k]  = 0;
        p->width[k]  = 50;
        p->bright[k] = 10;
        p->sync[k]   = 10;
        p->peak[k]   = 0;
    }
    p->range = 1;
}

void wt_params_clamp(wt_params_t *p)
{
    for (int k = 0; k < WT_KEYS; k++) {
        if (p->shape[k] > 100) p->shape[k] = 100;
        if (p->width[k] < 10)  p->width[k] = 10;
        if (p->width[k] > 90)  p->width[k] = 90;
        if (p->bright[k] > 10) p->bright[k] = 10;
        if (p->sync[k] < 10)   p->sync[k] = 10;
        if (p->sync[k] > 80)   p->sync[k] = 80;
        if (p->peak[k] == 1)   p->peak[k] = 0;
        if (p->peak[k] > 63)   p->peak[k] = 63;
    }
    if (p->range > 3) p->range = 3;
}

static uint8_t geo_mean(uint8_t a, uint8_t b)
{
    return (uint8_t)lrintf(sqrtf((float)a * (float)b));
}

void wt_params_blend_mid(wt_params_t *p)
{
    p->shape[1]  = (uint8_t)((p->shape[0]  + p->shape[2]  + 1) / 2);
    p->width[1]  = (uint8_t)((p->width[0]  + p->width[2]  + 1) / 2);
    p->bright[1] = (uint8_t)((p->bright[0] + p->bright[2] + 1) / 2);
    p->sync[1]   = geo_mean(p->sync[0], p->sync[2]);
    if (p->peak[0] && p->peak[2]) {
        uint8_t pk = geo_mean(p->peak[0], p->peak[2]);
        p->peak[1] = pk < 2 ? 2 : pk;
    } else {
        p->peak[1] = 0;
    }
}

uint8_t wt_synth_harmonics(uint8_t range)
{
    return s_harmonics[range > 3 ? 3 : range];
}

uint8_t wt_synth_clean_note(uint8_t range)
{
    float fmax = 24000.0f / (float)wt_synth_harmonics(range);
    return (uint8_t)floorf(69.0f + 12.0f * log2f(fmax / 440.0f));
}

/* Unnormalised inverse real FFT of one frame, in place. On entry buf[2n],
 * buf[2n+1] hold c[n] (n = 1..WT_FFT_M-1) and buf[0], buf[1] the real DC and
 * Nyquist terms; on exit buf[j] = sum_n Re(c[n] e^{2 pi i n j / WT_CYCLE})
 * plus those two. The half-length trick: z[m] = x[2m] + i x[2m+1] is the
 * WT_FFT_M-point inverse DFT of Z[k] = E[k] + i O[k], with E/O the even and
 * odd halves rebuilt from c[k] and c[M-k]; z lands interleaved, which is x. */
static void irfft_inplace(float *buf)
{
    const int M = WT_FFT_M;

    /* Unpack. The spectrum is Hermitian, X[k] = c[k]/2, hence the halves. */
    float d = buf[0], q = buf[1];
    buf[0] = d + q;
    buf[1] = d - q;
    for (int k = 1; k < M / 2; k++) {
        int j = M - k;
        float ar = buf[2 * k], ai = buf[2 * k + 1];
        float br = buf[2 * j], bi = buf[2 * j + 1];
        float dr = ar - br, di = ai + bi;
        float wc = TW_COS(k), ws = TW_SIN(k);
        float pr = wc * dr - ws * di;
        float pi = wc * di + ws * dr;
        buf[2 * k]     = 0.5f * (ar + br - pi);
        buf[2 * k + 1] = 0.5f * (ai - bi + pr);
        buf[2 * j]     = 0.5f * (ar + br + pi);
        buf[2 * j + 1] = 0.5f * (bi - ai + pr);
    }
    buf[M + 1] = -buf[M + 1];   /* k = M/2: Z = conj(c) */

    /* Bit-reversal permutation over the M complex points. */
    for (int i = 1, r = 0; i < M; i++) {
        int bit = M >> 1;
        for (; r & bit; bit >>= 1) r ^= bit;
        r |= bit;
        if (i < r) {
            float tr = buf[2 * i], ti = buf[2 * i + 1];
            buf[2 * i] = buf[2 * r];
            buf[2 * i + 1] = buf[2 * r + 1];
            buf[2 * r] = tr;
            buf[2 * r + 1] = ti;
        }
    }

    /* Radix-2 butterflies, twiddle e^{+2 pi i j / len} = table index j * N / len. */
    for (int len = 2; len <= M; len <<= 1) {
        int half = len >> 1;
        int stride = WT_CYCLE / len;
        for (int i = 0; i < M; i += len) {
            for (int j = 0; j < half; j++) {
                float wc = TW_COS(j * stride), ws = TW_SIN(j * stride);
                float *u = &buf[2 * (i + j)];
                float *v = &buf[2 * (i + j + half)];
                float vr = v[0] * wc - v[1] * ws;
                float vi = v[0] * ws + v[1] * wc;
                v[0] = u[0] - vr;
                v[1] = u[1] - vi;
                u[0] += vr;
                u[1] += vi;
            }
        }
    }
}

/* Accumulate one step of size `jump` at phase `th` into S[n] = sum J e^{-i n th},
 * n = 1..H: the rotation e^{-i th} is stepped once per harmonic. */
static void add_jump(float *buf, uint8_t H, float th, float jump)
{
    if (jump == 0.0f) return;
    float wr = cosf(th), wi = -sinf(th);
    float zr = 1.0f, zi = 0.0f;
    for (int n = 1; n <= H; n++) {
        float t = zr * wr - zi * wi;
        zi = zr * wi + zi * wr;
        zr = t;
        buf[2 * n]     += jump * zr;
        buf[2 * n + 1] += jump * zi;
    }
}

/* One frame, spectrum model steps 1-5, into its own WT_CYCLE-float slot.
 * `r` is the effective sync ratio (already capped at H); `width` is the
 * square part's pulse width, 0.10..0.90. */
static void build_frame(float *buf, uint8_t H, float shape, float width, float bright,
                        float r, float amount, float pos)
{
    memset(buf, 0, WT_CYCLE * sizeof(float));

    /* 1. The hard-synced slave, a blend (1 - shape) saw + shape square, from
     * its steps: S[n] = sum_k J_k e^{-i n th_k}, c[n] = S[n] / (i pi n), and
     * linear in the steps, so the two waveforms' steps are summed weighted.
     * Saw (+1 -> -1 ramp): +2 at every slave restart th_k = 2 pi k / r inside
     * the cycle, and at phase 0 the step 2 (r - kmax) back from where the cut
     * ramp ended. Square (+1 for `width` of each slave period, then -1, no
     * slope): +2 at each restart, -2 at 2 pi (k + width) / r inside the
     * cycle, and at phase 0 the step from the cycle's end value back to +1,
     * so the steps sum to 0. */
    int kmax = (int)ceilf(r - 1e-6f) - 1;
    if (kmax < 0) kmax = 0;
    float sq_last = 1.0f;                    /* the square's value at the cycle end */
    for (int k = 1; k <= kmax; k++) {
        add_jump(buf, H, 2.0f * WT_PI * (float)k / r, 2.0f);
        sq_last += 2.0f;
    }
    for (int k = 0; (float)k + width < r - 1e-6f; k++) {
        add_jump(buf, H, 2.0f * WT_PI * ((float)k + width) / r, -2.0f * shape);
        sq_last -= 2.0f;
    }
    float restart = (1.0f - shape) * 2.0f * (r - (float)kmax) + shape * (1.0f - sq_last);
    add_jump(buf, H, 0.0f, restart);

    /* 2. Bright: flat up to the sync harmonic, n^-k slope above it. */
    float tilt = 2.5f * (1.0f - bright / 10.0f);
    float acc = 0.0f;
    for (int n = 1; n <= H; n++) {
        float g = WT_SAW_SCALE / (WT_PI * (float)n);
        if (tilt > 0.0f && (float)n > r) g *= powf((float)n / r, -tilt);
        float sr = buf[2 * n], si = buf[2 * n + 1];
        float cr = si * g, ci = -sr * g;      /* S / (i pi n) */
        buf[2 * n]     = cr;
        buf[2 * n + 1] = ci;
        acc += cr * cr + ci * ci;
    }

    /* 3. Level by Parseval (rms^2 = 0.5 sum |c|^2), before the Peak. */
    float rms = sqrtf(0.5f * acc);
    float gain = WT_GAIN_MAX;
    if (rms >= WT_SILENT_RMS) {
        gain = WT_REF_RMS / rms;
        if (gain > WT_GAIN_MAX) gain = WT_GAIN_MAX;
    }

    /* 4. Peak: Gaussian in log2 harmonic. */
    for (int n = 1; n <= H; n++) {
        float m = gain;
        if (amount > 0.0f) {
            float l = log2f((float)n / pos);
            m *= 1.0f + amount * WT_PEAK_BOOST * expf(-WT_PEAK_K * l * l);
        }
        buf[2 * n]     *= m;
        buf[2 * n + 1] *= m;
    }

    /* 5. Bins above H stay zero (the cap); DC and Nyquist are zero. */
    irfft_inplace(buf);
}

void wt_synth_build_frames(const wt_params_t *p, uint8_t first, uint8_t count, float *frames)
{
    twiddles_init();
    const uint8_t H = wt_synth_harmonics(p->range);

    /* Effective peak positions per keyframe (the peak rule in wt_synth.h);
     * all 0 when no keyframe has a peak. */
    const uint8_t *pk = p->peak;
    float pos_k[WT_KEYS], amt_k[WT_KEYS], ratio_k[WT_KEYS];
    pos_k[0] = (float)(pk[0] ? pk[0] : (pk[1] ? pk[1] : pk[2]));
    pos_k[2] = (float)(pk[2] ? pk[2] : (pk[1] ? pk[1] : pk[0]));
    if (pk[1])               pos_k[1] = (float)pk[1];
    else if (pk[0] && pk[2]) pos_k[1] = sqrtf((float)pk[0] * (float)pk[2]);
    else                     pos_k[1] = (float)(pk[0] ? pk[0] : pk[2]);
    for (int k = 0; k < WT_KEYS; k++) {
        amt_k[k]   = pk[k] ? 1.0f : 0.0f;
        ratio_k[k] = (float)p->sync[k] / 10.0f;
    }

    unsigned end = (unsigned)first + count;
    if (end > WT_FRAMES) end = WT_FRAMES;
    for (unsigned f = first; f < end; f++) {
        /* Segment A -> M up to WT_MID_FRAME, M -> B after it. */
        int a = (f <= WT_MID_FRAME) ? 0 : 1, b = a + 1;
        float u = (f <= WT_MID_FRAME)
                ? (float)f / (float)WT_MID_FRAME
                : (float)(f - WT_MID_FRAME) / (float)(WT_FRAMES - 1 - WT_MID_FRAME);
        float shape  = ((float)p->shape[a] + ((float)p->shape[b] - (float)p->shape[a]) * u) / 100.0f;
        float width  = ((float)p->width[a] + ((float)p->width[b] - (float)p->width[a]) * u) / 100.0f;
        float bright = (float)p->bright[a] + ((float)p->bright[b] - (float)p->bright[a]) * u;
        float amount = amt_k[a] + (amt_k[b] - amt_k[a]) * u;
        /* Effective ratio: capped at H, so the sync harmonic always survives. */
        float r   = ratio_k[a] * powf(ratio_k[b] / ratio_k[a], u);
        if (r > (float)H) r = (float)H;
        float pos = (pos_k[a] > 0.0f) ? pos_k[a] * powf(pos_k[b] / pos_k[a], u) : 1.0f;
        build_frame(frames + f * WT_CYCLE, H, shape, width, bright, r, amount, pos);
    }
}

static int8_t preview_point(float v)
{
    long s = lrintf(v);
    if (s > 127) s = 127;
    if (s < -127) s = -127;
    return (int8_t)s;
}

void wt_synth_finish(float *frames, int16_t *out, wt_preview_t *pv)
{
    const uint32_t total = (uint32_t)WT_FRAMES * WT_CYCLE;
    float peak = 0.0f;
    for (uint32_t i = 0; i < total; i++) {
        float a = fabsf(frames[i]);
        if (a > peak) peak = a;
    }
    float scale = (peak > WT_FULL_SCALE) ? WT_FULL_SCALE / peak : 1.0f;

    /* Previews before the conversion overwrites frame 0. */
    float ps = scale * 127.0f / WT_FULL_SCALE;
    static const uint8_t key_frame[WT_KEYS] = { 0, WT_MID_FRAME, WT_FRAMES - 1 };
    for (int k = 0; k < WT_KEYS; k++) {
        const float *src = frames + key_frame[k] * WT_CYCLE;
        for (int i = 0; i < WT_PREVIEW_POINTS; i++) {
            pv->frame[k][i] = preview_point(src[i * (WT_CYCLE / WT_PREVIEW_POINTS)] * ps);
        }
    }

    /* Forward and through byte copies, so `out` may alias `frames`: the
     * int16 store at byte 2i only reaches floats already read. */
    uint8_t *src = (uint8_t *)frames;
    uint8_t *dst = (uint8_t *)out;
    for (uint32_t i = 0; i < total; i++) {
        float v;
        memcpy(&v, src + 4u * i, sizeof v);
        long s = lrintf(v * scale);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        int16_t o = (int16_t)s;
        memcpy(dst + 2u * i, &o, sizeof o);
    }
}
