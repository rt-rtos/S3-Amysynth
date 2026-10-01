#pragma once

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Differential panel flush
 * Sends only the tiles that changed since the last flush, using a shadow
 * copy of what the panel RAM holds.
 *
 * Obligations: display_flush is called from synth_ui_task only, after the
 * frame is fully drawn into the u8g2 buffer; u8g2 in full-buffer mode,
 * rotation U8G2_R0. Not reentrant. display_flush_invalidate may be called
 * from the same task, including from inside a display_flush (the I2C byte
 * callback does that on a failed transfer).
 *
 * Guarantees: on return the panel shows the buffer if every transfer
 * succeeded. After a failed transfer the shadow is invalid and the next
 * flush sends the full frame. If the shadow could not be allocated, every
 * flush is a full u8g2_SendBuffer.
 *
 * Invariant: the shadow equals panel RAM, or the shadow is invalid. */

/* Allocates the shadow; safe to call once at display init. */
void display_flush_init(void);

/* Sends what differs from the panel. */
void display_flush(u8g2_t *u8g2);

/* The next display_flush sends the full frame. */
void display_flush_invalidate(void);

#ifdef __cplusplus
}
#endif
