#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Top-row status badge (BLE session indicator) ─────────────────────────
 * Pure rendering: the caller supplies the link state, this file owns the glyph
 * and where it lands.
 *
 * The badge has no layout of its own - it composites on top of whatever view
 * already filled the buffer, so a fixed x breaks the moment a header string
 * grows. Placement instead reads the frame buffer back and refuses to draw
 * over lit pixels: preferred slot when empty, nearest empty slot otherwise,
 * nothing at all when the top row is full. Header text always wins.
 *
 * Requires u8g2 in full-buffer mode at U8G2_R0 (how priv_i2c_u8g2.c sets the
 * panel up); display_badge_draw() verifies both and degrades to drawing
 * nothing rather than guessing. */

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
