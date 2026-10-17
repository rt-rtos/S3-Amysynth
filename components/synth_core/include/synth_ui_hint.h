#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── On-screen button hint bar ────────────────────────────────────────────
 * MY_BUTTON_1/2/3 are overloaded across screens and overlays. This module
 * never dispatches input: it only labels the buttons for the view that
 * synth_ui_active_view() reports. */

/* Static single-line buffer owned by this module, valid until the next call,
 * e.g. "1:Patch 2:Pitch 3:Menu". Cheap enough to call every frame. */
const char *synth_ui_hint_text(void);

/* False while synth_ui_active_view() is a view that draws into the strip's
 * rows (the chord-progression and ball drop screens). Overlays opened from
 * those screens show the strip. */
bool synth_ui_hint_visible(void);

#ifdef __cplusplus
}
#endif
