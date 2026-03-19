#include "priv_u8g2_seq.h"
#include <stdio.h>

static const char* track_labels[SEQ_TRACKS] = {"BD", "SD", "CH", "OH"};

void priv_u8g2_seq_draw_frame(u8g2_t *u8g2, const priv_u8g2_seq_state_t *state) {
    u8g2_ClearBuffer(u8g2);

    // === HEADER ===
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    char buf[16];
    snprintf(buf, sizeof(buf), "BPM %3d", state->bpm);
    u8g2_DrawStr(u8g2, 2, 8, buf);

    snprintf(buf, sizeof(buf), "PAT %02d", state->current_pattern);
    u8g2_DrawStr(u8g2, 55, 8, buf);

    // Play/pause icon
    if (state->playing) {
        u8g2_DrawTriangle(u8g2, 105, 2, 105, 7, 112, 4);   // ▶
    } else {
        u8g2_DrawBox(u8g2, 105, 2, 2, 6);
        u8g2_DrawBox(u8g2, 110, 2, 2, 6);                 // ▮▮
    }

    u8g2_DrawHLine(u8g2, 0, 10, 128);

    // === GRID ===
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);   // labels
    const int grid_x = 26;
    const int col_w = 6;
    const int cell_size = 5;
    const int row_h = 10; // Increased row height for 4 tracks to fill more space
    const int grid_top = 20; // moved down a couple rows to add space under header

    for (int t = 0; t < SEQ_TRACKS; t++) {
        int y = grid_top + t * row_h;

        // Track label
        u8g2_DrawStr(u8g2, 2, y + 6, track_labels[t]);

        // Step cells
        for (int s = 0; s < SEQ_STEPS; s++) {
            int x = grid_x + s * col_w;

            if (state->grid[t][s]) {
                u8g2_DrawBox(u8g2, x + 1, y, cell_size, cell_size);   // filled = trigger ON
            } else {
                u8g2_DrawFrame(u8g2, x + 1, y, cell_size, cell_size); // empty outline
            }
        }
    }

    // Bar separators (every 4 steps)
    for (int b = 1; b < 4; b++) {
        int x = grid_x + (b * 4) * col_w - 1;
        u8g2_DrawVLine(u8g2, x, grid_top - 3, SEQ_TRACKS * row_h + 2);
    }

    // === PLAYHEAD (XOR highlight) ===
    if (state->playing || state->edit_mode) {
        int ph_x = grid_x + state->current_step * col_w - 1;
        u8g2_SetDrawColor(u8g2, 2);           // XOR = invert whatever is there
        u8g2_DrawBox(u8g2, ph_x, grid_top - 3, col_w + 1, SEQ_TRACKS * row_h + 2);
        u8g2_SetDrawColor(u8g2, 1);           // back to normal
    }

    // === SELECTION CURSOR (blinking or solid) ===
    if (state->edit_mode) {
        int sel_y = grid_top + state->selected_track * row_h - 1;
        int sel_x = grid_x + state->selected_step * col_w;
        u8g2_DrawRFrame(u8g2, sel_x, sel_y, cell_size + 2, cell_size + 2, 1); // rounded for visibility
    }

    u8g2_SendBuffer(u8g2);
}
