#include "note_div.h"

static const struct { uint16_t ticks; char label[6]; } s_divs[NOTE_DIV_COUNT] = {
    [NOTE_DIV_4BAR]  = { 768, "4BAR"  },
    [NOTE_DIV_2BAR]  = { 384, "2BAR"  },
    [NOTE_DIV_1_1]   = { 192, "1/1"   },
    [NOTE_DIV_1_2]   = {  96, "1/2"   },
    [NOTE_DIV_1_4]   = {  48, "1/4"   },
    [NOTE_DIV_1_8]   = {  24, "1/8"   },
    [NOTE_DIV_1_16]  = {  12, "1/16"  },
    [NOTE_DIV_1_32]  = {   6, "1/32"  },
    [NOTE_DIV_1_2T]  = {  64, "1/2T"  },
    [NOTE_DIV_1_4T]  = {  32, "1/4T"  },
    [NOTE_DIV_1_8T]  = {  16, "1/8T"  },
    [NOTE_DIV_1_16T] = {   8, "1/16T" },
    [NOTE_DIV_1_32T] = {   4, "1/32T" },
    [NOTE_DIV_1_2D]  = { 144, "1/2D"  },
    [NOTE_DIV_1_4D]  = {  72, "1/4D"  },
    [NOTE_DIV_1_8D]  = {  36, "1/8D"  },
    [NOTE_DIV_1_16D] = {  18, "1/16D" },
    [NOTE_DIV_1_32D] = {   9, "1/32D" },
    [NOTE_DIV_8BAR]  = {1536, "8BAR"  },
    [NOTE_DIV_16BAR] = {3072, "16BAR" },
};

/* The LFO rate picker's scroll order: the long rates first, then enum order. */
static const uint8_t s_lfo_order[NOTE_DIV_COUNT] = {
    NOTE_DIV_16BAR, NOTE_DIV_8BAR,
    NOTE_DIV_4BAR, NOTE_DIV_2BAR, NOTE_DIV_1_1, NOTE_DIV_1_2, NOTE_DIV_1_4,
    NOTE_DIV_1_8, NOTE_DIV_1_16, NOTE_DIV_1_32,
    NOTE_DIV_1_2T, NOTE_DIV_1_4T, NOTE_DIV_1_8T, NOTE_DIV_1_16T, NOTE_DIV_1_32T,
    NOTE_DIV_1_2D, NOTE_DIV_1_4D, NOTE_DIV_1_8D, NOTE_DIV_1_16D, NOTE_DIV_1_32D,
};

uint16_t note_div_ticks(note_div_t d)
{
    return ((unsigned)d < NOTE_DIV_COUNT) ? s_divs[d].ticks : s_divs[NOTE_DIV_1_4].ticks;
}

const char *note_div_label(note_div_t d)
{
    return ((unsigned)d < NOTE_DIV_COUNT) ? s_divs[d].label : "?";
}

note_div_t note_div_lfo_step(note_div_t d, int dir)
{
    if ((unsigned)d >= NOTE_DIV_COUNT) return NOTE_DIV_1_4;
    unsigned i = 0;
    while (i < NOTE_DIV_COUNT - 1u && s_lfo_order[i] != (uint8_t)d) i++;
    if (dir > 0)      i = (i + 1u) % NOTE_DIV_COUNT;
    else if (dir < 0) i = (i + NOTE_DIV_COUNT - 1u) % NOTE_DIV_COUNT;
    return (note_div_t)s_lfo_order[i];
}
