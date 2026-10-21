#pragma once

/* Shared voice-config layer: leaf utilities, canonical constants, the WAVE
 * voice skeleton, the LFO source pair and slot-1 wiring, and (without
 * CONFIG_SEQ_LFO_SHARED) the native-LFO topology appliers. */

#include "sdkconfig.h"
#include "seq_model.h"     /* seq_env_t, lfo_wave_t */
#include <stdbool.h>
#include <stdint.h>

/* Canonical LFO-target depth scalars - single source of truth. Each is the
 * per-domain exchange rate for the SHARED depth%: anchored so equal depth
 * reads as equal musical intensity across targets. Applies to the native
 * (COEF_MOD) path AND the software-stepper pushes (sequencer/arp/live),
 * which must multiply the same scalar inside their powf(2, ...) term. */
#define VOICE_LFO_DEPTH_FILTER 3.0f
/* Amp is AMY's dB combine (amp_combine_controls, amy.c): a COEF_MOD weight w
 * on a +-1 carrier is a gain swing of 10^(+-3w), symmetric about the authored
 * level, so the loud half-cycle overshoots the unmodulated note. The shared
 * wire keeps the dip and removes the overshoot by pre-dropping the synth's
 * level (the LFO cap factor of amy_event.synth_level, owned by synth_level.h)
 * by the same 10^(-3w): the swing then runs from 10^(-6w) up to the unmodulated
 * level, a 30 dB tremolo at 100 % depth that is never louder than the note.
 * The software stepper's AMP (synth_level_set_sw_lfo) dips linearly from
 * unity to 1 - depth. */
#define VOICE_LFO_DEPTH_AMP    0.25f
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

/* Anchor for every pitch push through freq COEF_CONST: software-LFO steppers
 * (sequencer layers, arp, live voice), unison detune, bass-preset offsets.
 * AMY takes that constant as an absolute frequency in Hz and stores it as
 * log2(f / 440) (AMY docs/synth.md, Control Coefficients), so a desired ratio
 * r is sent as SEQ_LFO_PITCH_BASE_HZ * r; a bare ratio lands about 8.8 octaves
 * down and mutes the voice. 440 Hz itself is the neutral push, identical to
 * the reset default of 0. */
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
 * AMY's amp combine is in dB (AMY docs/synth.md, Control Coefficients): each
 * coefficient's contribution is linear in the exponent, amp multiplier =
 * 10^(3 * coef * x). Wobble AMP therefore applies DOWNWARD-ONLY: the carrier's
 * amp CONST is pre-dropped by the swing (10^(-3*coef)), so the breathing peaks
 * exactly at the authored LFO depth and dips below it. The +half of the swing
 * would otherwise multiply the effective LFO depth through the unclamped
 * exponential MOD rail and overdrive every target.
 * RATE is linear in log2-frequency: 1.0 => +/-1 octave rate swing. */
#define VOICE_WOB_DEPTH_AMP    0.075f
#define VOICE_WOB_DEPTH_RATE   1.0f

/* ── WOBBLE authoring unit ───────────────────────────────────────────────
 * Stored wob_depth is a 0..100 percentage of VOICE_WOB_DEPTH_AMP - meaningless
 * to the user, so authoring and display speak whole dB of TOTAL DIP below the
 * authored depth: the LFO breathes from full authored depth down to -N dB and
 * back (downward-only, see above).
 *
 * With the dB combine above, total dip = 120 * coef dB with
 * coef = wob/100 * VOICE_WOB_DEPTH_AMP: full scale is
 * 120 * VOICE_WOB_DEPTH_AMP = 9 dB in 1 dB steps.
 *
 * 0 dB is OFF, not a distinct setting: at zero swing the modulator is parked
 * (osc2 silenced), so nothing exists between "off" and "1 dB".
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
 * The melodic sequencer, arp and ADSR editor all drive the same seq_env_t
 * into AMY breakpoint sets and need the same guard rails. Clamp against these
 * names, not respelled numbers, so UI/store/push path cannot drift apart.
 *
 * Scope: NOTE-TRIGGERED voices only. The drone engines keep their own floors -
 * their envelopes are free-running and open over hundreds of ms, so a shared
 * declick policy would encode a constraint they do not have.
 *
 * The ADSR editor derives its minimum marker spacing from ATTACK_MIN_MS (via
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

/* Ceiling for any single envelope segment. Far past the ADSR editor's longest
 * axis (15 s); exists to reject restored garbage, not to constrain authoring. */
