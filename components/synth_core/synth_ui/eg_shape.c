#include "synth_ui/eg_shape.h"
#include <math.h>

#define EG_SHAPE_EPS 0.0002f   /* BREAKPOINT_EPS: floor for the log-domain types */

float eg_shape_eval(uint8_t eg_type, float v0, float v1, float t)
{
    eg_type = (uint8_t)(eg_type & 3u);

    if (eg_type == 1) {         /* ENVELOPE_LINEAR */
        return v0 + (v1 - v0) * t;
    }

    if (eg_type == 2 || eg_type == 3) {   /* DX7 / TRUE_EXPONENTIAL */
        float a = (v0 > EG_SHAPE_EPS) ? v0 : EG_SHAPE_EPS;
        float b = (v1 > EG_SHAPE_EPS) ? v1 : EG_SHAPE_EPS;
        if (eg_type == 2 && b > a) {
            /* DX7 attack law: levels map linear->DX7 (log2 + 12.375) then
             * through the attack-range curve; time is normalised so only the
             * ratio matters. Degenerate spans fall through to true-exp. */
            float l0 = log2f(a) + 12.375f;
            float l1 = log2f(b) + 12.375f;
            float m0 = 1.0f - ((l0 > 4.25f) ? (l0 - 4.25f) : 0.0f) / 9.375f;
            float m1 = 1.0f - ((l1 > 4.25f) ? (l1 - 4.25f) : 0.0f) / 9.375f;
            float dl = log2f(m0) - log2f(m1);
            if (dl > 1e-6f) {
                float t_const = 1.0f / dl;
                float my_t0   = -t_const * log2f(m0);
                float level   = 4.25f
                    + 9.375f * (1.0f - exp2f(-(my_t0 + t) / t_const));
                return exp2f(level - 12.375f);
            }
        }
        /* TRUE_EXPONENTIAL, and DX7 decay/release (also plain true-exp). */
        float la = log2f(a), lb = log2f(b);
        return exp2f(la + (lb - la) * t);
    }

    /* ENVELOPE_NORMAL: overshoot-compensated "false exponential". */
    const float rate      = -4.328085f;   /* EXP_RATE_VAL */
    const float overshoot = 1.0f / (1.0f - exp2f(rate));
    float y = v0 + (v1 - v0) * overshoot * (1.0f - exp2f(rate * t));
    if (y < 0.0f) y = 0.0f;
    return y;
}
