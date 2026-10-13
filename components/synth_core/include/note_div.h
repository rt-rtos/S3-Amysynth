#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tempo-synced note divisions: the one table behind every rate picker (arp,
 * echo, sequencer/arp/live/drone LFOs, stutter drone grid). Ticks are at the
 * sequencer's 48 PPQ, so a quarter note is 48.
 *
 * Contract: every function is pure and stateless, callable from any task (and
 * from the render path); no allocation, no locks. An id >= NOTE_DIV_COUNT
 * returns the 1/4 row's ticks and the label "?". note_div_hz / note_div_ms
 * return the musical value; each consumer applies its own physical limit
 * (LFO rate cap, echo line length). Obligation: bpm > 0 for hz and ms. */

#define NOTE_DIV_PPQ 48

/* Enum order is the id the project store writes, and the scroll order of
 * every picker except the LFO rate picker (note_div_lfo_step): straight, then
 * triplets, then dotted, then the LFO-only long rates. */
typedef enum {
    NOTE_DIV_4BAR = 0,
    NOTE_DIV_2BAR,
    NOTE_DIV_1_1,
    NOTE_DIV_1_2,
    NOTE_DIV_1_4,
    NOTE_DIV_1_8,
    NOTE_DIV_1_16,
    NOTE_DIV_1_32,
    NOTE_DIV_1_2T,
    NOTE_DIV_1_4T,
    NOTE_DIV_1_8T,
    NOTE_DIV_1_16T,
    NOTE_DIV_1_32T,
    NOTE_DIV_1_2D,
    NOTE_DIV_1_4D,
    NOTE_DIV_1_8D,
    NOTE_DIV_1_16D,
    NOTE_DIV_1_32D,
    NOTE_DIV_8BAR,
    NOTE_DIV_16BAR,
    NOTE_DIV_COUNT
} note_div_t;

/* The divisions every picker offers: ids below this. The ids from here to
 * NOTE_DIV_COUNT are the LFO rate's alone. */
#define NOTE_DIV_STD_COUNT NOTE_DIV_8BAR

/* Length of one division in sequencer ticks (NOTE_DIV_PPQ per quarter). */
uint16_t note_div_ticks(note_div_t d);

/* Display label: "4BAR", "1/1", "1/8T", "1/8D" ...; never NULL. */
const char *note_div_label(note_div_t d);

/* The LFO rate picker's neighbour of d: one step through the order list
 * 16BAR, 8BAR, 4BAR, 2BAR, 1/1 ... 1/32D (dir > 0 toward 1/32D, dir < 0
 * back, dir 0 stays), wrapping at both ends. An id >= NOTE_DIV_COUNT
 * returns 1/4. */
note_div_t note_div_lfo_step(note_div_t d, int dir);

/* One cycle per division, in Hz at bpm. */
static inline float note_div_hz(note_div_t d, float bpm)
{
    return 0.8f * bpm / (float)note_div_ticks(d);
}

/* One division in ms at bpm. */
static inline float note_div_ms(note_div_t d, float bpm)
{
    return 1250.0f * (float)note_div_ticks(d) / bpm;
}

#ifdef __cplusplus
}
#endif
