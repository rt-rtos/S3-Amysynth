#pragma once

/* Shared voice-config layer: leaf utilities, canonical constants, the WAVE
 * voice skeleton and the native-LFO topology appliers used by every engine. */

#include "seq_model.h"     /* seq_env_t, lfo_wave_t */
#include <stdbool.h>
#include <stdint.h>

/* Canonical LFO-target depth scalars - single source of truth. Each is the
 * per-domain exchange rate for the SHARED depth%: anchored so equal depth
 * reads as equal musical intensity across targets. Applies to the native
 * (COEF_MOD) path AND the software-stepper pushes (sequencer/arp/live),
 * which must multiply the same scalar inside their powf(2, ...) term. */
#define VOICE_LFO_DEPTH_FILTER 3.0f
#define VOICE_LFO_DEPTH_AMP    0.5f
/* Pitch is log2: 1.0 = a full octave at 100% depth, which made even 5%
 * (+-60 cents) heavy vibrato. One semitone at 100% puts the whole musical
 * vibrato range on the knob - depth% ~= 1.2 cents/%. Octave-scale sweeps
 * are deliberately out of this LFO's reach (per-target field if ever
 * wanted, like the filter's octave-denominated swing). */
#define VOICE_LFO_DEPTH_PITCH  (1.0f / 12.0f)
#define VOICE_LFO_DEPTH_SCAN   0.5f
#define VOICE_LFO_DEPTH_PAN    0.5f  /* swing around the 0.5 center baseline */
/* DIST target (software stepper only - see voice_push_dist_lfo). Drive is
 * perceived roughly log like cutoff, so the swing is denominated in octaves
 * of pre-gain around the committed value: 100% depth = +/-2 oct (x1/4..x4).
 * Mix is bounded [0,1] and linear like pan: 100% depth = +/-0.5 swing. */
#define VOICE_LFO_DEPTH_DIST_OCT 2.0f
#define VOICE_LFO_DEPTH_DIST_MIX 0.5f

/* Anchor for software-LFO pitch pushes (every software stepper: sequencer
 * layers, arp, live voice). AMY's freq COEF_CONST is an absolute frequency in
 * Hz, converted via logfreq_of_freq(x) = log2(x / 440) (amy.c,
 * ZERO_LOGFREQ_IN_HZ). Multiplying the desired ratio by this base makes the
 * resulting logfreq constant the pure octave offset - 440 Hz itself is the
 * neutral push, identical to the reset default of 0. */
#define SEQ_LFO_PITCH_BASE_HZ 440.0f

/* FILTER-target swing ceiling, in quarter-octave steps (16 = 4.0 octaves). */
#define VOICE_LFO_FLT_OCT_Q_MAX 16u

/* Effective FILTER swing in octaves. AMY's filter_freq COEF_MOD rail is
 * log2-frequency, so this value IS the native coefficient; the software
 * stepper uses it as the 2^x exponent scale. Resolves the flt_oct_q legacy
 * sentinel (0 = depth% x VOICE_LFO_DEPTH_FILTER - the pre-octave law). */
static inline float voice_lfo_filter_octaves(const seq_lfo_t *l)
{
    uint8_t q = l->flt_oct_q;
    if (q == 0) return ((float)l->depth / 100.0f) * VOICE_LFO_DEPTH_FILTER;
    if (q > VOICE_LFO_FLT_OCT_Q_MAX) q = VOICE_LFO_FLT_OCT_Q_MAX;
    return 0.25f * (float)q;
}

/* WOBBLE (second-order LFO): osc2 chained as the osc1 carrier's mod_source.
 * AMP rides the dB combine (contribution is linear in the exponent: amp
 * multiplier = 10^(3 * coef * wob)) and is applied DOWNWARD-ONLY: the
 * carrier's amp CONST is pre-dropped by the swing (10^(-3*coef)) so the
 * breathing peaks exactly at the authored LFO depth and dips below it, never
 * above. Untamed, the +half of the swing multiplies the effective LFO depth
 * through the unclamped exponential MOD rail (2.8x at the former 0.15 coef)
 * and overdrives every target - the "too loud" wobble finding, 2026-08-03.
 * RATE is linear in log2-frequency: 1.0 => +/-1 octave rate swing. */
#define VOICE_WOB_DEPTH_AMP    0.075f
#define VOICE_WOB_DEPTH_RATE   1.0f

