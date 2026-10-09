#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "seq_model.h"     /* seq_env_t */
#include "note_div.h"
#include "chord_types.h"   /* chord_type_t, chord_type_name() */
#include "quantizer.h"     /* quantizer_chord_intervals() */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Stutter house drone ──────────────────────────────────────────────────
 * A standalone drone synth, independent of the sequencer layers and the arp:
 * its own AMY synth slots (2/3, synth_slots.h), state and screen.
 *
 * Sound design (WAVE mode):
 *   osc0 = NOTE-following carrier, one voice per chord note, amplitude gated
 *          via mod_source by a continuously-running square LFO. Under the dB
 *          amp combine (voice_config.h), amp = const_sent * 10^(3*m*LFO) with
 *          LFO in {-1,+1}, giving peak_lin on-beat and
 *          peak_lin * 10^(-duck_db/20) off-beat. Knob mapping:
 *          drone_set_amp_peak / drone_set_amp_duck.
 *   osc1 = PULSE LFO at a tempo-locked division (the "stutter rate").
 *   LPF24 cutoff swept slowly, tempo-locked in bars.
 *   A single "sub" voice tracks the chord ROOT a fixed interval below.
 *
 * PATCH mode loads an AMY patch instead of the raw 2-osc voice, so the patch
 * owns its oscillators and amplitude: the stutter LFO and const/mod amp
 * controls are WAVE-only, while chord voicing, filter sweep and resonance
 * still apply.
 *
 * LFO frequency and sweep period both derive from the global BPM. */

typedef enum {
    DRONE_SRC_WAVE  = 0,
    DRONE_SRC_PATCH = 1,
} drone_source_t;

/* Chord shapes come from the shared quantizer_chord_intervals() table; the
 * drone caps voice count here regardless of how long the shared row is. */
#define DRONE_CHORD_MAX_NOTES 5

/* Stutter step patterns: 8-step on/off masks read by the filter-blip
 * service path. A 0 bit closes the filter for that step, turning the even
 * stutter grid into a rhythm. */
typedef enum {
    DRONE_PAT_FULL   = 0,   /* 1 1 1 1 1 1 1 1  every subdivision fires */
    DRONE_PAT_FOUR   = 1,   /* 1 0 1 0 1 0 1 0  four-on-floor           */
    DRONE_PAT_OFFBEAT= 2,   /* 0 1 0 1 0 1 0 1  offbeat skip            */
    DRONE_PAT_GALLOP = 3,   /* 1 1 0 1 1 0 1 0  galloping push          */
    DRONE_PAT_DUB    = 4,   /* 1 0 0 0 1 0 1 0  sparse dub stab         */
    DRONE_PAT_COUNT
} drone_pattern_t;

/* Chord progression follow, shared by both drones. Append-only: the stutter
 * drone's value persists in project snapshots.
 *   OFF   - root_note + own chord, the progression is ignored.
 *   ROOT  - own chord shape on the applied progression chord's root, placed at
 *           the pitch nearest root_note (-6..+5 semitones, kept in 24..72).
 *   CHORD - as ROOT, with the applied chord's type instead of the own chord.
 * With no progression chord applied (progression off or empty, or before its
 * first apply) ROOT and CHORD sound as OFF. A follow change lands on the bar
 * line the progression change is due on; notes the old and new voicing share
 * keep sounding without a retrigger. */
typedef enum {
    DRONE_FOLLOW_OFF = 0,
    DRONE_FOLLOW_ROOT,
    DRONE_FOLLOW_CHORD,
    DRONE_FOLLOW_COUNT
} drone_follow_t;

/* ── Lifecycle ── */
void drone_core_init(void);

/* Per-UI-frame service: drains the setters' pending rebuild and note changes
 * (the only place outside drone_core_follow_changed() that sends drone notes),
 * advances the tempo-locked filter sweep and keeps the LFO frequency in sync
 * with the current BPM. Call once per UI frame from synth_ui_task, like
 * arp_core_service(). */
void drone_core_service(void);

/* Re-voice the drone after the applied progression chord changed.
 * Obligations: synth_ui_task only (it reads
 * sequencer_core_progression_applied_chord()); called by the progression
 * service, after drone_core_service() has run in the same frame.
 * Guarantees: no-op under DRONE_FOLLOW_OFF; otherwise the note changes are sent
 * now, timed for the applied chord's land tick (immediately when that tick is
 * 0 or already past). */
void drone_core_follow_changed(void);

/* ── Parameter setters ──
 * Setters that change which notes sound (enable, solo mute, root, chord,
 * follow, sub on/off and interval) and those that rebuild the synths only
 * record: the sound change lands at the next drone_core_service() frame
 * (<= 50 ms). They send no notes, so they may be called from any task. */
void drone_set_enabled(bool on);          /* sustained note-on/off of the voices */

/* Silence the drone without disturbing drone_set_enabled()'s state: used by the
 * sequencer solo hook, so releasing solo restores whatever the user had set.
 * Unrelated to the stutter duck depth (drone_set_amp_duck). */
void drone_set_solo_muted(bool muted);

/* Silence the drone while the transport is stopped, the same way: used by the
 * transport hook (seq_transport_change_cb_t). Stop releases the notes through
 * the envelope; play brings the drone back as a fresh enable does. Starts
 * unpaused: the core's transport boots playing. */
