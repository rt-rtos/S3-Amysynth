#pragma once

/* ── Shared LFO bank ─────────────────────────────────────────────────────
 * One AMY synth per unique LFO source, shared by every consumer voice that
 * uses the same source settings and wired to it through mod slot 1
 * (voice_lfo_wire() in voice_config.h). The bank owns synth numbers
 * LFO_SYNTH_BASE .. LFO_SYNTH_BASE + LFO_SYNTH_MAX - 1 (synth_slots.h), which
 * are no consumer's slot. Each LFO synth is one voice of two oscs: osc 0 the
 * carrier, osc 1 the wobble, configured by voice_lfo_source_apply().
 *
 * Key: the seq_lfo_t fields that configure the source - wave, rate,
 * wob_rate, wob_depth, wob_reach. depth, flt_oct_q, targets and mode stay
 * per consumer (they become that consumer's COEF_MOD1 depths).
 *
 * Contract:
 * - Execution context: UI task (Core 0) only, every function but
 *   lfo_bank_info() (its own contract below). Sends go
 *   through amy_helpers, never under amy_queue_lock; nothing here allocates
 *   or blocks, and acquire never waits.
 * - A send is not an apply (amy_helpers.h): an AMY-side failure creating an
 *   LFO synth (OOM in the patch loader) cannot be observed here and degrades
 *   to no modulation, not to the stepper. Acquire returns LFO_BANK_NONE (the
 *   caller falls back to its software stepper) only for a NULL or disabled
 *   lfo or when every entry is in use, and always when CONFIG_SEQ_LFO_SHARED
 *   is off.
 * - Consumers acquire the new key before releasing the old one, so an
 *   unchanged source keeps its synth (and phase) across a re-apply.
 * - An LFO synth's oscs never name a consumer as a mod source: AMY has no
 *   cycle guard for synth references.
 * - A released entry stays alive until lfo_bank_service() frees it; an
 *   acquire of the same key before then revives it. A released entry with a
 *   phase warp still due is freed only after that warp's AMY time.
 * - Bar lock: while the transport plays, every LFO synth's carrier has phase
 *   0 at the start of each of its cycles on the bar grid counted from the
 *   bar baseline (sequencer_core_bar_baseline_tick(); one cycle =
 *   note_div_ticks(rate)). A new synth is warped onto the grid when it is
 *   created; each bar line then re-warps every live synth at that bar's AMY
 *   time. Nothing is warped while the transport is stopped. A carrier whose
 *   rate the wobble swings (wob_depth > 0, reach not DEPTH) is never locked,
 *   nor one held at the native rate cap at the current BPM (lfo_rate_to_hz()
 *   below the note division's own frequency), which runs off its
 *   note-division period. */

#include <stdbool.h>
#include <stdint.h>
#include "seq_model.h"     /* seq_lfo_t */
#include "lfo_targets.h"   /* lfo_target_map_t */

#define LFO_BANK_NONE 0xFFu   /* no LFO synth: the consumer uses the stepper */

/* Clear the table. Called once from sequencer_core_init(), before any
 * consumer can acquire; sends nothing. */
void lfo_bank_init(void);

/* Synth number of the LFO synth for lfo's source fields at bpm: an existing
 * entry with an equal key gains a reference, else the first free entry is
 * created (pool definition, then voice_lfo_source_apply()). LFO_BANK_NONE
 * per the contract above. */
uint8_t lfo_bank_acquire(const seq_lfo_t *lfo, uint16_t bpm);

/* Drop one reference. At zero the entry is freed by a later
 * lfo_bank_service(). No-op for LFO_BANK_NONE or a number outside the bank. */
void lfo_bank_release(uint8_t n);

/* Schedule the bar-line warps due within the next two frames (bar lock,
 * above), then free every released entry whose hold time has passed. 50 ms
 * UI frame. */
void lfo_bank_service(void);

/* Retune every live LFO synth (carrier and wobble freq) to bpm. Called from
 * sequencer_core_set_bpm(). */
void lfo_bank_set_bpm(uint16_t bpm);

/* ── Consumer binding ────────────────────────────────────────────────────
 * One consumer synth's link to the bank. *ref is the consumer's own record of
 * the LFO synth it holds (LFO_BANK_NONE = none); the caller owns it and
 * passes the same one every time. */
typedef enum {
    LFO_BIND_BANK,   /* wired to a shared LFO synth; *ref holds it          */
    LFO_BIND_OFF,    /* NULL, disabled or no targets: unwired, *ref NONE    */
    LFO_BIND_FULL,   /* no free entry: unwired, *ref NONE; the caller falls
                        back to its own path (carriers or stepper)          */
} lfo_bind_t;

/* Bind `synth` to the LFO synth for lfo's source at bpm and wire the oscs in
 * `map` through voice_lfo_wire(). For an enabled lfo with targets: acquire,
 * wire, then release the previous *ref (acquire before release, so an
 * unchanged source keeps its synth and phase). Otherwise, or when the bank is
 * full: unwire the oscs in `map` and release *ref. Execution context as the
 * bank (UI task only). With CONFIG_SEQ_LFO_SHARED off: sends nothing, sets
 * *ref to LFO_BANK_NONE and returns LFO_BIND_FULL. */
lfo_bind_t lfo_bank_bind(uint8_t synth, const lfo_target_map_t *map,
                         const seq_lfo_t *lfo, uint16_t bpm, uint8_t *ref);

/* Release *ref and set it to LFO_BANK_NONE. Sends no AMY event: for a
 * consumer whose wiring is already gone (patch re-apply, synth released) or
 * that unwired itself. UI task only. */
void lfo_bank_unbind(uint8_t *ref);

/* Read-only view of entry i (0 .. LFO_SYNTH_MAX - 1, synth_slots.h): its
 * synth number with *refs, *pending and *key (source fields only) filled, or
 * LFO_BANK_NONE for an unused or out-of-range entry (outputs untouched). Any
 * output may be NULL. Any task: a read-only snapshot that may tear against a
 * concurrent acquire/release; diagnostic use only (st.lfobank). */
uint8_t lfo_bank_info(uint8_t i, uint16_t *refs, bool *pending, seq_lfo_t *key);
