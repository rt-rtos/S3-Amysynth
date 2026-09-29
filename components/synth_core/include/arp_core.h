#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "seq_model.h"     /* seq_env_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Standalone arpeggiator ──────────────────────────────────────────────
 * Independent of the sequencer's layers: it owns up to ARP_MAX_SLOTS note
 * inputs, its own scale/root, its own AMY synth, and runs in sync with the
 * global tempo via repeating AMY SEQUENCE events.
 *
 * Slots store the RAW chromatic MIDI value. Special sentinels:
 *   -1 (empty)   : slot unused; not part of the playback cycle.
 *   ARP_REST (-2): deliberate silent step (SLOT mode only); occupies time but
 *                  plays nothing. Meaningless in UP/DOWN - store only -1 there.
 * Snapping happens only at playback and for display, so the stored value never
 * absorbs its own quantized output - no compounding drift. */

#define ARP_MAX_SLOTS 8
#define ARP_REST ((int16_t)-2)   /* deliberate silent step in ARP_SLOT mode */

typedef enum {
    ARP_UP   = 0,
    ARP_DOWN = 1,
    ARP_SLOT = 2,   /* slot-order: position preserved, ARP_REST honoured */
    ARP_DIR_COUNT
} arp_dir_t;

/* RATE table: musical subdivision -> ticks per arp note.
 * AMY_SEQUENCER_PPQ = 48, so a 1/16 = 12 ticks (matches SEQ_TICKS_PER_STEP). */
typedef enum {
    ARP_RATE_1_1  = 0,
    ARP_RATE_1_4  = 1,   /* quarter note   */
    ARP_RATE_1_8  = 2,   /* eighth note    */
    ARP_RATE_1_16 = 3,  /* sixteenth note */
    ARP_RATE_1_32 = 4, /* thirty-second  */
    ARP_RATE_1_4T = 5,
    ARP_RATE_1_8T = 6,
    ARP_RATE_1_16T = 7,
    ARP_RATE_1_32T = 8,  
    ARP_RATE_COUNT
} arp_rate_t;

/* What arp_snap() snaps slot notes to. Persisted as a u8 in the project. */
typedef enum {
    ARP_QUANT_OWN    = 0,   /* the arp's own root/scale */
    ARP_QUANT_GLOBAL = 1,   /* the global scale quantizer */
    ARP_QUANT_CHORD  = 2,   /* the playing progression chord */
    ARP_QUANT_COUNT
} arp_quant_mode_t;

/* ── Lifecycle ── */
void arp_core_init(void);

/* Recompute and re-emit the whole arp sequence immediately, for callers that
 * must force a refresh (e.g. tempo changes). Setters instead mark dirty and
 * let arp_core_service() coalesce the re-emit. */
void arp_core_refresh(void);

/* Cancel all scheduled arp AMY events without re-emitting (sequencer pause),
 * leaving enabled/configuration state alone. Does NOT kill a voice mid-gate -
 * follow with sequencer_kill_synth_voices(SEQ_ARP_SYNTH). */
void arp_core_clear_all(void);

/* Drain a pending coalesced re-emit; cheap no-op otherwise. Call once per UI
 * frame so a fast encoder spin collapses into a single re-emit. */
void arp_core_service(void);

/* Request a coalesced re-emit. Used by the transport on resume: emission is
 * gated on the sequencer playing, so the schedule cleared at pause must be
 * rebuilt explicitly. */
void arp_core_mark_dirty(void);

/* ── Parameter setters (each re-emits the sequence) ── */
void arp_set_enabled(bool enabled);

/* Silence the arp without disturbing arp_set_enabled()'s state: used by the
 * sequencer solo hook, so releasing solo restores whatever the user had set.
 * Kills the sounding voices and re-emits synchronously - call from the UI task. */
void arp_set_solo_muted(bool muted);
void arp_set_direction(arp_dir_t dir);
void arp_set_octaves(uint8_t octaves);        /* clamped 1..ARP_OCT_MAX */
void arp_set_rate(arp_rate_t rate);
void arp_set_gate_pct(uint8_t gate_pct);      /* clamped 10..100        */
void arp_set_scale(uint8_t scale_index);
void arp_set_root_note(uint8_t root_note);
/* Choose what the slot notes snap to:
 *   ARP_QUANT_OWN    - the arp's own scale/root (arp_set_scale/root_note).
 *   ARP_QUANT_GLOBAL - the global scale quantizer; with the quantizer
 *                      disabled the arp plays chromatic.
 *   ARP_QUANT_CHORD  - the progression chord the melodic rows are playing
 *                      (its root, chord type mapped to a scale); while no
 *                      progression chord is applied it behaves as GLOBAL.
 * Values >= ARP_QUANT_COUNT clamp to CHORD, the boot default. */
void arp_set_quant_mode(arp_quant_mode_t mode);
arp_quant_mode_t arp_get_quant_mode(void);
/* Sound selection: one flat number over the full melodic catalog
 * (0..SEQ_PATCH_FULL_MAX - Juno/DX7 strings, raw waves, bass, wavetable, FM,
 * additive). Rebuilds the synth slot and re-applies any authored ADSR/filter. */