#define VOICE_ENV_TIME_MAX_MS     60000u

/* Sustain is stored as a percentage of peak. */
#define VOICE_ENV_SUSTAIN_MAX_PCT 100u

/* KS string feedback when the row has never authored one (seq_filter_t.feedback
 * at its 0 sentinel): the voice configure sends it, and the filter editor opens
 * its FB readout on it. The one home of the value. 0.998 rings for seconds at
 * A3; 0.9 died in about 0.3 s, so a row switched to KS sounded muted. AMY's own
 * default is 0 and 1.0 is lossless; above 1 the KS buffer diverges. */
#define VOICE_KS_FEEDBACK_DEFAULT 0.998f

/* Reset a voice_params_t (seq_model.h) to defaults: zeroed/unauthored, with
 * amp_trim at unity, LFO1's rate and wobble rate at 1/8, and LFO2 at the LFO
 * editor's seed (sine, 1/1, depth 50) with enabled false and no targets. The
 * single place those get their non-zero defaults - use it instead of
 * re-setting them after a memset. */
void voice_params_init_defaults(voice_params_t *vp);

/* lfo_wave_t -> AMY wave constant. RANDOM maps to NOISE: compute_mod_noise
 * is a native sample-and-hold (redraws only when the carrier phasor wraps),
 * replacing the 20 Hz software lfo_next_rand() poll. */
uint16_t voice_lfo_wave_to_amy(lfo_wave_t wave);

/* Oscs a wave-topology voice (sequencer_core_lfo_wave_layout()) reserves
 * above its audible layout for a per-voice LFO carrier and wobble: none with
 * CONFIG_SEQ_LFO_SHARED (the LFO is a shared LFO synth), the pair otherwise. */
#if CONFIG_SEQ_LFO_SHARED
#define VOICE_LFO_PAIR_OSCS 0u
#else
#define VOICE_LFO_PAIR_OSCS 2u
#endif

/* ── Shared WAVE-voice skeleton ──────────────────────────────────────────
 * The canonical "N voices, osc0 = note-following carrier" build behind every
 * wave patch on the shared apply path (melodic rows, arp, live, drones). Sends the pool definition, the audible
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
    uint8_t  oscs_per_voice;       /* audible oscs + any reserved above them
                                      (VOICE_LFO_PAIR_OSCS)                   */
    uint16_t wave;                 /* AMY wave constant for osc0           */
    float    osc0_amp_const;       /* the shared apply path: 1.0           */
    float    osc0_amp_vel;         /* the shared apply path: 1.0           */
    bool     ks_feedback_authored; /* authored feedback drives KS decay    */
    float    ks_feedback;          /* seq_filter_t.feedback (0..1) when authored;
                                      <= 0 or unauthored -> VOICE_KS_FEEDBACK_DEFAULT */
    float    ks_duty_ofs;          /* seq_filter_t.ks_duty_ofs: KS pluck duty
                                      = 0.5 + this; 0 = AMY's default burst */
    int16_t  wt_preset;            /* >=0 => e->preset (WAVETABLE); else -1 */
    const seq_dist_t *dist;        /* re-pushed on every build so a rebuild
                                      never drops the stage; NULL = leave the
                                      synth's distortion untouched            */
    const voice_unison_t *unison;  /* unison spec (seq_model.h); NULL,
                                      bypass or count <= 1 = the single-osc
                                      build. The
                                      caller passes the EFFECTIVE count: it
                                      has sized oscs_per_voice for count
                                      copies plus any reserved pair, excluded
                                      KS and clamped against the osc budget.
                                      `stereo` picks one cluster osc or the
                                      L/R pair; size oscs_per_voice with
                                      voice_unison_oscs_per_voice()          */
} voice_wave_cfg_t;

