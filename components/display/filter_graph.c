#include "filter_graph.h"
#include "seq_clamp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ── Layout ── */
#define FG_TOPBAR_H  16          /* rows 0..15 */
#define FG_PLOT_W   128
#define FG_PLOT_H   (64 - FG_TOPBAR_H)   /* 48 rows */
#define FG_PLOT_Y0  FG_TOPBAR_H

/* Passband is displayed at 75% of plot height, leaving 25% headroom for
 * resonance spikes (which can reach up to 100% for Q > ~1.33). */
#define FG_PASSBAND_NORM  0.75f

/* Number of log-spaced frequency samples for the response curve. */
#define FG_NPTS  24

static const char * const s_type_names[FGRAPH_FILTER_COUNT] = {
    "NONE", "LPF", "BPF", "HPF", "LPF24", "NOTCH", "PHASR"
};

/* ── Response functions (2nd-order biquad magnitude, unclipped) ── */

static float lpf2_mag(float r, float Q)
{
    float d1 = 1.0f - r * r;
    float d2 = r / Q;
    return 1.0f / sqrtf(d1 * d1 + d2 * d2 + 1e-12f);
}

static float hpf2_mag(float r, float Q)
{
    float r2 = r * r;
    float d1 = 1.0f - r2;
    float d2 = r / Q;
    return r2 / sqrtf(d1 * d1 + d2 * d2 + 1e-12f);
}

static float bpf2_mag(float r, float Q)
{
    float num = r / Q;
    float d1  = 1.0f - r * r;
    float d2  = r / Q;
    return num / sqrtf(d1 * d1 + d2 * d2 + 1e-12f);
}

static float notch2_mag(float r, float Q)
{
    float num = 1.0f - r * r;
    float d2  = r / Q;
    return fabsf(num) / sqrtf(num * num + d2 * d2 + 1e-12f);
}

/* 6-stage allpass phaser (AMY FILTER_PHASER): six identical first-order
 * allpasses H1 = (a + z^-1)/(1 + a z^-1), feedback fb around the chain with a
 * one-sample loop delay, then a 50/50 dry/wet sum. |H1| = 1, so only the chain
 * phase moves — the dry+wet sum nulls at the -180/-540/-900 degree crossings,
 * giving 3 notches with the middle one at the cutoff. Coefficient mapping
 * mirrors the engine (filters.c): a from tan(pi*fc/fs), fb linear in Q up to
 * 0.85. Unlike the biquads this depends on absolute frequency, hence f and fc
 * instead of their ratio; fs must match AMY_SAMPLE_RATE. */
static float phaser_mag(float f, float fc, float Q)
{
    const float fs = 48000.0f;
    float t  = tanf((float)M_PI * fc / fs);
    float a  = (t - 1.0f) / (t + 1.0f);
    float fb = 0.85f * (Q - FGRAPH_RES_MIN) / (FGRAPH_RES_MAX - FGRAPH_RES_MIN);
    fb = SEQ_CLAMP_F32(fb, 0.0f, 0.85f);

    float w  = 2.0f * (float)M_PI * f / fs;
    float zr = cosf(w);              /* z^-1 = e^{-jw} */
    float zi = -sinf(w);

    /* H1 = (a + z^-1) / (1 + a z^-1) */
    float nr = a + zr,        ni = zi;
    float dr = 1.0f + a * zr, di = a * zi;
    float dm = dr * dr + di * di + 1e-12f;
    float h1r = (nr * dr + ni * di) / dm;
    float h1i = (ni * dr - nr * di) / dm;

    /* Hc = H1^6 */
    float hr = h1r, hi = h1i;
    for (int k = 1; k < 6; k++) {
        float tr = hr * h1r - hi * h1i;
        hi = hr * h1i + hi * h1r;
        hr = tr;
    }

    /* Hw = Hc / (1 - fb * Hc * z^-1) */
    float lr = hr * zr - hi * zi;
    float li = hr * zi + hi * zr;
    dr = 1.0f - fb * lr;
    di = -fb * li;
    dm = dr * dr + di * di + 1e-12f;
    float wr = (hr * dr + hi * di) / dm;
    float wi = (hi * dr - hr * di) / dm;

    /* out = 0.5 * (dry + wet) */
    float sr = 0.5f * (1.0f + wr);
    float si = 0.5f * wi;
    return sqrtf(sr * sr + si * si);
}

/* Display magnitude (0..1) at frequency f for cutoff fc, Q and filter type.
 * Passband = FG_PASSBAND_NORM; a resonance spike may exceed it, capped at 1. */
