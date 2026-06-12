#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sequencer dimensions ── */
#define SEQ_TRACKS    4
#define SEQ_STEPS     16          /* default steps for a new layer  */
#define SEQ_MAX_STEPS 32          /* maximum steps supported         */
#define MAX_LAYERS    4           /* compile-time layer limit        */

/* ── Layer type ── */
typedef enum {
    SEQ_LAYER_DRUM    = 0,
    SEQ_LAYER_MELODIC = 1,
} seq_layer_type_t;

/* ── Per-layer data (display + audio shared) ── */
typedef struct {
    seq_layer_type_t type;
    uint8_t  num_steps;                              /* 16 or 32               */
    uint8_t  num_tracks;                             /* = SEQ_TRACKS           */
    bool     grid[SEQ_TRACKS][SEQ_MAX_STEPS];        /* step on/off state      */
    uint8_t  step_note[SEQ_TRACKS][SEQ_MAX_STEPS];   /* per-step MIDI pitch    */
    uint8_t  track_base_note[SEQ_TRACKS];            /* current base note      */
    uint8_t  synth_id;
    uint16_t patch;
    uint32_t synth_flags;
    uint8_t  num_voices;
    uint8_t  step_page;                              /* display page 0|1 (32-step) */
} seq_layer_t;

/* ── Global sequencer display/UI state ── */
typedef struct {
    seq_layer_t layers[MAX_LAYERS];
    uint8_t     num_layers;
    uint8_t     active_layer_idx;
    uint16_t    bpm;
    uint8_t     current_pattern;
    uint8_t     current_step;       /* 0 .. (active layer num_steps - 1) */
    bool        playing;
    uint8_t     selected_track;     /* 0 .. SEQ_TRACKS-1                 */
    uint8_t     selected_step;      /* 0 .. (active layer num_steps - 1) */
    bool        edit_mode;
    bool        drum_select_mode;   /* true while note-select btn held   */
    bool        patch_select_mode;  /* true while patch-select btn held  */
} priv_u8g2_seq_state_t;

/**
 * @brief Draw one full sequencer frame from the provided state.
 */
void priv_u8g2_seq_draw_frame(u8g2_t *u8g2, const priv_u8g2_seq_state_t *state);

#ifdef __cplusplus
}
#endif