/* ── Unison index map ────────────────────────────────────────────────────
 * Shared by the build, the live push and the sequencer's layout query, so the
 * pool shape and the osc masks cannot disagree. n = EFFECTIVE copy count;
 * n <= 1 is the single-osc build. The stack deals the copies over G groups
 * (voice_unison_groups(stack); G is cut to n), m = n / G each, a remainder
 * unrendered; copies are numbered i = 0..n-1 group by group, by pitch
 * position inside the group (j = i mod m, j = 0 most flat), and render
 * inside AMY unison-cluster oscs. Stereo off (or n <= 1): every copy is in
 * osc 0. Stereo on with n >= 2 and m >= 2: even j in side L, odd j in R (an
 * odd m puts the extra copy of every group in L); L is osc 0, R is osc 1, so
 * copy_osc returns the side osc and the "heads" (the oscs carrying envelope,
 * filter, dist and pan) are the cluster oscs themselves. Stereo on with
 * m = 1 keeps the two-osc pool but plays every copy centred on osc 0; osc 1
 * is built silent. Pure; any task. */
/* Pool shape: 2 for a stereo pair (stereo, n >= 2), else 1; plus
 * VOICE_LFO_PAIR_OSCS. */
uint8_t voice_unison_oscs_per_voice(uint8_t n, bool stereo);
uint8_t voice_unison_groups(uint8_t stack);                  /* notes in a voice_unison_stack_t */
/* The row defaults every unison owner seeds with and the store falls back to:
 * Off, Oct+Sub stack at count 3, detune 0, spread 50, blend 100, stereo on,
 * shift centred. Plain constants (project store rule); pure, any task. */
voice_unison_t voice_unison_defaults(void);
uint8_t voice_unison_copy_osc(const voice_unison_t *u, uint8_t i, uint8_t n);
uint8_t voice_unison_head_osc(uint8_t group, uint8_t n, bool stereo); /* 0: L, 1: R */
uint8_t voice_unison_copies_mask(uint8_t n, bool stereo);    /* bit = osc */
uint8_t voice_unison_heads_mask(uint8_t n, bool stereo);     /* 0 when n <= 1 */

/* The factor the build and the live push multiply base_amp by for the amp
 * CONST of cluster osc `group` (0: L, 1: R) under n effective copies: 1.0 for
 * stereo off or n < 2, else the pair's share of the centred cluster's level.
 * For a caller that writes amp CONST itself on a unison synth (the stutter
 * gate): writing level * this per head keeps the pair's L/R balance. Pure,
 * no AMY access; any task, render-path safe. */
float voice_unison_osc_level(const voice_unison_t *u, uint8_t group, uint8_t n);

/* Core-0 / UI-task only; pushes through amy_helpers (never amy_queue_lock). */
void voice_build_wave(const voice_wave_cfg_t *cfg);

/* Re-send only the unison fields (unison count/spacing/offset/blend, groups
 * and intervals, pan and
 * amp CONST, and on a stereo pair trigger_phase) to the cluster oscs of a
 * synth built by voice_build_wave() with this same count and stereo flag - a
 * live detune/stack/shift/spread/blend turn without a rebuild (mirrors
 * fm_voice_push_live). A bypass, count or stereo change needs a rebuild
 * instead (the pool shape moves). base_amp = the build's osc0_amp_const.
 * No-op under bypass or when count <= 1. Known interaction: an LFO PAN target writes pan CONST 0.5 on
 * the coupled oscs and flattens the spread until the next push. Core-0 /
 * UI-task only. */
void voice_push_unison_live(uint8_t synth, const voice_unison_t *u,
                            float base_amp);

/* ── Per-voice distortion (AMY DIST_*) ───────────────────────────────────
 * The distortion block is owned by the caller's voice_params_t (seq_model.h),
 * exactly like the filter and the LFO: melodic layers hold one per track, the
 * arp / both drones / live-play hold one each. There is no global or
 * per-domain set - a synth's distortion is whatever its owner last pushed.
 *
 * Reach: an event naming osc N (voice-relative) reaches osc N of every voice
 * in the synth; osc 0 is the BASE osc. Wave voices and ALGO/FM voices (whose
 * osc 0 carries the summed operator output) are fully covered; multi-osc patch
 * strings distort their base osc only. That is deliberate - see
 * voice_apply_dist.
 *
 * Applies to any synth the caller names, patch-backed or wave-backed. If a
 * future patch layout needs excluding, gate at the call sites the way the LFO
 * editor gates WOBBLE on the consumer's LFO path; nothing here inspects the
 * target's topology. */

/* Clamp a block to the documented field ranges, in place. Callers that accept
 * values from the wire, a snapshot, or the UI run this before storing. */
void voice_dist_clamp(seq_dist_t *d);