/* ── WOBBLE authoring unit ───────────────────────────────────────────────
 * Stored wob_depth is a 0..100 percentage of VOICE_WOB_DEPTH_AMP - meaningless
 * to the user, so authoring and display speak whole dB of TOTAL DIP below the
 * authored depth: the LFO breathes from full authored depth down to -N dB and
 * back (downward-only, see above).
 *
 * The AMP coefficient enters AMY's dB combine linearly (amp = 10^(3*coef*mod),
 * amp_combine_controls) and the carrier CONST is dropped by the same swing, so
 * total dip = 120 * coef dB with coef = wob/100 * VOICE_WOB_DEPTH_AMP: full
 * scale is 120 * VOICE_WOB_DEPTH_AMP = 9 dB in 1 dB steps.
 *
 * 0 dB is OFF, not a distinct setting: at zero swing the modulator is parked
 * (osc2 silenced), so nothing exists between "off" and "1 dB". The stored byte
 * keeps its 0..100 meaning, so old snapshots load unchanged.
 *
 * The same control also swings carrier RATE by up to +/-1 octave
 * (VOICE_WOB_DEPTH_RATE) when the reach includes rate; rate-only reach has no
 * depth breathing to measure, so its readout speaks octaves instead of dB. */
#define VOICE_WOB_DB_MAX  9u   /* = 120 * VOICE_WOB_DEPTH_AMP; keep in step */

/* Stored 0..100 -> whole dB of swing, rounded (0 => OFF). */
uint8_t voice_wob_depth_to_db(uint8_t wob_depth);

/* Whole dB of swing -> stored 0..100. Round-trips with the above at every
 * step, so repeated editing never drifts. */
uint8_t voice_wob_db_to_depth(uint8_t db);

/* ── Note-triggered envelope bounds ──────────────────────────────────────
 * The melodic sequencer, arp and graph editor all drive the same seq_env_t
 * into AMY breakpoint sets and need the same guard rails. Clamp against these
 * names, not respelled numbers, so UI/store/push path cannot drift apart.
 *
 * Scope: NOTE-TRIGGERED voices only. The drone engines keep their own floors -
 * their envelopes are free-running and open over hundreds of ms, so a shared
 * declick policy would encode a constraint they do not have.
 *
 * The graph editor derives its minimum marker spacing from ATTACK_MIN_MS (via
 * graph_popup_set_min_x_gap()) so the plot cannot enforce a stricter floor
 * than the engine.
 */

/* Below roughly one render block (5.33 ms at 48 kHz) an attack is
 * indistinguishable from a step, which clicks on percussive material. 2 ms
 * keeps the ramp inside a single block while still counting as a ramp. */
#define VOICE_ENV_ATTACK_MIN_MS   2u

/* Longer than the attack floor: a note-off lands on whatever the sustain stage
 * held, so the tail needs a few blocks to reach zero without a tick. */
#define VOICE_ENV_RELEASE_MIN_MS  5u

/* Ceiling for any single envelope segment. Far past the graph editor's longest
 * axis (15 s); exists to reject restored garbage, not to constrain authoring. */
#define VOICE_ENV_TIME_MAX_MS     60000u

/* Sustain is stored as a percentage of peak. */
#define VOICE_ENV_SUSTAIN_MAX_PCT 100u

/* Reset a voice_params_t (seq_model.h) to defaults: zeroed/unauthored, with
 * amp_trim at unity. The single place the trim gets its non-zero default -
 * use it instead of re-setting 1.0 after a memset. */
void voice_params_init_defaults(voice_params_t *vp);

/* lfo_wave_t -> AMY wave constant. RANDOM maps to NOISE: compute_mod_noise
 * is a native sample-and-hold (redraws only when the carrier phasor wraps),
 * replacing the 20 Hz software lfo_next_rand() poll. */
uint16_t voice_lfo_wave_to_amy(lfo_wave_t wave);

/* ── Shared WAVE-voice skeleton ──────────────────────────────────────────
 * The canonical "N voices, osc0 = note-following carrier" build used by the
 * arp, drone and melodic sequencer. Sends the pool definition, the audible
 * skeleton, and a park (amp CONST 0, MOD rails cleared) for every osc above
 * the audible layout up to oscs_per_voice. Guarantee: a reserved osc never
 * sounds until its owner configures it - a note with no osc named reaches
 * every osc of the voice, and an unconfigured osc plays at AMY's reset
 * defaults (a full-level SINE on the note). Deliberately does NOT touch
 * envelopes, filters or mod routing - each engine layers its own
 * specialization on top as deltas, after this call.
 *
 * AMY resets every osc of every existing voice on each pool definition
 * (patches_load_patch), unchanged shape or not, so a rebuild cuts held voices
 * and the caller re-sends whatever it layered on top. */
