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
};

uint16_t note_div_ticks(note_div_t d)
{
    return ((unsigned)d < NOTE_DIV_COUNT) ? s_divs[d].ticks : s_divs[NOTE_DIV_1_4].ticks;
}

const char *note_div_label(note_div_t d)
{
    return ((unsigned)d < NOTE_DIV_COUNT) ? s_divs[d].label : "?";
}
