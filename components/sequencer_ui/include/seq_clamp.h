#pragma once

#include <stdint.h>

/* Type-specific helpers avoid macro double-evaluation pitfalls. */
/* Using 'int' for the input 'value' in unsigned helpers allows negative results 
   from (current + delta) to be safely evaluated before returning the clamped type. */
static inline int seq_clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static inline uint8_t seq_clamp_u8(int value, uint8_t min_value, uint8_t max_value)
{
    if (value < (int)min_value) return min_value;
    if (value > (int)max_value) return max_value;
    return (uint8_t)value;
}

static inline uint16_t seq_clamp_u16(int value, uint16_t min_value, uint16_t max_value)
{
    if (value < (int)min_value) return min_value;
    if (value > (int)max_value) return max_value;
    return (uint16_t)value;
}

static inline float seq_clamp_f32(float value, float min_value, float max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

#ifndef SEQ_CLAMP_INT
#define SEQ_CLAMP_INT(value, min_value, max_value) \
    seq_clamp_int((value), (min_value), (max_value))
#endif

#ifndef SEQ_CLAMP_U8
#define SEQ_CLAMP_U8(value, min_value, max_value) \
    seq_clamp_u8((value), (min_value), (max_value))
#endif

#ifndef SEQ_CLAMP_U16
#define SEQ_CLAMP_U16(value, min_value, max_value) \
    seq_clamp_u16((value), (min_value), (max_value))
#endif

#ifndef SEQ_CLAMP_F32
#define SEQ_CLAMP_F32(value, min_value, max_value) \
    seq_clamp_f32((value), (min_value), (max_value))
#endif