typedef struct {
    uint8_t  synth;
    uint8_t  num_voices;
    uint8_t  oscs_per_voice;       /* 3 when oscs 1-2 are reserved as the LFO pair */
    uint16_t wave;                 /* AMY wave constant for osc0           */
    float    osc0_amp_const;       /* arp/melodic 1.0; drone const_sent    */
    float    osc0_amp_vel;         /* arp/melodic 1.0; drone 0.0           */
    bool     ks_feedback_authored; /* authored feedback drives KS decay    */
    float    ks_feedback;          /* seq_filter_t.feedback (0..1) when authored;
                                      <= 0 or unauthored -> fixed 0.9 default */
    float    ks_duty_ofs;          /* seq_filter_t.ks_duty_ofs: KS pluck duty
                                      = 0.5 + this; 0 = AMY's default burst */
    int16_t  wt_preset;            /* >=0 => e->preset (WAVETABLE); else -1 */
    const seq_dist_t *dist;        /* re-pushed on every build so a rebuild
                                      never drops the stage; NULL = leave the
                                      synth's distortion untouched            */
    const voice_unison_t *unison;  /* copy-fan spec (seq_model.h); NULL or
                                      count <= 1 = the single-osc build. The
                                      caller passes the EFFECTIVE count: it
                                      has sized oscs_per_voice for count
                                      copies plus any reserved pair, excluded
                                      KS and clamped against the osc budget.
                                      `layout` picks the layout; size
                                      oscs_per_voice with
                                      voice_unison_oscs_per_voice()          */
} voice_wave_cfg_t;

/* ── Unison index map ────────────────────────────────────────────────────
 * Shared by the build, the live push and the sequencer's layout query, so the
 * pool shape and the osc masks cannot disagree. n = EFFECTIVE copy count
 * (>= 2 and even when headed or engine; n <= 1 is the single-osc build and
 * reads as the fan). Copies are numbered i = 0..n-1 by pitch position (i = 0
 * most flat); mirror pair p = min(i, n-1-i), p = 0 outermost. Headed: pair p
 * sits in group L when p is even, else R; copy i goes to the group of pair p
 * if i == p, the other group otherwise, and within a group the copies occupy
 * ascending osc indices in ascending i order. Engine: the same interleaving
 * (even i in L, odd in R) but a group is one AMY unison-cluster osc - L is
 * osc 0, R is osc 1 - so copy_osc returns the group osc and the "heads" are
 * the two cluster oscs. Fan: copy i is osc i. `layout` is a
 * voice_unison_layout_t (seq_model.h). */
uint8_t voice_unison_oscs_per_voice(uint8_t n, uint8_t layout); /* n+2 / n+4 / 4 */
uint8_t voice_unison_copy_osc(uint8_t i, uint8_t n, uint8_t layout);
uint8_t voice_unison_head_osc(uint8_t group, uint8_t n, uint8_t layout); /* 0: L, 1: R */
uint8_t voice_unison_copies_mask(uint8_t n, uint8_t layout);    /* bit = osc */
uint8_t voice_unison_heads_mask(uint8_t n, uint8_t layout);     /* 0 when fan */

/* Core-0 / UI-task only; pushes through amy_helpers (never amy_queue_lock). */
void voice_build_wave(const voice_wave_cfg_t *cfg);

/* Re-send only the per-copy unison fields (freq/pan/amp CONST, trigger_phase)
 * to a synth built by voice_build_wave() with this same count - a live
 * detune/spread/blend turn without a rebuild (mirrors fm_voice_push_live).
 * On the headed layout the copies take detune and blend and the two heads take
 * the spread on their pan CONST; on the engine layout each cluster osc takes
 * its unison fields, group gain and pan. A count or layout change needs a
 * rebuild instead (the pool shape moves). base_amp = the build's
 * osc0_amp_const (the heads carry it on the headed layout, so the copies
 * ignore it). No-op when count <= 1. Known interaction: an
 * LFO PAN target writes pan CONST 0.5 on the coupled oscs and flattens the
 * spread until the next push. Core-0 / UI-task only. */
void voice_push_unison_live(uint8_t synth, const voice_unison_t *u,
                            float base_amp);