static float filter_display_mag(uint8_t type, float f, float fc, float Q)
{
    if (fc < 1.0f) fc = 1.0f;
    float r = f / fc;
    if (r < 1e-4f) r = 1e-4f;
    float mag;
    switch (type) {
        case FGRAPH_FILTER_LPF: {
            float m = lpf2_mag(r, Q);
            mag = m * FG_PASSBAND_NORM;
            break;
        }
        case FGRAPH_FILTER_LPF24: {
            float m = lpf2_mag(r, Q);
            mag = m * m * FG_PASSBAND_NORM;   /* two cascaded biquads */
            break;
        }
        case FGRAPH_FILTER_HPF: {
            float m = hpf2_mag(r, Q);
            mag = m * FG_PASSBAND_NORM;
            break;
        }
        case FGRAPH_FILTER_BPF: {
            float m = bpf2_mag(r, Q);
            /* BPF unity gain at center = 1.0; scale to passband level. */
            mag = m * FG_PASSBAND_NORM;
            break;
        }
        case FGRAPH_FILTER_NOTCH: {
            float m = notch2_mag(r, Q);
            mag = m * FG_PASSBAND_NORM;
            break;
        }
        case FGRAPH_FILTER_PHASER: {
            float m = phaser_mag(f, fc, Q);
            mag = m * FG_PASSBAND_NORM;
            break;
        }
        default:   /* FGRAPH_FILTER_NONE */
            mag = FG_PASSBAND_NORM;
            break;
    }
    return SEQ_CLAMP_F32(mag, 0.0f, 1.0f);
}

/* Convert normalised cutoff 0..1 → Hz. */
static float norm_to_hz(float norm)
{
    return FGRAPH_CUTOFF_HZ_MIN *
           powf(FGRAPH_CUTOFF_HZ_MAX / FGRAPH_CUTOFF_HZ_MIN, norm);
}

/* Convert normalised resonance 0..1 → Q. */
static float norm_to_q(float norm)
{
    return FGRAPH_RES_MIN + norm * (FGRAPH_RES_MAX - FGRAPH_RES_MIN);
}

/* One right-aligned KS readout box at row fy: blanked background, outline
 * frame when selected, inverted fill while adjusting. */
static void draw_ks_box(u8g2_t *u8g2, const char *buf, uint8_t fy, bool sel,
                        bool editing)
{
    uint8_t fw = (uint8_t)u8g2_GetStrWidth(u8g2, buf);
    uint8_t fx = (uint8_t)(126 - fw);
    const uint8_t fh = 11;

    u8g2_SetDrawColor(u8g2, 0);
    u8g2_DrawBox(u8g2, (uint8_t)(fx - 2), fy, (uint8_t)(fw + 4), fh);
    u8g2_SetDrawColor(u8g2, 1);
    if (sel && editing) {
        u8g2_DrawBox(u8g2, (uint8_t)(fx - 2), fy, (uint8_t)(fw + 4), fh);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, fx, (uint8_t)(fy + 8), buf);
        u8g2_SetDrawColor(u8g2, 1);
    } else {
        if (sel) u8g2_DrawRFrame(u8g2, (uint8_t)(fx - 2), fy, (uint8_t)(fw + 4), fh, 1);
        u8g2_DrawStr(u8g2, fx, (uint8_t)(fy + 8), buf);
    }
}

/* KS string-feedback and pluck-duty readouts, hanging under the Q readout
 * below the header divider, DT directly under FB. Only on feedback waves, so
 * the header layout is unchanged for other targets. Blanks the plot behind
 * them, so call AFTER the curve (or OFF line). Both shape the KS string, not
 * the biquad, so they are drawn even while the filter is OFF. FB shows two
 * decimals from 99% up, where one detent is a fraction of a percent. */
static void draw_feedback_box(u8g2_t *u8g2, const filter_graph_t *fg)
{
    if (!fg->has_feedback) return;

    char buf[12];
    float fb = fg->feedback_norm;
    if (fb >= 1.0f) {
        snprintf(buf, sizeof(buf), "FB:100%%");
    } else if (fb >= 0.99f) {
        snprintf(buf, sizeof(buf), "FB:%.2f%%", (double)(fb * 100.0f));
    } else {
        snprintf(buf, sizeof(buf), "FB:%.1f%%", (double)(fb * 100.0f));
    }
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
    const uint8_t fy = FG_TOPBAR_H;   /* first row under the divider */
    draw_ks_box(u8g2, buf, fy, fg->cursor == 2, fg->editing);

    snprintf(buf, sizeof(buf), "DT:%.2f", (double)fg->ks_duty);
    draw_ks_box(u8g2, buf, (uint8_t)(fy + 11), fg->cursor == 3, fg->editing);
}

