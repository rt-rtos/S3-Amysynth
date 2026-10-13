#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "seq_model.h"                  /* seq_env_t, voice_params_t */
#include "chord_types.h"                /* chord_type_t */
#include "custompatches/drone_core.h"   /* DRONE_CHORD_MAX_NOTES */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Shared drone plumbing ────────────────────────────────────────────────
 * What the stutter drone (drone_core) and the standard drone (drone_std_core)
 * do the same way: the note emitter with its sounding-set bookkeeping, chord
 * voicing, the envelope store, and the root and sub ranges.
 *
 * Execution context: the note functions send AMY events through amy_helpers,
 * so they run on a FreeRTOS task (the drones call them from synth_ui_task),
 * never an ISR or the render path, and never with amy_queue_lock held. The
 * envelope helpers and drone_chord_tones() only touch the memory they are
 * given. */

#define DRONE_ROOT_MIN          24    /* C1, lowest selectable drone root  */
#define DRONE_ROOT_MAX          72    /* C5, highest selectable drone root */
#define DRONE_SUB_MIDI_MIN      12    /* the sub note's MIDI range         */
#define DRONE_SUB_MIDI_MAX      108
#define DRONE_SUB_INTERVAL_MIN  (-36) /* sub interval, semitones below the root */
#define DRONE_SUB_INTERVAL_MAX  (-1)

/* Smallest amp CONST a drone writes. AMY skips a coef that is exactly 0
 * (amp_combine_controls), which would read as full level; one this small
 * hard-mutes instead. */
#define DRONE_AMP_MUTE 1e-6f

/* The main synth's voice count for `patch`: DRONE_CHORD_MAX_NOTES, cut to
 * what fits SEQ_TRACK_OSC_BUDGET - for a patch string through
 * sequencer_core_clamp_patch_voices(), for the FM range at FM_NUM_OPS + 1
 * oscs per voice and the additive range at ADD_MAX_PARTIALS + 1 (the custom
 * voice's partial count can grow without a drone rebuild). At least 1.
 * Logs a warning when a patch string clamps. Any task. */
uint8_t drone_voices_for(uint16_t patch);

/* The notes one drone has note-on'd and not yet released, so every note-off
 * names a sounding note, and the latest amy_event.time sent for a note. One
 * per drone, written only by that drone's note task. */
typedef struct {
    uint8_t  sounding[DRONE_CHORD_MAX_NOTES];
    uint8_t  sounding_n;
    int16_t  sounding_sub;    /* -1 = none */
    uint32_t last_sched_ms;
} drone_notes_t;

/* The voicing drone_apply_enabled() brings the sounding set to. */
typedef struct {
    uint8_t      main_synth;
    uint8_t      sub_synth;
    bool         sounding;      /* false: release everything            */
    uint8_t      root;          /* effective root (follow applied)      */
    chord_type_t chord;
    uint8_t      voices;        /* main voices built; the chord is truncated
                                   to this many tones (drone_chord_tones) */
    bool         sub;           /* a sub note at root + sub_interval    */
    int8_t       sub_interval;
} drone_voicing_t;

/* Empty sounding set. */
void drone_notes_init(drone_notes_t *n);

/* One note-on/off on `synth` (the instrument allocator picks a voice) at
 * land_ms, or at n->last_sched_ms if that is later: a follow change scheduled
 * for the bar line must not be overtaken by a later edit's notes, and AMY
 * keeps equal-time events in send order. Velocity 1 on, 0 off. */
void drone_note(drone_notes_t *n, uint8_t synth, bool on, float midi_note,
                uint32_t land_ms);

/* The main synth's notes for root + chord, at most `voices` of them (0 means
 * none fit, treated as 1): the root alone for an empty chord; when fewer
 * voices than chord tones fit, the root, then the fifth (interval 7) if the
 * chord has one, then the top tone, then the rest in chord order. Notes
 * clamped to 0..127. Returns the count written to out. */
uint8_t drone_chord_tones(uint8_t root, chord_type_t chord, uint8_t voices,
                          uint8_t out[DRONE_CHORD_MAX_NOTES]);

/* The single note emitter: bring n's sounding set to v - the chord tones on
 * main, one note at root + sub_interval (clamped to the sub MIDI range) on
 * the sub, nothing while !v->sounding. Notes in both sets are held, not
 * retriggered; releases go out before attacks. */
void drone_apply_enabled(drone_notes_t *n, const drone_voicing_t *v,
                         uint32_t land_ms);

/* Note-off the whole sounding set and forget it (ahead of a rebuild). */
void drone_release_all(drone_notes_t *n, uint8_t main_synth, uint8_t sub_synth,
                       uint32_t land_ms);

/* ── Envelope store (shared graph editor) ──
 * eg1 false = EG0 (vp->env), true = EG1 (vp->env1). Set copies env with the
 * VOICE_ENV_ATTACK_MIN_MS / VOICE_ENV_RELEASE_MIN_MS floors and marks the
 * envelope authored; returns false (vp untouched) for a NULL env. Pushing to
 * AMY stays with the drone, which knows its oscs. */
void drone_env_get(const voice_params_t *vp, bool eg1, seq_env_t *out);
bool drone_env_set(voice_params_t *vp, bool eg1, const seq_env_t *env);
/* Both envelopes' floors, for a whole voice-params load. */
void drone_env_floor_vp(voice_params_t *vp);

#ifdef __cplusplus
}
#endif