/* ── Per-voice distortion (AMY DIST_*) ───────────────────────────────────
 * The distortion block is owned by the caller's voice_params_t (seq_model.h),
 * exactly like the filter and the LFO: melodic layers hold one per track, the
 * arp / both drones / live-play hold one each. There is no global or
 * per-domain set - a synth's distortion is whatever its owner last pushed.
 *
 * Reach: the event addresses osc 0, which AMY resolves to the BASE osc of every
 * voice in the synth. Wave voices and ALGO/FM voices (whose osc 0 carries the
 * summed operator output) are fully covered; multi-osc patch strings distort
 * their base osc only. That is deliberate - see voice_apply_dist.
 *
 * Applies to any synth the caller names, patch-backed or wave-backed. If a
 * future patch layout needs excluding, gate at the call sites the way the LFO
 * editor gates WOBBLE on sequencer_core_lfo_native_layout(); nothing here
 * inspects the target's topology. */

/* Clamp a block to the documented field ranges, in place. Callers that accept
 * values from the wire, a snapshot, or the UI run this before storing. */
void voice_dist_clamp(seq_dist_t *d);

/* Push `d` to `synth` (base osc of each voice); type OFF actively disables the
 * stage rather than leaving the last setting running. Clamps a copy, so the
 * caller's block is untouched. NULL `d`: no-op. Core-0 / UI-task only. */
void voice_apply_dist(uint8_t synth, const seq_dist_t *d);

/* Same as voice_apply_dist, addressed to one voice-relative osc instead of
 * the base osc - the SILENT-head layout carries the stage on each head. */
void voice_apply_dist_osc(uint8_t synth, uint8_t osc, const seq_dist_t *d);

/* One distortion-target stepper tick, the PATCH-mode fallback. Computes the
 * swept drive and/or mix - whichever of LFO_TARGET_DIST_DRIVE / DIST_MIX the
 * caller checked in lfo->targets - around the committed `base` block, and
 * writes ONLY the drive/mix coefs' CONST term (type/bits/rate stay whatever the
 * dist editor last applied). Drive and mix have COEF_MOD rails in AMY now, so
 * native-carrier tracks drive distortion through voice_apply_native_lfo_topo()
 * instead; only PATCH-mode tracks (no free carrier osc) reach it through this
 * stepper. The law here matches the native rail exactly. No-op when the shaper
 * is OFF - an inert target, not an implicit enable. Core-0 / UI-task only. */
void voice_push_dist_lfo(uint8_t synth, const seq_dist_t *base,
                         const seq_lfo_t *lfo, float val);

/* Park voice-relative oscs [first, end) of every voice of `synth`: amp CONST
 * 0 (render skips the osc) and its amp/freq MOD rails cleared. For builders
 * outside voice_build_wave that reserve oscs (the bass presets' LFO pair);
 * voice_build_wave parks its own. Send after the pool definition, before
 * any configuration of those oscs. Core-0 / UI-task only. */
void voice_park_oscs(uint8_t synth, uint8_t first, uint8_t end);

/* Apply the AMY-native mod-source LFO routing for one synth built by
 * voice_build_wave() with oscs_per_voice=3: osc0 gets mod_source=1 plus the
 * selected target's COEF_MOD depth, osc1 becomes the LFO carrier at the
 * BPM-synced rate. Pass lfo=NULL (or a disabled lfo) to deactivate: the mod
 * coupling is cleared and the carrier silenced.
 *
 * Idempotent and state-clean: every target's COEF_MOD is cleared before the
 * selected one is set, because re-sending the same voice count does not reset
 * the osc pool - a prior target's coef would persist and keep modulating (the
 * stale-AMP case rides AMY's convex dB combine and ramps to a DC rail).
 *
 * Core-0 / UI-task only; pushes through amy_helpers (never amy_queue_lock). */
void voice_apply_native_lfo(uint8_t synth, const seq_lfo_t *lfo, uint16_t bpm);

/* Topology-parameterized applier behind voice_apply_native_lfo (the wave-build
 * special case: carrier 1, both masks 0x01). carrier_osc = voice-relative index
 * of the reserved LFO carrier (wobble = carrier+1). Two masks select where each
 * target's COEF_MOD lands (bit n = osc n): pitch_mask takes the PITCH and SCAN
 * rails, voice_mask the FILTER, AMP, PAN and DIST rails. They are the same mask
 * on a flat layout; the SILENT-head layout splits them, since the copies carry
 * the pitch and the heads carry the per-voice stages. Every osc in either mask
 * gets mod_source and a full sibling clear. Derive the masks from
 * seq_track_voice_layout() / sequencer_core_lfo_native_layout(), never hardcode
 * osc indices. */
void voice_apply_native_lfo_topo(uint8_t synth, const seq_lfo_t *lfo,
                                 uint16_t bpm, uint8_t carrier_osc,
                                 uint8_t pitch_mask, uint8_t voice_mask);