void filter_graph_draw(u8g2_t *u8g2, const filter_graph_t *fg)
{
    if (!u8g2 || !fg) return;

    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);

    /* ── Top bar ── */
    const char *type_name = (fg->filter_type < FGRAPH_FILTER_COUNT)
                            ? s_type_names[fg->filter_type] : "?";

    /* Left: voice label (e.g. "L1 T2" or "ARP"). */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(u8g2, 2, 8, fg->label);

    /* Right: value readout for the selected cursor parameter. */
    {
        char val_buf[12];
        float hz = norm_to_hz(fg->cutoff_norm);
        float q  = norm_to_q(fg->resonance_norm);
        if (fg->cursor == 1) {
            snprintf(val_buf, sizeof(val_buf), "Q:%.1f", (double)q);
        } else {
            if (hz >= 1000.0f) {
                snprintf(val_buf, sizeof(val_buf), "%.1fk", (double)(hz / 1000.0f));
            } else {
                snprintf(val_buf, sizeof(val_buf), "%uHz", (unsigned)hz);
            }
        }
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        uint8_t vw = (uint8_t)u8g2_GetStrWidth(u8g2, val_buf);
        uint8_t vx = (uint8_t)(126 - vw);
        /* Matches the type/EN fields: outline frame when the cutoff (0) or
         * resonance (1) cursor is selected, inverted fill while adjusting. */
        if (fg->cursor == 0 || fg->cursor == 1) {
            if (fg->editing) {
                u8g2_DrawBox(u8g2, (uint8_t)(vx - 2), 0, (uint8_t)(vw + 4), 11);
                u8g2_SetDrawColor(u8g2, 0);
                u8g2_DrawStr(u8g2, vx, 8, val_buf);
                u8g2_SetDrawColor(u8g2, 1);
            } else {
                u8g2_DrawRFrame(u8g2, (uint8_t)(vx - 2), 0, (uint8_t)(vw + 4), 11, 1);
                u8g2_DrawStr(u8g2, vx, 8, val_buf);
            }
        } else {
            u8g2_DrawStr(u8g2, vx, 8, val_buf);
        }
    }

    /* Centre: filter type name plus enable checkbox as one centred group, so
     * both stay visible regardless of the cursor. The checkbox is only for
     * toggle-capable targets (melodic/arp); the drone is a fixed LPF24. */
    {
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, type_name);

        const uint8_t CB_W = 7;    /* enable checkbox outer width */
        const uint8_t CB_GAP = 4;  /* gap between type name and checkbox */
        uint8_t group_w = fg->show_toggles ? (uint8_t)(tw + CB_GAP + CB_W) : tw;
        uint8_t tx = (uint8_t)((128 - group_w) / 2);

        /* Type name - cursor 4 highlight only where the type is selectable. */
        if (fg->show_toggles && fg->cursor == 4) {
            if (fg->editing) {
                /* Inverted box = value is live. */
                u8g2_DrawBox(u8g2, (uint8_t)(tx - 2), 0, (uint8_t)(tw + 4), 11);
                u8g2_SetDrawColor(u8g2, 0);
                u8g2_DrawStr(u8g2, tx, 8, type_name);
                u8g2_SetDrawColor(u8g2, 1);
            } else {
                u8g2_DrawRFrame(u8g2, (uint8_t)(tx - 2), 0, (uint8_t)(tw + 4), 11, 1);
                u8g2_DrawStr(u8g2, tx, 8, type_name);
            }
        } else {
            u8g2_DrawStr(u8g2, tx, 8, type_name);
        }

        /* Enable checkbox - filled = on; cursor 5 selects it (outline
         * highlight, inverted while toggling). */
        if (fg->show_toggles) {
            uint8_t bx = (uint8_t)(tx + tw + CB_GAP);
            const uint8_t by = 2, bh = 7;   /* checkbox rows 2..8, aligned to text */
            bool sel = (fg->cursor == 5);

            if (sel && fg->editing) {
                u8g2_DrawBox(u8g2, (uint8_t)(bx - 2), 0, (uint8_t)(CB_W + 4), 11);
                u8g2_SetDrawColor(u8g2, 0);
                u8g2_DrawFrame(u8g2, bx, by, CB_W, bh);
                if (fg->enabled) u8g2_DrawBox(u8g2, (uint8_t)(bx + 2), (uint8_t)(by + 2), 3, 3);
                u8g2_SetDrawColor(u8g2, 1);
            } else {
                if (sel) u8g2_DrawRFrame(u8g2, (uint8_t)(bx - 2), 0, (uint8_t)(CB_W + 4), 11, 1);
                u8g2_DrawFrame(u8g2, bx, by, CB_W, bh);
                if (fg->enabled) u8g2_DrawBox(u8g2, (uint8_t)(bx + 2), (uint8_t)(by + 2), 3, 3);
            }
        }
    }

    /* Divider. */
    u8g2_DrawHLine(u8g2, 0, FG_TOPBAR_H - 1, 128);

    /* ── Disabled state: flat line + "OFF" ── */
    if (!fg->enabled) {
        uint8_t y_mid = (uint8_t)(FG_PLOT_Y0 + FG_PLOT_H / 2);
        u8g2_DrawHLine(u8g2, 0, y_mid, FG_PLOT_W);
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        uint8_t ow = (uint8_t)u8g2_GetStrWidth(u8g2, "OFF");
        u8g2_DrawStr(u8g2, (uint8_t)((FG_PLOT_W - ow) / 2), (uint8_t)(y_mid - 2), "OFF");
        draw_feedback_box(u8g2, fg);
        return;
    }

    /* ── Compute response curve ── */
    float fc = norm_to_hz(fg->cutoff_norm);
    float Q  = norm_to_q(fg->resonance_norm);
    const uint8_t baseline_y = (uint8_t)(FG_PLOT_Y0 + FG_PLOT_H - 1);

    /* Sample frequencies depend only on the compile-time Hz range, so compute
     * once and reuse: this redraws continuously while the encoder turns. */
    static float s_curve_hz[FG_NPTS];
    static bool  s_curve_hz_init = false;
    if (!s_curve_hz_init) {
        for (int i = 0; i < FG_NPTS; i++) {
            float t = (float)i / (float)(FG_NPTS - 1);
            s_curve_hz[i] = FGRAPH_CUTOFF_HZ_MIN *
                            powf(FGRAPH_CUTOFF_HZ_MAX / FGRAPH_CUTOFF_HZ_MIN, t);
        }
        s_curve_hz_init = true;
    }
    uint8_t px[FG_NPTS];
    uint8_t py[FG_NPTS];
    for (int i = 0; i < FG_NPTS; i++) {
        float t = (float)i / (float)(FG_NPTS - 1);
        float f = s_curve_hz[i];
        float mag = filter_display_mag(fg->filter_type, f, fc, Q);
        px[i] = (uint8_t)(t * (float)(FG_PLOT_W - 1));
        /* Y: top of plot = high magnitude; bottom = silence. */
        uint8_t amp_px = (uint8_t)(mag * (float)(FG_PLOT_H - 2) + 0.5f);
        py[i] = (uint8_t)(FG_PLOT_Y0 + (FG_PLOT_H - 2) - amp_px);
    }

    /* Fill area under the curve (vertical lines from curve to baseline). */
    int seg = 0;
    for (uint8_t x = 0; x < FG_PLOT_W; x++) {
        while (seg < FG_NPTS - 2 && px[seg + 1] <= x) seg++;
        int y;
        int dX = (int)px[seg + 1] - (int)px[seg];
        if (dX < 1) {
            y = py[seg];
        } else {
            y = (int)py[seg] + ((int)py[seg + 1] - (int)py[seg]) * (int)(x - px[seg]) / dX;
        }
        if (y < FG_PLOT_Y0)    y = FG_PLOT_Y0;
        if (y > (int)baseline_y) y = baseline_y;
        u8g2_DrawVLine(u8g2, x, (uint8_t)y, (uint8_t)(baseline_y - y + 1));
    }

    /* ── Cutoff cursor: XOR the column at cutoff_norm position ── */
    uint8_t cx = (uint8_t)(fg->cutoff_norm * (float)(FG_PLOT_W - 1));
    u8g2_SetDrawColor(u8g2, 2);   /* XOR: inverts whatever is there */
    u8g2_DrawVLine(u8g2, cx, FG_PLOT_Y0, FG_PLOT_H - 1);
    u8g2_SetDrawColor(u8g2, 1);

    /* Last so its blanked background wins over the curve and cutoff cursor. */
    draw_feedback_box(u8g2, fg);
}
