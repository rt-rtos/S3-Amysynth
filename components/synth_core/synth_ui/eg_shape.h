#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Envelope segment shape for UI plots ──────────────────────────────────
 * A float mirror of AMY's compute_breakpoint_scale() (envelope.c): the value
 * of one breakpoint segment from v0 to v1 at normalised time t (0..1), with
 * levels as linear amplitude 0..1, so a drawn curve matches what eg_type
 * sounds like. eg_type is AMY's ENVELOPE_* number: 0 Normal, 1 Linear, 2 DX7,
 * 3 TrueExp (only the low two bits are read). Keep in sync with envelope.c.
 * Pure float math, callable from any task; not for the render path. */
float eg_shape_eval(uint8_t eg_type, float v0, float v1, float t);

#ifdef __cplusplus
}
#endif
