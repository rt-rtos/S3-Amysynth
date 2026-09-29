#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Top-row BLE session badge, drawn over the finished frame. Lands only on
 * blank top-row pixels: the preferred x if free, else the nearest free slot,
 * else nowhere. Needs u8g2 full-buffer mode at U8G2_R0 and draws nothing
 * otherwise. */

/* Draws the BLE badge, returning true if it was drawn. preferred_x is a hint,
 * not a promise; pass 0 for "wherever it fits". connected selects the inverted
 * plate (central attached) over the bare glyph (advertising only). */
bool display_badge_draw(u8g2_t *u8g2, uint8_t preferred_x, bool connected);

/* Draws a short text badge (4x6 font on an inverted plate, 8 rows high) with
 * the same placement rules: nearest clear top-row slot to preferred_x, nothing
 * when the row is full. Returns true if it was drawn. */
bool display_badge_draw_text(u8g2_t *u8g2, uint8_t preferred_x, const char *text);

#ifdef __cplusplus
}
#endif