void arp_set_patch(uint16_t patch_number);
/* Live FM voice edits: re-push s_fm_voice to the arp synth when the arp is
 * playing it (PATCH source, SEQ_PATCH_FM_CUSTOM); no-op otherwise. Called from
 * sequencer_core_fm_voice_changed() alongside the melodic-layer fanout. */
void arp_core_fm_voice_changed(uint8_t what);   /* FM_PUSH_* / op index */
/* Same contract for s_additive_voice / SEQ_PATCH_ADDITIVE_CUSTOM. */
void arp_core_additive_voice_changed(void);
/* Set slot value to a chromatic MIDI note, -1 to clear, or ARP_REST for a
 * deliberate silent step (meaningful in ARP_SLOT mode only). */
void arp_set_slot(uint8_t idx, int16_t chromatic_note);

/* ── Runtime-editable ADSR envelope (shared graph editor) ──
 * One envelope applied to the arp synth's voices. set stores + pushes, and it
 * is re-applied after a patch change so it survives patch reconfig. */
void arp_get_envelope(seq_env_t *out);
void arp_set_envelope(const seq_env_t *env);

/* ── Second envelope (EG1, shared graph editor) ──
 * Independent breakpoint generator, audible only once something is wired to
 * COEF_EG1: the arp routes filter_freq_coefs through it whenever its filter is
 * authored+enabled, and many Juno/DX7 patches route their own bp1. */
void arp_get_envelope2(seq_env_t *out);
void arp_set_envelope2(const seq_env_t *env);

/* ── Runtime-editable filter (shared filter editor) ──
 * Parallel to the envelope; defaults to bypass. Re-applied after a patch
 * change when filter_authored. */
void arp_get_filter(seq_filter_t *out);
void arp_set_filter(const seq_filter_t *f);

/* ── Editor live-preview (AMY only; store + authored flags untouched) ──
 * Audition scratch editor values on the arp synth. Cancel restores by calling
 * these again with the stored values. */
void arp_preview_envelope(const seq_env_t *env);
void arp_preview_envelope2(const seq_env_t *env);
void arp_preview_filter(const seq_filter_t *f);
void arp_preview_dist(const seq_dist_t *d);

/* ── Runtime-editable distortion (shared distortion editor) ──
 * Parallel to the filter; defaults to type OFF. arp_reapply_dist() re-asserts
 * after a patch change, which rebuilds the voices and drops the stage. */
void arp_get_dist(seq_dist_t *out);
void arp_set_dist(const seq_dist_t *d);
void arp_reapply_dist(void);

/* ── LFO (shared editor) ──
 * Native carrier or 20 Hz software stepper, chosen per patch as on melodic
 * rows (sequencer_core_lfo_native_layout). */
void arp_get_lfo(seq_lfo_t *out);
void arp_set_lfo(const seq_lfo_t *lfo);

/* Recompute and push the LFO carrier frequency at the current BPM.
 * Called by sequencer_core_set_bpm() after s_bpm is updated.
 * No-op unless lfo_authored and the patch has a native carrier pair. */
void arp_core_refresh_lfo_freq(void);

/* ── Getters (for UI display) ── */
bool      arp_get_enabled(void);
arp_dir_t arp_get_direction(void);
uint8_t   arp_get_octaves(void);
arp_rate_t arp_get_rate(void);
const char *arp_rate_name(arp_rate_t rate);
uint8_t   arp_get_gate_pct(void);
uint8_t   arp_get_scale(void);
uint8_t   arp_get_root_note(void);
uint16_t     arp_get_patch(void);
int16_t   arp_get_slot(uint8_t idx);          /* raw chromatic, -1=empty, ARP_REST=-2 */
/* Snapped pitch the slot will actually play (for display), or -1 if empty/rest. */
int16_t   arp_get_slot_snapped(uint8_t idx);

/* ── Per-target amplitude trim (graph editor amp mode) ──
 * 0..1 multiplier on note velocity at emit time. Default 1.0 (unity).
 * set stores + marks dirty → coalesced re-emit on next arp_core_service(). */
void  arp_set_amp_scale(float v);
float arp_get_amp_scale(void);

/* ── Portamento / glide (AMY-native, PORTAMENTO_MS delta) ──
 * Milliseconds of exponential glide between note pitches, applied inside AMY
 * (portamento_alpha low-pass on logfreq); 0 = off, matching AMY's reset value.
 * Not a scheduling change, so it does not mark the arp dirty - but a
 * patch/synth reconfigure DOES wipe it (AMY resets portamento_alpha on osc
 * reset), so arp_rebuild() re-pushes it after every patch change. */
void     arp_set_portamento_ms(uint16_t ms);
uint16_t arp_get_portamento_ms(void);
#define ARP_PORTAMENTO_MAX_MS 100u    /* glide ceiling, ms (1 ms/detent).
                                       * Matches the melodic layer Glide range;
                                       * longer saved glides clamp on load. */

#define ARP_OCT_MAX 4

#ifdef __cplusplus
}
#endif
