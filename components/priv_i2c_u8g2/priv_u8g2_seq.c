#include "priv_u8g2_seq.h"
#include "patch_names.h"
#include <stdio.h>

/* ── GM percussion note names (index 0 = MIDI note 27, index 60 = note 87) ── */
static const char *const s_gm_drum_names[] = {
    /* 27 */ "HiQ",  "Slap", "ScP",  "ScL",  "Stk",  "SqCl", "MtCl", "MtBl",
    /* 35 */ "ABD",  "BD1",  "Rim",  "Snr",  "Clap", "SN2",
    /* 41 */ "LFT",  "CHH",  "HFT",  "PHH",  "LoT",  "OHH",
    /* 47 */ "LMT",  "HMT",  "Cr1",  "HiT",  "Rid",  "Chi",
    /* 53 */ "RdB",  "Tam",  "Spl",  "CBl",  "Cr2",  "Vib",
    /* 59 */ "Rd2",  "HBo",  "LBo",  "MHC",  "OHC",  "LCo",
    /* 65 */ "HTi",  "LTi",  "HAg",  "LAg",  "Cab",  "Mar",
    /* 71 */ "ShW",  "LgW",  "ShG",  "LgG",  "Clv",  "HWB",
    /* 77 */ "LWB",  "MCu",  "OCu",  "MTr",  "OTr",  "Shk",
    /* 83 */ "JBl",  "BTr",  "Cas",  "MSr",  "OSr"
};
#define GM_DRUM_NOTE_MIN 27
#define GM_DRUM_NOTE_MAX 87

static const char *gm_drum_short_name(uint8_t note)
{
    if (note < GM_DRUM_NOTE_MIN || note > GM_DRUM_NOTE_MAX) return "???";
    return s_gm_drum_names[note - GM_DRUM_NOTE_MIN];
}

/* Produce a 3-char null-terminated note name: "C4", "C#4", "D4" … */
static void note_name_str(uint8_t midi_note, char buf[4])
{
    static const char *const note_names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    int octave = (int)(midi_note / 12) - 1;
    snprintf(buf, 4, "%s%d", note_names[midi_note % 12], octave);
}

void priv_u8g2_seq_draw_frame(u8g2_t *u8g2, const priv_u8g2_seq_state_t *state)
{
    /* Nothing to draw until at least one layer exists. */
    if (state->num_layers == 0) {
        u8g2_ClearBuffer(u8g2);
        u8g2_SendBuffer(u8g2);
        return;
    }

    const seq_layer_t *layer     = &state->layers[state->active_layer_idx];
    const uint8_t      num_steps = layer->num_steps;
    const uint8_t      page      = layer->step_page;
    const uint8_t      step_start = (uint8_t)(page * 16);

    u8g2_ClearBuffer(u8g2);

    /* === HEADER === */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    char buf[16];

    snprintf(buf, sizeof(buf), "BPM %3d", state->bpm);
    u8g2_DrawStr(u8g2, 2, 8, buf);

    /* Layer indicator: "L0 DRM" / "L1 MEL" */
    const char *type_str = (layer->type == SEQ_LAYER_DRUM) ? "DRM" : "MEL";
    snprintf(buf, sizeof(buf), "L%d %s", state->active_layer_idx, type_str);
    u8g2_DrawStr(u8g2, 52, 8, buf);

    if (layer->type == SEQ_LAYER_MELODIC) {
        /* Patch number, right-aligned to a fixed right edge so a 1-3 digit
         * value never grows into the play-icon column at x=105. The "Pt"
         * prefix is dropped and a smaller 5x7 font is used to stay within the
         * 84..102 budget regardless of digit count. */
        const uint8_t patch_right = 102;   /* last column the digits may touch */
        snprintf(buf, sizeof(buf), "%u", (unsigned)layer->patch);
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
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

    /* Page indicator for 32-step layers ("P1" / "P2") */
    if (num_steps == SEQ_MAX_STEPS) {
        snprintf(buf, sizeof(buf), "Pg%d", page + 1);
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        u8g2_DrawStr(u8g2, 120, 8, buf);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    }

    u8g2_DrawHLine(u8g2, 0, 10, 128);

    /* === GRID === */
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
    const int grid_x    = 26;
    const int col_w     = 6;
    const int cell_size = 5;
    const int row_h     = 10;
    const int grid_top  = 20;

    for (int t = 0; t < SEQ_TRACKS; t++) {
        int y = grid_top + t * row_h;

        /* Track label */
        char label[4];
        if (layer->type == SEQ_LAYER_DRUM) {
            snprintf(label, sizeof(label), "%s",
                     gm_drum_short_name(layer->track_base_note[t]));
        } else {
            note_name_str(layer->track_base_note[t], label);
        }

        if (state->drum_select_mode && t == (int)state->selected_track) {
            u8g2_DrawBox(u8g2, 0, y, 25, row_h - 1);       /* filled bg */
            u8g2_SetDrawColor(u8g2, 0);                     /* black text */
            u8g2_DrawStr(u8g2, 1, y + 6, label);
            u8g2_SetDrawColor(u8g2, 1);                     /* restore */
        } else {
            u8g2_DrawStr(u8g2, 2, y + 6, label);
        }

        /* 16-cell window for the current page */
        for (int s = 0; s < 16; s++) {
            int abs_step = (int)step_start + s;
            int x        = grid_x + s * col_w;
            if (layer->grid[t][abs_step]) {
                u8g2_DrawBox(u8g2, x + 1, y, cell_size, cell_size);
            } else {
                u8g2_DrawFrame(u8g2, x + 1, y, cell_size, cell_size);
            }
        }
    }

    /* Beat separators (every 4 visible steps) */
    for (int b = 1; b < 4; b++) {
        int x = grid_x + (b * 4) * col_w - 1;
        u8g2_DrawVLine(u8g2, x, grid_top - 3, SEQ_TRACKS * row_h + 2);
    }

    /* === PLAYHEAD (XOR highlight) === */
    uint8_t cur_step = state->current_step;
    if ((state->playing || state->edit_mode) &&
        cur_step >= step_start && cur_step < step_start + 16) {
        int ph_x = grid_x + (cur_step - step_start) * col_w - 1;
        u8g2_SetDrawColor(u8g2, 2);
        u8g2_DrawBox(u8g2, ph_x, grid_top - 3, col_w + 1, SEQ_TRACKS * row_h + 2);
        u8g2_SetDrawColor(u8g2, 1);
    }

    /* === SELECTION CURSOR === */
    if (state->edit_mode) {
        uint8_t sel_abs = state->selected_step;
        if (sel_abs >= step_start && sel_abs < step_start + 16) {
            int sel_y = grid_top + state->selected_track * row_h - 1;
            int sel_x = grid_x + (sel_abs - (int)step_start) * col_w;
            u8g2_DrawRFrame(u8g2, sel_x, sel_y, cell_size + 2, cell_size + 2, 1);
        }
    }

    /* === PATCH-SELECT NAME OVERLAY ===
     * While the user is browsing patches (BPM button held into patch-select),
     * show the current patch's human name in a centred banner over the grid so
     * the full-range browser is legible. Only drawn when the name table is
     * compiled in (CONFIG_SEQ_PATCH_SHOW_NAMES); otherwise the top-bar number
     * is the only readout. patch_name_for() is a no-op returning NULL when the
     * table is excluded, so this whole block costs nothing then. */
    if (state->patch_select_mode && layer->type == SEQ_LAYER_MELODIC) {
        const char *pname = patch_name_for(layer->patch);
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

    u8g2_SendBuffer(u8g2);
}