/* Push `d` to `synth` (base osc of each voice); type OFF actively disables the
 * stage rather than leaving the last setting running. Clamps a copy, so the
 * caller's block is untouched. NULL `d`: no-op. Core-0 / UI-task only. */
void voice_apply_dist(uint8_t synth, const seq_dist_t *d);

/* Same as voice_apply_dist, addressed to one voice-relative osc instead of
 * the base osc - a unison voice carries the stage on each cluster osc. */
void voice_apply_dist_osc(uint8_t synth, uint8_t osc, const seq_dist_t *d);

/* One distortion-target stepper tick, the PATCH-mode fallback. Computes the
 * swept drive and/or mix - whichever of LFO_TARGET_DIST_DRIVE / DIST_MIX the
 * caller checked in lfo->targets - around the committed `base` block, and
 * writes ONLY the drive/mix coefs' CONST term (type/bits/rate stay whatever the
 * dist editor last applied). Only voices with no modulator osc reach this
 * stepper: not bound to a shared LFO synth (voice_lfo_wire), and without
 * CONFIG_SEQ_LFO_SHARED not on native carriers either
 * (voice_apply_native_lfo_topo). The law here matches the rails exactly.
 * No-op when the shaper is OFF - an inert target, not an implicit enable.
 * Core-0 / UI-task only. */
void voice_push_dist_lfo(uint8_t synth, const seq_dist_t *base,
                         const seq_lfo_t *lfo, float val);

/* Park voice-relative oscs [first, end) of every voice of `synth`: amp CONST
 * 0 (render skips the osc) and its amp/freq MOD rails cleared. For builders
 * outside voice_build_wave that reserve oscs (the bass presets' LFO pair
 * without CONFIG_SEQ_LFO_SHARED);
 * voice_build_wave parks its own. Send after the pool definition, before
 * any configuration of those oscs. Core-0 / UI-task only. */
void voice_park_oscs(uint8_t synth, uint8_t first, uint8_t end);

#if !CONFIG_SEQ_LFO_SHARED
/* Native per-voice carriers: compiled only without CONFIG_SEQ_LFO_SHARED,
 * whose voices reserve no carrier pair.
 *
 * Apply the AMY-native mod-source LFO routing for one synth built by
 * voice_build_wave() with oscs_per_voice=3: osc0 gets mod_source=1 plus a
 * COEF_MOD depth for each checked target, osc1 becomes the LFO carrier at the
 * BPM-synced rate. Pass lfo=NULL (or a disabled lfo) to deactivate: the mod
 * coupling is cleared and the carrier silenced.
 *
 * Idempotent and state-clean: every target's COEF_MOD is cleared before the
 * selected one is set. An LFO edit re-applies this on live oscs with no pool
 * definition (and so no osc reset) in between, so a prior target's coef would
 * persist and keep modulating (the stale-AMP case rides AMY's dB combine and
 * ramps to a DC rail).
 *
 * Core-0 / UI-task only; pushes through amy_helpers (never amy_queue_lock). */
void voice_apply_native_lfo(uint8_t synth, const seq_lfo_t *lfo, uint16_t bpm);

/* Topology-parameterized applier behind voice_apply_native_lfo (the wave-build
 * special case: carrier 1, both masks 0x01). carrier_osc = voice-relative index
 * of the reserved LFO carrier (wobble = carrier+1). Two masks select where each
 * target's COEF_MOD lands (bit n = osc n): pitch_mask takes the PITCH and SCAN
 * rails, voice_mask the FILTER, AMP, PAN and DIST rails. They are the same mask
 * on the wave build, unison cluster oscs included, which carry both the pitch
 * and the per-voice stages; a topology whose sounding oscs differ from the
 * oscs carrying those stages splits them. Every osc in either mask
 * gets mod_source and a full sibling clear. Derive the masks from
 * seq_track_voice_layout() / sequencer_core_lfo_native_layout(), never hardcode
 * osc indices. The carrier pair itself is configured by
 * voice_lfo_source_apply(). */
void voice_apply_native_lfo_topo(uint8_t synth, const seq_lfo_t *lfo,
                                 uint16_t bpm, uint8_t carrier_osc,
                                 uint8_t pitch_mask, uint8_t voice_mask);
#endif

