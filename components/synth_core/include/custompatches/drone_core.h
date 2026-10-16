#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "seq_model.h"     /* seq_env_t, seq_filter_t, seq_lfo_t, seq_dist_t */
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
 * Sound design:
 *   - The main synth plays one SEQ_PATCH_* id from the unified patch list
 *     (drone_stutter_patch_excluded() domain), applied through the shared
 *     apply path at the voice count the patch's osc budget allows, up to
 *     DRONE_CHORD_MAX_NOTES; a chord with more tones than voices is truncated
 *     (drone_chord_tones).
 *   - The stutter gate: amp CONST writes scheduled on the sequencer tick
 *     grid at the stutter rate, timed ahead through amy_event.time - the on
 *     level peak_lin at each open step (odd steps swung), the off level
 *     peak_lin * 10^(-duck_db/20) gate-length later. They go to the oscs of
 *     the patch's LFO target map voice mask (osc 0 of a wave voice, the Juno
 *     SILENT head, the DX7/FM ALGO osc), replacing the head's own level.
 *     Knob mapping: drone_set_amp_peak / drone_set_amp_duck.
 *   - The standard per-voice toolset on the main synth, re-imposed after
 *     every rebuild where authored: a filter (seq_filter_t) with its
 *     envelope routing matrix on the same oscs, a free LFO (seq_lfo_t: a
 *     shared LFO synth, lfo_bank.h, over the same target map; none when the
 *     bank is full; without CONFIG_SEQ_LFO_SHARED the native carriers on
 *     wave patches only), the ADSR/EG1 graph storage and distortion. A fresh
 *     drone's filter is an authored LPF24 swept by an authored 4-bar sine
 *     LFO. The per-step blip writes the filter's cutoff CONST around the
 *     filter's own cutoff.
 *   - A one-voice sine "sub" tracks the chord ROOT a fixed interval below,
 *     gated with the main; no filter.
 *
 * The gate rate derives from the global BPM. */

/* Chord shapes come from the shared quantizer_chord_intervals() table; the
 * drone caps voice count here regardless of how long the shared row is. */
#define DRONE_CHORD_MAX_NOTES 5

/* Stutter step patterns: 8-step on/off masks read by the gate scheduler. A
 * 0 bit skips that step's gate opening (it stays at the duck level), turning
 * the even stutter grid into a rhythm; the blip fires on open steps only. */
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

/* The stutter drone's persisted state (the project's DRON section): the
 * drone_get_* / drone_set_* values. */
typedef struct {
    bool            enabled;
    chord_type_t    chord;
    uint8_t         root;
    uint16_t        patch;
    float           amp_peak;
    float           amp_duck;
    note_div_t      rate;
    bool            sub_enabled;
    int8_t          sub_interval;
    float           gate_len;
    uint8_t         swing;
    float           blip;
    drone_pattern_t pattern;
    drone_follow_t  follow;
    voice_params_t  vp;
} drone_params_t;

/* Fallback values of the project format for the stutter drone: off, root A2
 * alone, SAW_DOWN at 1/16 with the sub an octave down, a full gate pattern,
 * and the authored LPF24 sweep. Pure; any task; fills every field. Never
 * reads Kconfig; drone_core_init() seeds the boot drone from it (no Kconfig
 * overlay). */
void drone_params_defaults(drone_params_t *out);

/* ── Lifecycle ── */
void drone_core_init(void);

/* Per-UI-frame service: drains the setters' pending rebuild and note changes
 * (the only place outside drone_core_follow_changed() that sends drone notes),
 * schedules the gate and writes the blip. Call once per UI frame from
 * synth_ui_task, like arp_core_service(). */
void drone_core_service(void);

/* Re-push the native LFO carrier frequency after a BPM change (called from
 * sequencer_core_set_bpm, like drone_std_core_refresh_lfo_freq). A no-op with
 * CONFIG_SEQ_LFO_SHARED (no per-voice carriers). UI task. */
void drone_core_refresh_lfo_freq(void);

/* Live custom-voice edits: re-push s_fm_voice (FM_PUSH_* scope or operator
 * index `what`) / s_additive_voice to the main synth when the drone plays
 * SEQ_PATCH_FM_CUSTOM / SEQ_PATCH_ADDITIVE_CUSTOM; no-op otherwise. No
 * rebuild, so held notes keep sounding. Called from
 * sequencer_core_fm_voice_changed() / sequencer_core_additive_voice_changed(),
 * like the arp's hooks. UI task. */
void drone_core_fm_voice_changed(uint8_t what);
void drone_core_additive_voice_changed(void);

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
void drone_set_chord(chord_type_t chord); /* chord preset the carrier plays       */
void drone_set_root_note(uint8_t midi_note); /* drone-local root (24..72); independent of global quantizer root */
void drone_set_follow(drone_follow_t f);  /* progression follow; invalid values ignored */
/* Gate levels - Peak/Duck dB model.
 *   PEAK 0..1 linear: the on-beat carrier level. Default 0.5.
 *   DUCK 0..1 linear: duck_db = duck * 40, how far the off-beat sits below
 *                     the on-beat. 0 = flat, 1 = -40 dB. Default 0.5.
 * The AMY coefs (m, const_sent) are derived internally - see the s_amp_*
 * helpers in drone_core.c; do not compute them at call sites. */
void drone_set_amp_peak(float c);         /* 0.0..1.0 on-beat level (linear)      */
void drone_set_amp_duck(float m);         /* 0.0..1.0 duck depth (0=flat,1=-40dB) */
void drone_set_rate(note_div_t rate);     /* stutter grid note division           */
void drone_set_patch(uint16_t patch);     /* excluded ids -> TRIANGLE              */

