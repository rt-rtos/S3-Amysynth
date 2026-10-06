#pragma once

#include "iot_button.h"
#include "my_buttons.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Gesture dispatcher: turns button events and encoder steps into synth_ui
 * calls for the active view (synth_ui_active_view()), and owns the hold state
 * the gestures share: patch hold, pitch hold, SHIFT with its per-button chord
 * latches, and the SHIFT+0 bounce chord. Which gesture does what on which
 * screen is CONTROLS.md.
 *
 * Execution context, both functions: task context on Core 0, after
 * synth_ui_init(). Not ISR-safe and not render-path-safe. The per-screen
 * branches run editor code with deep frames; the calling task needs a stack
 * like the button and encoder tasks have (8192).
 *
 * Each function has one caller at a time. The two may run concurrently with
 * each other: the encoder side only reads the hold flags the button side
 * writes. */

/* One button event. Obligation: `event` is BUTTON_PRESS_DOWN, BUTTON_PRESS_UP,
 * BUTTON_SINGLE_CLICK or BUTTON_LONG_PRESS_START, delivered in the order the
 * button produced them; a hold only ends when its PRESS_UP arrives. */
void input_dispatch_button(my_button_id_t button_id, button_event_t event);

/* `steps` encoder detents (signed, nonzero), routed to the active view with
 * the hold modifiers applied. */
void input_dispatch_encoder_steps(long steps);

#ifdef __cplusplus
}
#endif
