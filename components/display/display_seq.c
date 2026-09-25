#include "display_seq.h"
#include "patch_names.h"
#include "seq_chords.h"   /* chord sentinel test for the track pitch label */
#include <stdio.h>

/* Static role labels for the drum tracks (low body, mid attack, high tick,
 * percussive accent, and the fifth row's auxiliary voice); the loaded preset
 * is named in the patch overlay. */
static const char *const drum_track_labels[] = {
    "LOW", "MID", "TOP", "PERC", "AUX"
};
_Static_assert(sizeof(drum_track_labels) / sizeof(drum_track_labels[0]) == SEQ_TRACKS,
               "one drum label per row");

/* Grid geometry for a layer's row and step counts. The single place the grid
 * pitch lives: every row, separator and playhead coordinate derives from it. */
typedef struct {
    int grid_x;     /* left edge of step column 0                   */
    int col_w;      /* step pitch                                   */
    int cell_w;     /* drawn cell width                             */
    int cell_size;  /* drawn cell height                            */
    int row_h;      /* row pitch                                    */
    int grid_top;   /* top of row 0's cells                         */
    int span_top;   /* top of the beat separators and the playhead  */
    int span_h;     /* their height                                 */
} seq_grid_geom_t;

static seq_grid_geom_t seq_grid_geom(uint8_t num_tracks, uint8_t num_steps)
{
    seq_grid_geom_t g;
    /* The whole loop in one row: 16 steps at a 6 px pitch, 32 at 3 px. Both
     * end at x=122; cells keep their 5 px height. A frame needs 3 px of width
     * to have an unlit interior, so at 2 px an empty cell is drawn as a
     * centered dash instead. */
    g.grid_x    = 26;
    g.col_w     = (num_steps == SEQ_MAX_STEPS) ? 3 : 6;
    g.cell_w    = g.col_w - 1;
    g.cell_size = 5;
    /* Five rows close up to an 8 px pitch and start higher, so the last row
     * clears the hint strip. */
    if (num_tracks > SEQ_TRACKS_DEFAULT) {
        g.row_h    = 8;
        g.grid_top = 16;
    } else {
        g.row_h    = 10;
        g.grid_top = 20;
    }
    g.span_top = g.grid_top - 3;
    g.span_h   = num_tracks * g.row_h + 2;
    return g;
}

/* 3-char note name ("C4", "C#4", ...), or the chord-slot label "CHn" when the
 * stored note is a chord preset sentinel (seq_chords.h). */
static void note_name_str(uint8_t midi_note, char buf[4])
{
    if (SEQ_NOTE_IS_CHORD(midi_note)) {
        snprintf(buf, 4, "CH%u", (unsigned)(SEQ_CHORD_INDEX(midi_note) + 1u));
        return;
    }
    static const char *const note_names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    int octave = (int)(midi_note / 12) - 1;
    snprintf(buf, 4, "%s%d", note_names[midi_note % 12], octave);
}

