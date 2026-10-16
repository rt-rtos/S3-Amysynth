#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "seq_model.h"     /* voice_params_t, voice_unison_t */
#include "note_div.h"
#include "arp_core.h"      /* arp_quant_mode_t: the drop snaps as the arp does */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Ball drop engine ────────────────────────────────────────────────────
 * An arp-shaped engine whose pattern comes from a ball game: balls drop from
 * a spawner onto a field of line segments; every impact is one note on the
 * DROP synth (pitch from the segment's row, velocity from the impact speed,
 * gate from the air time to the ball's next impact), and a ball crossing the
 * jackpot cells fires the SUB synth at its root. The loop is simulated once
 * from a seed on the UI task and handed to AMY as repeating tagged events,
 * exactly as the arp's slots are; nothing here runs on the render task.
 *
 * Field: DROP_COLS x DROP_ROWS cells over the OLED's blue band, DROP_CELL_W x
 * DROP_CELL_H pixels each, the header in the yellow band. Row DROP_ROWS-1 (the
 * bottom) plays the root; each row up is `octaves` scale degrees higher.
 * The sim runs in integer fixed point (DROP_Q fractional bits), so the same
 * seed gives the same loop on the host and the target.
 *
 * Execution context: UI task only, through the amy_helpers ingress FIFO.
 * Setters mark the loop dirty; drop_core_service() re-simulates and re-emits
 * once per UI frame. */

#define DROP_COLS       16
#define DROP_ROWS       8
#define DROP_CELL_W     8
#define DROP_CELL_H     6
#define DROP_FIELD_W    (DROP_COLS * DROP_CELL_W)   /* 128 px */
#define DROP_FIELD_H    (DROP_ROWS * DROP_CELL_H)   /* 48 px: the blue band */

#define DROP_SEG_MAX    16
#define DROP_BALL_MAX   8
#define DROP_SEG_ANGLES 8      /* 22.5 degree steps, 0 = horizontal */
#define DROP_OCT_MAX    4
#define DROP_LOOP_BARS_MAX 8
#define DROP_SPAWN_COL  8      /* the spawner's column */
#define DROP_PORTAMENTO_MAX_MS 100u   /* as ARP_PORTAMENTO_MAX_MS */

typedef enum {
    DROP_SEG_PLAIN = 0,   /* reflect, restitution DROP_RESTITUTION */
    DROP_SEG_SPLIT = 1,   /* reflect left or right by `ratio` from the PRNG */
    DROP_SEG_DAMP  = 2,   /* reflect at a fifth of the speed: ends a ball */
    DROP_SEG_TYPE_COUNT
} drop_seg_type_t;

typedef struct {
    uint8_t col;      /* 0..DROP_COLS-1 */
    uint8_t row;      /* 0..DROP_ROWS-1, 0 = top */
    uint8_t angle;    /* 0..DROP_SEG_ANGLES-1 */
    uint8_t type;     /* drop_seg_type_t */
    uint8_t ratio;    /* SPLIT: percent of balls sent left, 0..100 */
} drop_seg_t;

typedef struct {
    uint8_t col0;     /* left end, 0..DROP_COLS-1 */
    uint8_t col1;     /* right end, col0..DROP_COLS-1 */
    uint8_t row;
    bool    enabled;
} drop_jackpot_t;

/* The two synths: the impact voice and the jackpot (sub) voice. */
typedef enum { DROP_VOICE_MAIN = 0, DROP_VOICE_SUB = 1, DROP_VOICE_COUNT } drop_which_t;

/* ── Lifecycle ── */
void drop_core_init(void);

/* Re-simulate the loop and re-emit it now; setters mark dirty instead and
 * let drop_core_service() coalesce. */
void drop_core_refresh(void);

/* Cancel every scheduled drop event without re-emitting (transport stop),
 * leaving the configuration alone. Does NOT silence a note mid-gate: follow
 * with sequencer_kill_synth_voices() on both slots, as the transport does. */
void drop_core_clear_all(void);

/* Drain a pending coalesced refresh; cheap no-op otherwise. Once per UI
 * frame. */
void drop_core_service(void);

/* Request a coalesced refresh (the transport on resume, OOM resync). */
void drop_core_mark_dirty(void);

/* ── Loop parameters (each marks dirty) ── */
void       drop_set_enabled(bool enabled);
bool       drop_get_enabled(void);
/* User mute, apart from enabled; muting clears and kills at once. Persisted. */
void       drop_set_muted(bool muted);
bool       drop_get_muted(void);
/* Solo duck from the sequencer's solo hook; not persisted. */
void       drop_set_solo_muted(bool muted);
void       drop_set_rate(note_div_t rate);        /* ids >= NOTE_DIV_STD_COUNT ignored */
note_div_t drop_get_rate(void);
void       drop_set_loop_bars(uint8_t bars);      /* clamped 1..DROP_LOOP_BARS_MAX */
uint8_t    drop_get_loop_bars(void);
void       drop_set_octaves(uint8_t octaves);     /* clamped 1..DROP_OCT_MAX */
uint8_t    drop_get_octaves(void);
void       drop_set_gate_pct(uint8_t gate_pct);   /* clamped 10..100 */
uint8_t    drop_get_gate_pct(void);
/* The seed the loop is simulated from. reroll draws a new one unless the
 * seed is frozen; set takes any value (a loaded project). */
void       drop_set_seed(uint32_t seed);
uint32_t   drop_get_seed(void);
void       drop_reroll_seed(void);
void       drop_set_seed_frozen(bool frozen);
bool       drop_get_seed_frozen(void);
void       drop_set_scale(uint8_t scale_index);
uint8_t    drop_get_scale(void);
void       drop_set_root_note(uint8_t root_note);     /* DROP synth, bottom row */
uint8_t    drop_get_root_note(void);
void       drop_set_sub_root_note(uint8_t root_note); /* SUB synth */
uint8_t    drop_get_sub_root_note(void);
/* What the rows snap to, with the arp's three sources (arp_set_quant_mode). */
void       drop_set_quant_mode(arp_quant_mode_t mode);
arp_quant_mode_t drop_get_quant_mode(void);

/* ── Field (each marks dirty) ── */
uint8_t drop_get_seg_count(void);
bool    drop_get_seg(uint8_t idx, drop_seg_t *out);
/* Index of the segment on a cell, or -1. */
int     drop_find_seg(uint8_t col, uint8_t row);
/* Place `seg` on its cell, replacing the one there; false when the field is
 * full or a field is out of range. Angle, type and ratio are clamped. */
bool    drop_set_seg(const drop_seg_t *seg);
bool    drop_remove_seg(uint8_t col, uint8_t row);
void    drop_get_jackpot(drop_jackpot_t *out);
void    drop_set_jackpot(const drop_jackpot_t *jp);   /* clamped, col1 >= col0 */

/* ── Per-synth voice (DROP_VOICE_MAIN / DROP_VOICE_SUB) ── */
void     drop_set_patch(drop_which_t w, uint16_t patch);   /* rebuilds the slot */
uint16_t drop_get_patch(drop_which_t w);
void     drop_set_portamento_ms(drop_which_t w, uint16_t ms);
uint16_t drop_get_portamento_ms(drop_which_t w);
void     drop_set_amp_scale(drop_which_t w, float v);     /* 0..1, marks dirty */
float    drop_get_amp_scale(drop_which_t w);
void     drop_get_vp(drop_which_t w, voice_params_t *out);
void     drop_get_unison(drop_which_t w, voice_unison_t *out);
uint8_t  drop_unison_max(drop_which_t w);
/* The whole voice at once (a loaded project): stores patch, glide, voice
 * block and unison, clamps, and rebuilds the slot once. */
void     drop_set_voice(drop_which_t w, uint16_t patch, uint16_t portamento_ms,
                        const voice_params_t *vp, const voice_unison_t *uni);

/* Screen focus: which synth the patch gesture and the status row address.
 * Session state, not persisted. */
drop_which_t drop_get_focus(void);
void         drop_set_focus(drop_which_t w);

/* ── View ── */
typedef struct { uint8_t x, y; } drop_ball_pos_t;   /* field pixels */

/* Ball positions at `loop_tick` (0..drop_loop_ticks()-1) of the current
 * loop: the deterministic sim replayed from the seed up to that tick, kept
 * incrementally between calls. Returns the count written. UI task only. */
uint8_t  drop_view_balls(uint32_t loop_tick, drop_ball_pos_t out[DROP_BALL_MAX]);
/* The loop length in sequencer ticks (loop_bars x SEQ_TICKS_PER_BAR). */
uint32_t drop_loop_ticks(void);
/* Impacts and jackpot crossings the last refresh emitted (0 while the loop
 * is not playing). */
uint8_t  drop_last_impact_count(void);
uint8_t  drop_last_jackpot_count(void);

/* Print the loop state and the last emitted loop (one line per impact and
 * crossing: tick, note, velocity, gate) to the console, for the DEV
 * sequencer dump; returns the lines printed. UI task only. */
unsigned drop_core_dump(void);

/* Segment direction for drawing: unit vector of `angle` in DROP_Q fixed
 * point (x right, y down). */
#define DROP_Q 12
void     drop_seg_dir(uint8_t angle, int32_t *dx, int32_t *dy);

#ifdef __cplusplus
}
#endif
