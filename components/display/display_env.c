#include "display_env.h"

#define ENV_MID_X0  60      /* left edge of the shared mid/right band */

static void env_draw_topbar(u8g2_t *u8g2, const env_view_t *v)
{
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(u8g2, 2, 8, v->label);

    uint8_t rw = 0;
    if (v->right[0]) {
        rw = (uint8_t)u8g2_GetStrWidth(u8g2, v->right);
        if (v->right_flash) {
            /* Inverted pad so it reads as an event, not a label. */
            u8g2_DrawBox(u8g2, (uint8_t)(128 - rw - 4), 0, (uint8_t)(rw + 4), 11);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, (uint8_t)(128 - rw - 2), 8, v->right);
            u8g2_SetDrawColor(u8g2, 1);
        } else {
            u8g2_DrawStr(u8g2, (uint8_t)(128 - rw - 2), 8, v->right);
        }
    }

    if (v->active[0]) {
        int x = (int)u8g2_GetStrWidth(u8g2, v->label) + 2 + 2;   /* label at x=2 */
        u8g2_SetFont(u8g2, u8g2_font_5x7_tf);
        int aw = (int)u8g2_GetStrWidth(u8g2, v->active);
        if (x + aw < (int)(128 - rw - 4))
            u8g2_DrawStr(u8g2, (uint8_t)x, 8, v->active);
    }

    if (v->mid[0]) {
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, v->mid);
        /* Between the left label (~x=56) and the right readout. */
        int mx = ENV_MID_X0 + (int)((128 - ENV_MID_X0 - (int)rw - 4 - (int)tw) / 2);
        if (mx < ENV_MID_X0) mx = ENV_MID_X0;
        u8g2_DrawStr(u8g2, (uint8_t)mx, 8, v->mid);
    }

    /* Divider at the yellow/blue boundary. */
    u8g2_DrawHLine(u8g2, 0, ENV_TOPBAR_H - 1, 128);
}

void env_view_draw(u8g2_t *u8g2, const env_view_t *v, const gpopup_t *plot)
{
    u8g2_SetDrawColor(u8g2, 1);
    env_draw_topbar(u8g2, v);
    graph_popup_draw(u8g2, plot);

    /* The top bar's right slot belongs to the point readout, so the curve
     * type stays visible here. The cleared pad keeps it legible when the
     * curve passes underneath. */
    if (v->type_code) {
        u8g2_SetFont(u8g2, u8g2_font_4x6_tr);
        uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, v->type_code);
        uint8_t tx = (uint8_t)(128 - tw - 2);
        uint8_t ty = (uint8_t)(ENV_TOPBAR_H + 8);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawBox(u8g2, (uint8_t)(tx - 1), (uint8_t)(ty - 6), (uint8_t)(tw + 3), 8);
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawStr(u8g2, tx, ty, v->type_code);
    }
}