void drone_set_paused(bool paused);
void drone_set_source(drone_source_t src);/* WAVE <-> PATCH (rebuilds the synths) */
void drone_set_wave(uint16_t amy_wave);   /* SAW_DOWN/SAW_UP/PULSE/TRIANGLE/SINE  */
void drone_set_chord(chord_type_t chord); /* chord preset the carrier plays       */
void drone_set_root_note(uint8_t midi_note); /* drone-local root (24..72); independent of global quantizer root */
void drone_set_follow(drone_follow_t f);  /* progression follow; invalid values ignored */
void drone_set_resonance(float r);        /* filter resonance                     */
/* Carrier amplitude - Peak/Duck dB model (WAVE mode only).
 *   PEAK 0..1 linear: the on-beat carrier level. Default 0.5.
 *   DUCK 0..1 linear: duck_db = duck * 40, how far the off-beat sits below
 *                     the on-beat. 0 = flat, 1 = -40 dB. Default 0.5.
 * The AMY coefs (m, const_sent) are derived internally - see the s_amp_*
 * helpers in drone_core.c; do not compute them at call sites. */
void drone_set_amp_peak(float c);         /* 0.0..1.0 on-beat level (linear)      */
void drone_set_amp_duck(float m);         /* 0.0..1.0 duck depth (0=flat,1=-40dB) */
void drone_set_rate(note_div_t rate);     /* stutter LFO note division            */
void drone_set_patch(uint16_t patch);     /* PATCH-mode preset                    */

/* The drone's opt-out from the shared patch catalog (patch_cycle.h): true for
 * ranges its excitation model can't play (NOISE, KS, bass presets, FM voices,
 * anything past its wavetable/raw-wave ceiling). Feeds both the cycling
 * domain's `excluded` predicate and drone_set_patch()'s snap-back. */
bool drone_patch_excluded(uint16_t patch);
void drone_set_sub_enabled(bool on);      /* sub drone on/off                     */
void drone_set_sub_interval(int8_t st);   /* sub interval, semitones (e.g. -12)   */
void drone_set_sweep_lo(float hz);        /* filter sweep low cutoff              */
void drone_set_sweep_hi(float hz);        /* filter sweep high cutoff             */
void drone_set_sweep_bars(uint8_t bars);  /* sweep period in bars (tempo-locked)  */
void drone_set_gate_len(float frac);      /* 0.05..0.95 chop length (osc1 duty)   */
void drone_set_swing(uint8_t pct);        /* 0..66 swing on the stutter grid      */
void drone_set_blip(float depth);         /* 0..1 per-step filter-zap depth       */
void drone_set_pattern(drone_pattern_t p);/* 8-step on/off mask                   */

/* ── Runtime-editable ADSR envelope (shared graph editor) ── */
void drone_get_envelope(seq_env_t *out);
void drone_set_envelope(const seq_env_t *env);

/* ── Second envelope (EG1, shared graph editor) ──
 * Independent breakpoint generator, timing-only: the drone's own WAVE-mode
 * patches wire no coef to it. Useful for a PATCH-mode drone whose loaded AMY
 * patch already routes its own bp1. */
void drone_get_envelope2(seq_env_t *out);
void drone_set_envelope2(const seq_env_t *env);

/* ── Editor live-preview (AMY only; drone store + authored flags untouched) ──
 * Audition scratch editor values on the drone's main (and, when enabled, sub)
 * synth. Cancel restores by calling these again with the stored values. */
void drone_preview_envelope(const seq_env_t *env);
void drone_preview_envelope2(const seq_env_t *env);

/* ── Whole voice-params block (project store) ──
 * The block with its authored flags, so a load restores an unauthored
 * envelope as unauthored. The set clamps like the per-block setters and
 * rebuilds the synths at the next drone_core_service() frame. */
void drone_get_voice_params(voice_params_t *out);
void drone_set_voice_params(const voice_params_t *vp);

/* ── Getters (for the list UI) ── */
bool           drone_get_enabled(void);
drone_source_t drone_get_source(void);
uint16_t       drone_get_wave(void);
chord_type_t   drone_get_chord(void);
uint8_t        drone_get_root_note(void);
drone_follow_t drone_get_follow(void);
float          drone_get_resonance(void);
float          drone_get_amp_peak(void);   /* 0..1 on-beat level knob value        */
float          drone_get_amp_duck(void);   /* 0..1 duck depth knob value           */
void           drone_set_amp_trim(float v);/* 0..1 per-target trim (default 1.0)   */
float          drone_get_amp_trim(void);   /* 0..1 per-target trim value           */

/* Visualiser helper: ON-beat (ceil) and OFF-beat (floor) amplitudes as 0..1
 * linear values, from the same dB math the engine uses. Either may be NULL. */
void           drone_get_amp_levels_norm(float *floor_norm, float *ceil_norm);
note_div_t     drone_get_rate(void);
uint16_t       drone_get_patch(void);
bool           drone_get_sub_enabled(void);
int8_t         drone_get_sub_interval(void);
float          drone_get_sweep_lo(void);
float          drone_get_sweep_hi(void);
uint8_t        drone_get_sweep_bars(void);
float          drone_get_gate_len(void);
uint8_t        drone_get_swing(void);
float          drone_get_blip(void);
drone_pattern_t drone_get_pattern(void);

/* Human-readable names for the enum values (for display). */
const char *drone_wave_name(uint16_t amy_wave);
const char *drone_chord_name(chord_type_t chord);   /* wraps chord_type_name() */
const char *drone_pattern_name(drone_pattern_t p);
const char *drone_follow_name(drone_follow_t f);    /* OFF / ROOT / CHORD */

#ifdef __cplusplus
}
#endif
