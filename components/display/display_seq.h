#pragma once

#include "u8g2.h"
#include "seq_model.h"      /* engine-owned data model: seq_layer_t, seq_env_t, SEQ_* */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Top-level screen the UI is showing ── */
typedef enum {
    UI_MODE_SEQUENCER = 0,
    UI_MODE_ARP       = 1,
    UI_MODE_DRONE     = 2,   /* stutter drone screen */
    UI_MODE_PROG      = 3,
    /* 4 was UI_MODE_TRACKOPTS; the screen is gone, the numbering is not
     * reused so the remaining values stay where they were. */
    UI_MODE_FM        = 5,
    UI_MODE_DRONE_STD = 6,   /* normal (free-running) drone screen */
    UI_MODE_DEV       = 7,   /* DEV menu (CONFIG_SYNTH_DEV_MENU) */
} ui_mode_t;

/* algo_banner_value sentinel: Shift+Turn landed on a patch with no FM
 * algorithm; the banner says so instead of showing a number. */
#define DISPLAY_ALGO_BANNER_NOFM   0xFF
/* The FM_CUSTOM voice is on its authored (custom) topology. */
#define DISPLAY_ALGO_BANNER_CUSTOM 0xFE

/* ── Global sequencer display/UI state ── */
typedef struct {
    seq_layer_t layers[MAX_LAYERS];
    uint8_t     num_layers;
    uint8_t     active_layer_idx;
    uint8_t     current_pattern;
    uint8_t     current_step;       /* 0 .. (active layer num_steps - 1) */
    bool        playing;
    uint8_t     selected_track;     /* 0 .. SEQ_TRACKS-1                 */
    uint8_t     selected_step;      /* 0 .. (active layer num_steps - 1) */
    bool        edit_mode;
    bool        drum_select_mode;   /* true while note-select btn held   */
    bool        patch_select_mode;  /* true while patch-select btn held  */
    bool        drum_pcm;           /* true = drum engine is PCM: drum row
                                       labels/banner show PCM presets, not
                                       patches. Mirror of the core engine,
                                       refreshed by seq_view_signature().  */
    uint8_t     algo_banner_ticks;  /* SEQ screen "ALGO n" banner: UI frames
                                       left to show it (Shift+Turn feedback,
                                       decayed by synth_ui_task); 0 = hidden */
    uint8_t     algo_banner_value;  /* algorithm index shown by the banner;
                                       DISPLAY_ALGO_BANNER_NOFM = the active
                                       patch has no FM algorithm            */

    /* ── Screen + menu overlay ── */
    ui_mode_t   ui_mode;            /* which top-level screen is active   */
    bool        menu_open;          /* true while the menu overlay is up  */
    uint8_t     menu_cursor;        /* highlighted menu item index        */
    bool        menu_editing;       /* true while editing the entered item*/
} display_seq_state_t;

/**
 * @brief Draw one full sequencer frame from the provided state.
 * @param bpm  Current BPM value, passed explicitly (not stored in state).
 */
void display_seq_draw_frame(u8g2_t *u8g2, const display_seq_state_t *state, uint16_t bpm);

#ifdef __cplusplus
}
#endif