/* ── LFO source pair ─────────────────────────────────────────────────────
 * The one carrier/wobble law, used by the LFO bank (lfo_bank.h; the pair is
 * a synth of its own) and, without CONFIG_SEQ_LFO_SHARED, by the native
 * topology above (the pair inside a consumer's voice). carrier_osc / wobble_osc are voice-relative.
 * Enabled lfo: the carrier runs voice_lfo_wave_to_amy(wave) at
 * lfo_rate_to_hz(rate, bpm) with no note, bend, velocity or envelope, amp
 * CONST 1 (or pre-dropped by the wobble swing), and takes the wobble osc as
 * its mod_source[0] with the WOB_REACH-selected COEF_MOD rails; the wobble osc
 * is a TRIANGLE at wob_rate, audible only while wob_depth > 0. NULL or
 * disabled lfo: both oscs dormant (amp CONST 0) and the carrier's wobble rails
 * cleared. `targets` and `depth` are not read - they belong to whoever the
 * carrier modulates.
 * Core-0 / UI-task only; pushes through amy_helpers (never amy_queue_lock). */
void voice_lfo_source_apply(uint8_t synth, uint8_t carrier_osc,
                            uint8_t wobble_osc, const seq_lfo_t *lfo,
                            uint16_t bpm);

/* BPM refresh of a pair configured by voice_lfo_source_apply(): only the
 * carrier's and the wobble's freq CONST. NULL lfo: no-op. Core-0 / UI-task
 * only; pushes through amy_helpers (never amy_queue_lock). */
void voice_lfo_source_set_rate(uint8_t synth, uint8_t carrier_osc,
                               uint8_t wobble_osc, const seq_lfo_t *lfo,
                               uint16_t bpm);

/* mod_synth value that unwires a mod slot. AMY unsets the slot for any synth
 * number >= its max_synths; 0xFFFF is AMY's unset sentinel, which a delta
 * reads as "no change", so the unwire value sits one below it. */
#define LFO_WIRE_UNSET 0xFFFEu

/* AMY mod slot of each per-voice LFO under CONFIG_SEQ_LFO_SHARED. LFO1
 * (voice_params_t.lfo) rides slot 1; LFO2 (voice_params_t.lfo2) rides slot 0,
 * which Juno and DX7 patch strings use for their own LFO
 * (lfo_targets_patch_owns_slot0(), lfo_targets.h). */
#define VOICE_LFO_SLOT_LFO1 1u
#define VOICE_LFO_SLOT_LFO2 0u

/* Wire one consumer synth to a shared LFO synth (lfo_bank_acquire()) through
 * mod slot `slot` (0 or 1; VOICE_LFO_SLOT_*): one synth-addressed event per
 * osc in voice_mask | pitch_mask | scan_mask (bit n = voice-relative osc n,
 * so it reaches osc n of every voice), setting mod_synth[slot] = lfo_synth,
 * clearing that slot's seven rails (COEF_MOD0 or COEF_MOD1) and then writing
 * each checked target's depth with the same law as
 * voice_apply_native_lfo_topo()'s COEF_MOD: PITCH on pitch_mask oscs, SCAN on
 * scan_mask oscs, FILTER, AMP, PAN (with pan CONST 0.5) and DIST on voice_mask
 * oscs. AMP also sets the slot's LFO cap (synth_level_set_lfo_cap(),
 * synth_level.h, which owns amy_event.synth_level and sends the slot caps x
 * mixer gain) to 10^(-3 * depth * VOICE_LFO_DEPTH_AMP), the overshoot cap
 * described at that scalar; every other call sets that slot's cap to 1.0, so
 * an unwire or a target change restores the level. The other slot's rails
 * and cap are never touched. NULL lfo, a disabled lfo, no targets or
 * lfo_synth == LFO_BANK_NONE unwire instead: mod_synth[slot] =
 * LFO_WIRE_UNSET and every rail of the slot 0. Unwiring slot 0 also clears a
 * patch string's own slot-0 wiring (mod_synth and mod_source share the
 * stored field); only a patch re-apply restores it.
 * Idempotent and state-clean, like the native applier. Masks come from
 * lfo_targets_of_patch() or the consumer's own layout.
 * Core-0 / UI-task only; pushes through amy_helpers (never amy_queue_lock). */
void voice_lfo_wire(uint8_t synth, uint8_t slot, uint16_t voice_mask,
                    uint16_t pitch_mask, uint16_t scan_mask,
                    const seq_lfo_t *lfo, uint8_t lfo_synth);
