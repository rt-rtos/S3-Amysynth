#pragma once

/* ── Mixer quick cells ───────────────────────────────────────────────────
 * Two fixed parameters per mixer row kind (Q1, Q2), each an existing setter
 * reached through its own getter, so nothing is stored here:
 *   melodic layer  CUT (every row's filter cutoff) / GAT (layer gate)
 *   drum layer     SWG (layer swing)               / -
 *   arp            OCT (octaves)                   / GLD (glide ms)
 *   stutter drone  PAT (gate pattern)              / RT (root note)
 *   normal drone   RT (root note)                  / CUT (filter cutoff)
 *   live voice     GLD (glide ms)                  / CUT (filter cutoff)
 * Track rows, master and clips have none. CUT moves the stored cutoff by a
 * semitone per detent (a ratio, so rows at different cutoffs move by the
 * same interval), clamped to 65..16000 Hz; it edits the stored value also
 * while the filter is off.
 *
 * Contract: UI/input task (Core 0) only, like the setters it calls; pushes
 * go through those setters (amy_helpers), never amy_queue_lock. slot 0 = Q1,
 * 1 = Q2; a slot the row does not have reads as absent and steps as a
 * no-op. */

#include <stdbool.h>
#include <stdint.h>
#include "mixer.h"   /* mix_src_t */

bool        mixer_quick_has(mix_src_t s, uint8_t slot);
/* Three-character label ("CUT"), "" when absent. */
const char *mixer_quick_label(mix_src_t s, uint8_t slot);
/* The value in at most four characters, "" when absent. */
void        mixer_quick_text(mix_src_t s, uint8_t slot, char out[5]);
/* Move the value by `detents` steps of its own unit, clamped. */
void        mixer_quick_step(mix_src_t s, uint8_t slot, int detents);