/* The drones' opt-out from the shared patch catalog (patch_cycle.h): true for
 * the ranges compiled out of this build (sequencer_core_patch_compiled_out())
 * and for NOISE and KS, which do not hold as a chord. Raw waves, wavetables,
 * Juno, DX7, the piano, bass, FM and additive play. The normal drone's
 * cycling domain and both drones' snap-back. Pure; any task. */
bool drone_patch_excluded(uint16_t patch);
/* The stutter drone's own domain: drone_patch_excluded(), plus every patch
 * with no osc map for the gate (the piano). Feeds the stutter screen's
 * cycling domain, drone_set_patch()'s snap-back and the project loader. Pure;
 * any task. */
bool drone_stutter_patch_excluded(uint16_t patch);
void drone_set_sub_enabled(bool on);      /* sub drone on/off                     */
void drone_set_sub_interval(int8_t st);   /* sub interval, semitones (e.g. -12)   */
void drone_set_gate_len(float frac);      /* 0.05..0.95 open time, fraction of a step */
void drone_set_swing(uint8_t pct);        /* 0..66 swing on the stutter grid      */
void drone_set_blip(float depth);         /* 0..1 per-step cutoff-zap depth       */
void drone_set_pattern(drone_pattern_t p);/* 8-step on/off mask                   */

/* ── Runtime-editable ADSR envelope (shared graph editor) ── */
void drone_get_envelope(seq_env_t *out);
void drone_set_envelope(const seq_env_t *env);

/* ── Second envelope (EG1, shared graph editor) ──
 * Breakpoint generator for the filter editor's envelope routing, main synth
 * only. */
void drone_get_envelope2(seq_env_t *out);
void drone_set_envelope2(const seq_env_t *env);

/* ── Editor live-preview (AMY only; drone store + authored flags untouched) ──
 * Audition scratch editor values on the drone's main synth (EG0 also on the
 * sub, when enabled). Cancel restores by calling these again with the stored values. */
void drone_preview_envelope(const seq_env_t *env);
void drone_preview_envelope2(const seq_env_t *env);

/* ── Filter (filter editor) ──
 * Pushed to main's gate oscs with its envelope routing; the sub has none.
 * set stores and marks authored; preview pushes only. UI task. */
void drone_get_filter(seq_filter_t *out);
void drone_set_filter(const seq_filter_t *f);
void drone_preview_filter(const seq_filter_t *f);  /* AMY only, store untouched */

/* ── Distortion (shared distortion editor) ──
 * Main only. reapply re-asserts after a rebuild, which clears the per-osc
 * stage. UI task. */
void drone_get_dist(seq_dist_t *out);
void drone_set_dist(const seq_dist_t *d);
void drone_preview_dist(const seq_dist_t *d);  /* AMY only, store untouched */
void drone_reapply_dist(void);

/* ── Free LFO (shared LFO editor) ── Main only. UI task. */
void drone_get_lfo(seq_lfo_t *out);
void drone_set_lfo(const seq_lfo_t *lfo);
/* True while the main synth's LFO runs on a shared LFO synth - the path on
 * which WOBBLE exists. Always false without CONFIG_SEQ_LFO_SHARED. UI task. */
bool drone_lfo_bound(void);
/* LFO2, main only: the arp's contract (arp_core.h, arp_set_lfo2), except that
 * the collapse which hands slot 0 back to the patch rebuilds at the next
 * drone_core_service() frame. UI task. */
bool drone_get_lfo2(seq_lfo_t *out);
void drone_set_lfo2(const seq_lfo_t *lfo, bool open);
bool drone_lfo2_bound(void);

/* ── Whole voice-params block (project store) ──
 * The block with its authored flags, so a load restores an unauthored
 * envelope as unauthored. The set clamps like the per-block setters and
 * rebuilds the synths at the next drone_core_service() frame. */
void drone_get_voice_params(voice_params_t *out);
void drone_set_voice_params(const voice_params_t *vp);

/* ── Getters (for the list UI) ── */
bool           drone_get_enabled(void);
chord_type_t   drone_get_chord(void);
uint8_t        drone_get_root_note(void);
drone_follow_t drone_get_follow(void);
float          drone_get_amp_peak(void);   /* 0..1 on-beat level knob value        */
float          drone_get_amp_duck(void);   /* 0..1 duck depth knob value           */
void           drone_set_amp_trim(float v);/* 0..1 per-target trim (default 1.0)   */
float          drone_get_amp_trim(void);   /* 0..1 per-target trim value           */

note_div_t     drone_get_rate(void);
uint16_t       drone_get_patch(void);
bool           drone_get_sub_enabled(void);
int8_t         drone_get_sub_interval(void);
float          drone_get_gate_len(void);
uint8_t        drone_get_swing(void);
float          drone_get_blip(void);
drone_pattern_t drone_get_pattern(void);

/* Human-readable names for the enum values (for display). */
const char *drone_chord_name(chord_type_t chord);   /* wraps chord_type_name() */
const char *drone_pattern_name(drone_pattern_t p);
/* A pattern's 8 steps as a mask (bit n = step n open); 0 if out of range. */
uint8_t     drone_pattern_mask(drone_pattern_t p);
const char *drone_follow_name(drone_follow_t f);    /* OFF / ROOT / CHORD */

#ifdef __cplusplus
}
#endif