void display_seq_draw_frame(u8g2_t *u8g2, const display_seq_state_t *state, uint16_t bpm)
{
    /* Nothing to draw until at least one layer exists. */
    if (state->num_layers == 0) {
        u8g2_ClearBuffer(u8g2);
        return;
    }

    const seq_layer_t *layer      = &state->layers[state->active_layer_idx];
    const uint8_t      num_steps  = layer->num_steps;
    const uint8_t      num_tracks = (layer->num_tracks > SEQ_TRACKS)
                                    ? SEQ_TRACKS : layer->num_tracks;

    u8g2_ClearBuffer(u8g2);

    /* === HEADER === */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    char buf[16];

    snprintf(buf, sizeof(buf), "BPM %3d", (int)bpm);
    u8g2_DrawStr(u8g2, 2, 8, buf);

    /* Layer indicator: "L0 DRM" / "L1 MEL" */
    const char *type_str = (layer->type == SEQ_LAYER_DRUM) ? "DRM" : "MEL";
    snprintf(buf, sizeof(buf), "L%d %s", state->active_layer_idx + 1, type_str);
    u8g2_DrawStr(u8g2, 52, 8, buf);

    if (layer->type == SEQ_LAYER_MELODIC) {
        /* Patch number, right-aligned and in 5x7 so 1-3 digits always fit the
         * 84..102 budget and never grow into the play-icon column at x=105.
         * In TRACK scope it is the cursor row's patch, so it follows the
         * cursor; the field has no room for a row marker. */
        const uint8_t patch_right = 102;   /* last column the digits may touch */
        uint16_t shown = (layer->patch_scope == SEQ_PATCH_SCOPE_TRACK)
                       ? layer->track_patch[state->selected_track]
                       : layer->patch;
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        snprintf(buf, sizeof(buf), "%u", (unsigned)shown);
        uint8_t pw = (uint8_t)u8g2_GetStrWidth(u8g2, buf);
        uint8_t px = (pw < patch_right) ? (uint8_t)(patch_right - pw) : 0;
        if (state->patch_select_mode) {
            /* Frame sized to the digits, never crossing into the play icon. */
            u8g2_DrawRFrame(u8g2, (uint8_t)(px - 2), 0,
                            (uint8_t)(pw + 4), 10, 1);
        }
        u8g2_DrawStr(u8g2, px, 8, buf);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    }

    /* Play / pause icon (fixed column at x=106, collision-free) */
    if (state->playing) {
        u8g2_DrawTriangle(u8g2, 106, 2, 106, 7, 113, 4);   /* ▶ */
    } else {
        u8g2_DrawBox(u8g2, 106, 2, 2, 6);
        u8g2_DrawBox(u8g2, 111, 2, 2, 6);                  /* ▮▮ */
    }

    u8g2_DrawHLine(u8g2, 0, 10, 128);

    /* === GRID === */
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
    const seq_grid_geom_t geom = seq_grid_geom(num_tracks, num_steps);
    const int grid_x    = geom.grid_x;
    const int col_w     = geom.col_w;
    const int cell_w    = geom.cell_w;
    const int cell_size = geom.cell_size;
    const int row_h     = geom.row_h;
    const int grid_top  = geom.grid_top;

    for (int t = 0; t < num_tracks; t++) {
        int y = grid_top + t * row_h;

        /* Track label: drums show the static role, melodic the pitch name. */
        char note_buf[4];
        const char *label;
        if (layer->type == SEQ_LAYER_DRUM) {
            label = drum_track_labels[t];
        } else {
            note_name_str(layer->track_base_note[t], note_buf);
            label = note_buf;
        }

        if (state->drum_select_mode && t == (int)state->selected_track) {
            u8g2_DrawBox(u8g2, 0, y, 25, row_h - 1);       /* filled bg */
            u8g2_SetDrawColor(u8g2, 0);                     /* black text */
            u8g2_DrawStr(u8g2, 1, y + 6, label);
            u8g2_SetDrawColor(u8g2, 1);                     /* restore */
        } else {
            u8g2_DrawStr(u8g2, 2, y + 6, label);
        }

        for (int s = 0; s < (int)num_steps; s++) {
            int x = grid_x + s * col_w;
            if (layer->grid[t][s]) {
                u8g2_DrawBox(u8g2, x + 1, y, cell_w, cell_size);
            } else if (cell_w >= 3) {
                u8g2_DrawFrame(u8g2, x + 1, y, cell_w, cell_size);
            } else {
                u8g2_DrawHLine(u8g2, x + 1, y + cell_size / 2, cell_w);
            }
        }
    }

    /* Beat separators (every 4 steps) */
    for (int b = 1; b < (int)num_steps / 4; b++) {
        int x = grid_x + (b * 4) * col_w - 1;
        u8g2_DrawVLine(u8g2, x, geom.span_top, geom.span_h);
    }

    /* === PLAYHEAD (XOR highlight) === */
    uint8_t cur_step = state->current_step;
    if ((state->playing || state->edit_mode) && cur_step < num_steps) {
        int ph_x = grid_x + cur_step * col_w - 1;
        u8g2_SetDrawColor(u8g2, 2);
        u8g2_DrawBox(u8g2, ph_x, geom.span_top, col_w + 1, geom.span_h);
        u8g2_SetDrawColor(u8g2, 1);
    }

    /* === SELECTION CURSOR === */
    if (state->edit_mode) {
        uint8_t sel = state->selected_step;
        if (sel < num_steps) {
            int sel_y = grid_top + state->selected_track * row_h - 1;
            int sel_x = grid_x + sel * col_w;
            u8g2_DrawRFrame(u8g2, sel_x, sel_y, cell_w + 2, cell_size + 2, 1);
        }
    }

    /* === PATCH-SELECT NAME OVERLAY ===
     * Centred banner with the current patch's human name while browsing. Only
     * drawn when the name table is compiled in (CONFIG_SEQ_PATCH_SHOW_NAMES);
     * patch_name_for() returns NULL otherwise, so this block costs nothing. */
    if (state->patch_select_mode) {
        const char *pname;
        if (layer->type == SEQ_LAYER_MELODIC) {
            /* TRACK scope browses the cursor row's timbre, so name that one. */
            pname = patch_name_for(
                (layer->patch_scope == SEQ_PATCH_SCOPE_TRACK)
                    ? layer->track_patch[state->selected_track]
                    : layer->patch);
        } else if (state->drum_pcm) {
            /* Drum layer, PCM engine: the selected track's ROM sample. */
            pname = pcm_preset_name_for(layer->track_pcm_preset[state->selected_track]);
        } else {
            /* Drum layer, SYNTH engine: the selected track's patch. */
            pname = patch_name_for(layer->track_patch[state->selected_track]);
        }
        if (pname) {
            u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
            uint8_t nw = (uint8_t)u8g2_GetStrWidth(u8g2, pname);
            if (nw > 124) nw = 124;
            uint8_t bx = (uint8_t)((128 - (nw + 4)) / 2);
            uint8_t by = 26;            /* banner top, over the grid area */
            /* Clear a box then frame + centred text for contrast over the grid. */
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawBox(u8g2, bx, by, (uint8_t)(nw + 4), 13);
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawRFrame(u8g2, bx, by, (uint8_t)(nw + 4), 13, 2);
            u8g2_DrawStr(u8g2, (uint8_t)(bx + 2), (uint8_t)(by + 9), pname);
        }
    }

    /* === FM-ALGORITHM BANNER (Shift+Turn feedback) ===
     * Same overlay shape as the patch-name banner above; timed rather than
     * hold-gated, so it outlives the detent by ~a second. Drawn last: if both
     * banners are up (patch hold + shift turn) this one wins. */
    if (state->algo_banner_ticks > 0) {
        char abuf[12];
        if (state->algo_banner_value == DISPLAY_ALGO_BANNER_NOFM) {
            snprintf(abuf, sizeof(abuf), "NOT FM");
        } else if (state->algo_banner_value == DISPLAY_ALGO_BANNER_CUSTOM) {
            snprintf(abuf, sizeof(abuf), "ALGO CUST");
        } else {
            snprintf(abuf, sizeof(abuf), "ALGO %u",
                     (unsigned)state->algo_banner_value);
        }
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        uint8_t nw = (uint8_t)u8g2_GetStrWidth(u8g2, abuf);
        uint8_t bx = (uint8_t)((128 - (nw + 4)) / 2);
        uint8_t by = 26;
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawBox(u8g2, bx, by, (uint8_t)(nw + 4), 13);
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawRFrame(u8g2, bx, by, (uint8_t)(nw + 4), 13, 2);
        u8g2_DrawStr(u8g2, (uint8_t)(bx + 2), (uint8_t)(by + 9), abuf);
    }
}

