#include "display_arp.h"
#include <stdio.h>
#include <string.h>

/* Small label+value field; framed when it is the cursor, double-framed when
 * also `editing` (value is live-adjustable). */
static void draw_field(u8g2_t *u8g2, uint8_t x, uint8_t y, const char *text,
                       bool selected, bool editing)
{
    uint8_t w = (uint8_t)u8g2_GetStrWidth(u8g2, text);
    if (selected) {
        u8g2_DrawRFrame(u8g2, (uint8_t)(x - 2), (uint8_t)(y - 8),
                        (uint8_t)(w + 4), 11, 1);
        if (editing) {
            u8g2_DrawRFrame(u8g2, (uint8_t)(x - 3), (uint8_t)(y - 9),
                            (uint8_t)(w + 6), 13, 1);
        }
    }
    u8g2_DrawStr(u8g2, x, y, text);
}

void display_arp_draw_frame(u8g2_t *u8g2, const arp_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);

    if (view == NULL) {
        return;
    }

    char buf[16];
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);

    /* ── Macro row 1 (yellow header): ARP | MODE | OCT | RATE ──
     * Cursor order 0..3 runs left-to-right. RATE is right-aligned so the
     * triplet names ("1/16T") can't run off-edge. */
    snprintf(buf, sizeof(buf), "ARP:%s", view->enabled ? "ON" : "OFF");
    draw_field(u8g2, 2, 8, buf, view->cursor == ARP_CUR_ENABLE, view->editing);

    snprintf(buf, sizeof(buf), "%s", view->mode_str ? view->mode_str : "UP");
    draw_field(u8g2, 40, 8, buf, view->cursor == ARP_CUR_MODE, view->editing);

    snprintf(buf, sizeof(buf), "OCT:%u", (unsigned)view->octaves);
    draw_field(u8g2, 62, 8, buf, view->cursor == ARP_CUR_OCT, view->editing);

    snprintf(buf, sizeof(buf), "R:%s", view->rate_str ? view->rate_str : "?");
    {
        uint8_t rw = (uint8_t)u8g2_GetStrWidth(u8g2, buf);
        uint8_t rx = (rw < 126u) ? (uint8_t)(126u - rw) : 0u;
        draw_field(u8g2, rx, 8, buf, view->cursor == ARP_CUR_RATE, view->editing);
    }

    /* ── Macro row 2 (blue): GATE | PATCH | QUANT | GLIDE ──
     * Cursor order 4..6 left-to-right. Baseline 25 clears the 16px yellow/blue
     * seam so text and cursor frames stay in the blue region. Four fields at
     * 5 px/char leave no slack: GATE is abbreviated to "GT:" and the patch
     * indicator sits right after it, which is what buys the quant field its
     * room between the patch number and the right-aligned glide. */
    snprintf(buf, sizeof(buf), "GT:%u%%", (unsigned)view->gate_pct);
    draw_field(u8g2, 2, 25, buf, view->cursor == ARP_CUR_GATE, view->editing);

    /* Patch indicator: number always, framed during the hold+turn gesture
     * (full name banners over the slot grid), as on the sequencer view. */
    {
        const uint8_t ix = 41;
        char ibuf[12];
        snprintf(ibuf, sizeof(ibuf), "P%u", (unsigned)view->patch);
        if (view->patch_select) {
            uint8_t iw = (uint8_t)u8g2_GetStrWidth(u8g2, ibuf);
            u8g2_DrawRFrame(u8g2, (uint8_t)(ix - 2), 17, (uint8_t)(iw + 4), 11, 1);
        }
        u8g2_DrawStr(u8g2, ix, 25, ibuf);
    }

    /* GLIDE (portamento), right-aligned: raw ms ("GL:200") or "GL:off" at
     * zero. Selected on the PORTA cursor. */
    uint8_t glide_x;
    {
        char gbuf[12];
        uint16_t ms = view->portamento_ms;
        if (ms == 0) {
            snprintf(gbuf, sizeof(gbuf), "GL:off");
        } else {
            snprintf(gbuf, sizeof(gbuf), "GL:%u", (unsigned)ms);
        }
        uint8_t gw = (uint8_t)u8g2_GetStrWidth(u8g2, gbuf);
        glide_x = (gw < 126u) ? (uint8_t)(126u - gw) : 0u;
        draw_field(u8g2, glide_x, 25, gbuf, view->cursor == ARP_CUR_PORTA, view->editing);
    }

    /* Scale source: GLOB snaps to the global quantizer, OWN to the arp's
     * private scale. Right-aligned against the glide field so the widest value
     * keeps clear of it whatever the glide time reads. */
    {
        snprintf(buf, sizeof(buf), "Q:%s", view->follow_quant ? "GLOB" : "OWN");
        uint8_t qw = (uint8_t)u8g2_GetStrWidth(u8g2, buf);
        uint8_t qx = (glide_x > (uint8_t)(qw + 3u)) ? (uint8_t)(glide_x - qw - 3u) : 0u;
        draw_field(u8g2, qx, 25, buf, view->cursor == ARP_CUR_QUANT, view->editing);
    }

    u8g2_DrawHLine(u8g2, 0, 28, 128);

    /* ── Note slots: two rows of 4 cells ── */
    const uint8_t cell_w = 30;
    const uint8_t cell_h = 14;
    const uint8_t base_x = 2;
    const uint8_t base_y = 40;   /* baseline of first slot row text */
    for (uint8_t i = 0; i < ARP_VIEW_SLOTS; i++) {
        uint8_t col = i % 4;
        uint8_t rowi = i / 4;
        uint8_t cx = (uint8_t)(base_x + col * (cell_w + 1));
        uint8_t cy = (uint8_t)(base_y + rowi * (cell_h + 2));

        const char *txt = view->slot_active[i] ? view->slot_name[i]
                        : view->slot_rest[i]   ? " R "
                        :                        "--";
        uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, txt);
        uint8_t tx = (uint8_t)(cx + (cell_w - tw) / 2);

        bool selected = (view->cursor == (ARP_CUR_SLOT0 + i));
        if (selected) {
            u8g2_DrawRFrame(u8g2, cx, (uint8_t)(cy - 10), cell_w, cell_h, 1);
            if (view->editing) {
                u8g2_DrawRFrame(u8g2, (uint8_t)(cx - 1), (uint8_t)(cy - 11),
                                (uint8_t)(cell_w + 2), (uint8_t)(cell_h + 2), 1);
            }
        }
        u8g2_DrawStr(u8g2, tx, cy, txt);
    }

    /* Patch-name banner over the slot grid during the hold+turn gesture, same
     * affordance as the sequencer view. patch_name is NULL when the name table
     * is compiled out, so this costs nothing then. */
    if (view->patch_select && view->patch_name) {
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        uint8_t nw = (uint8_t)u8g2_GetStrWidth(u8g2, view->patch_name);
        if (nw > 124) nw = 124;
        uint8_t bx = (uint8_t)((128 - (nw + 4)) / 2);
        uint8_t by = 34;            /* banner top, over the slot area */
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawBox(u8g2, bx, by, (uint8_t)(nw + 4), 13);
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawRFrame(u8g2, bx, by, (uint8_t)(nw + 4), 13, 2);
        u8g2_DrawStr(u8g2, (uint8_t)(bx + 2), (uint8_t)(by + 9), view->patch_name);
    }
}
