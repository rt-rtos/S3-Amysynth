#pragma once

/* ── Sequencer / audio data model ──────────────────────────────────────────
 * Engine-owned shared model for the step sequencer: it lives in the layer that
 * owns and mutates it (sequencer_core), not in a view component. display_seq.h
 * includes this header and keeps only render-view types. Per-step
 * parameter-lock storage widening belongs here too (see the env/filter/lfo
 * notes below). */

#include "chord_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sequencer dimensions ── */
#define SEQ_TRACKS    5           /* array width: most rows a layer can hold */
#define SEQ_TRACKS_DEFAULT 4      /* rows a new layer starts with; width of the
                                     per-row default tables                  */
#define SEQ_STEPS     16          /* default steps for a new layer  */
#define SEQ_MAX_STEPS 32          /* maximum steps supported         */
#define MAX_LAYERS    4           /* compile-time layer limit        */

/* ── FM algorithm override sentinel (seq_layer_t.fm_algo_override) ── */
#define SEQ_FM_ALGO_NONE 0xFF     /* no override: the patch's baked algorithm */

/* ── Layer type ── */
typedef enum {
    SEQ_LAYER_DRUM    = 0,
    SEQ_LAYER_MELODIC = 1,
} seq_layer_type_t;

/* ── Per-track repeat rate (fires every N bars instead of every bar) ── */
typedef enum {
    SEQ_REPEAT_1  = 1,
    SEQ_REPEAT_2  = 2,
    SEQ_REPEAT_4  = 4,
    SEQ_REPEAT_8  = 8,
} seq_repeat_rate_t;

/* ── Per-row chord progression follow (seq_layer_t.follow), melodic only ──
 * Semantics: sequencer_core_set_track_follow (sequencer_core.h). */
typedef enum {
    SEQ_FOLLOW_CHORD = 0,
    SEQ_FOLLOW_ROOT,
    SEQ_FOLLOW_OFF,
    SEQ_FOLLOW_COUNT,
} seq_follow_t;

/* ── Per-step ratchet (sub-trigger count within one step's slot) ──
 * 1 = plain single trigger (default). >1 subdivides the step into that many
 * evenly-spaced hits, each with its own short gate. */
#define SEQ_MAX_RATCHET 4

/* ── Per-step conditional trig ──
 * Two independent, composable conditions (both must hold for the step to
 * fire, and only then is probability rolled):
 * EVERY n: fires on one loop pass out of every n of the layer's pattern
 *          (1 = every loop = neutral; the first loop after play-start always
 *          counts, since loop_count 0 satisfies any divisor).
 * PREV:    fires only if this track's previous step-attempt sounded on its
 *          last evaluation, so any miss breaks the chain. */
#define SEQ_STEP_EVERY_MAX 4

/* ── Per-step pitch offset ──
 * Chromatic semitones added to the step's stored pitch at emit time, on both
 * the plain periodic path and the decorated one-shot path (chord tones
 * transpose as a block - the intervals are the feature). Offset-relative by
 * design: track pitch edits and drum bank changes rewrite step_note only, so
 * an authored offset survives them untouched. Deliberately NOT re-quantized. */
#define SEQ_STEP_PITCH_OFS_MAX 24

/* ── Per-step note transform ──
 * NONE is the zeroed default: the authored pitch, on the plain path. Any other
 * mode offsets the pitch per fire and so makes the step decorated
 * (sequencer_core_step_is_decorated, seq_core_trig.c).
 *   RANDOM    re-pitches every fire by +/- a fixed semitone span.
 *   RAMP_UP   walks the pitch up across successive layer loops, then wraps.
 *   RAMP_DOWN walks it down.
 * The transformed pitch re-snaps to the active scale/chord unless the step's
 * step_quant_bypass bit is set, in which case it is emitted chromatically. */
typedef enum {
    SEQ_STEP_TRANSFORM_NONE      = 0,
    SEQ_STEP_TRANSFORM_RANDOM    = 1,
    SEQ_STEP_TRANSFORM_RAMP_UP   = 2,
    SEQ_STEP_TRANSFORM_RAMP_DOWN = 3,
    SEQ_STEP_TRANSFORM_COUNT,
} seq_step_transform_t;

/* ── Filter type constants (mirror AMY's FILTER_* values) ── */
#define SEQ_FILTER_NONE  0
#define SEQ_FILTER_LPF   1
#define SEQ_FILTER_BPF   2
#define SEQ_FILTER_HPF   3
#define SEQ_FILTER_LPF24 4
#define SEQ_FILTER_NOTCH 5
#define SEQ_FILTER_PHASER 6
#define SEQ_FILTER_COUNT 7   /* first invalid value — range guards use this */

/* Envelope modulation targets: one AMY control-coef vector each. */
typedef enum {
    SEQ_EGT_PITCH  = 0,   /* freq_coefs[]         octaves, -4..+4 */
    SEQ_EGT_CUTOFF = 1,   /* filter_freq_coefs[]  octaves, -8..+8 */
    SEQ_EGT_DRIVE  = 2,   /* dist_drive_coefs[]   octaves, -4..+4 */
    SEQ_EGT_MIX    = 3,   /* dist_mix_coefs[]     linear,  -1..+1 */
    SEQ_EGT_SCAN   = 4,   /* duty_coefs[]         linear,  -1..+1 (wavetable patches only) */
    SEQ_EGT_COUNT
} seq_eg_target_t;

static inline float seq_eg_depth_max(seq_eg_target_t t)
{
    return (t == SEQ_EGT_CUTOFF) ? 8.0f
         : (t == SEQ_EGT_MIX || t == SEQ_EGT_SCAN) ? 1.0f : 4.0f;
}

/* ── Per-voice filter state (stored alongside the ADSR envelope) ──
 * enabled=false sends FILTER_NONE (bypass). cutoff_hz is in Hz, the same unit
 * as amy_event.filter_freq_coefs; AMY converts to log-freq internally. */
typedef struct {
    uint8_t filter_type;   /* SEQ_FILTER_* */
    float   cutoff_hz;     /* 65..8000 Hz */
    float   resonance;     /* 0.51..8.0 (Q factor) */
    bool    enabled;       /* false = bypass (FILTER_NONE sent) */
    float   feedback;      /* KS string decay, 0..1 (1.0 = lossless infinite
                              sustain; above 1 the KS buffer diverges). Only
                              meaningful on feedback waves, where the filter
                              editor exposes a dedicated FB cursor alongside the
                              still-editable biquad resonance. 0.0 (memset
                              default) = never authored, so apply paths leave
                              AMY's build-time 0.9 default untouched. */
    float   ks_duty_ofs;   /* KS pluck duty minus 0.5, -0.5..0.5 (AMY holds
                              duty 0..1; d and 1-d sound the same). Stored as
                              an offset so 0.0 (memset default) is AMY's 0.5
                              burst with no sentinel; apply paths write it on
                              every KS push, so returning to 0 restores the
                              default. FB-style cursor on feedback waves. */
    float   eg_depth[2][SEQ_EGT_COUNT];
                           /* Envelope routing depths, [0]=EG0 / [1]=EG1 by
                              seq_eg_target_t. 0.0 (memset default) = inert,
                              the target tracks COEF_CONST only; non-zero
                              routes that envelope through the target's
                              COEF_EG0/COEF_EG1 slot. Signs: negative cutoff =
                              downward sweep, positive pitch = starts high and
                              drops to the note. Lives in the filter block
                              because the depths ride its authored/preview/
                              snapshot plumbing. [1][SEQ_EGT_CUTOFF] is the
                              former filter_env_amount and keeps that meaning on
                              every row type (same convention as bass_presets.c
                              and arp_core.c). A row that owns the matrix - a
                              drum row, or any row whose filter block has been
                              authored - writes every slot, so a 0 clears
                              the rail (skip-on-zero would leave the last value
                              in AMY until the next patch load); every other
                              row writes the nonzero ones only, leaving its
                              patch string's own routing intact (see
                              filter_push_eg_depths). */
} seq_filter_t;

/* Is any EG1 rail live? The cutoff slot counts only while the filter is on,
 * because that is the only state in which it is pushed. */
static inline bool seq_filter_eg1_live(const seq_filter_t *f)
{
    for (uint8_t t = 0; t < SEQ_EGT_COUNT; t++) {
        if (t == SEQ_EGT_CUTOFF && !f->enabled) continue;
        if (f->eg_depth[1][t] != 0.0f) return true;
    }
    return false;
}

/* ── LFO (per-track tempo-synced software modulator) ── */
typedef enum { LFO_MODE_FREE = 0, LFO_MODE_RETRIG = 1 } lfo_mode_t;
typedef enum {
    LFO_WAVE_SINE = 0, LFO_WAVE_TRIANGLE,
    LFO_WAVE_SAW_UP,   LFO_WAVE_SAW_DOWN,
    LFO_WAVE_SQUARE,   LFO_WAVE_RANDOM,
    LFO_WAVE_COUNT,
} lfo_wave_t;
typedef enum {
    LFO_TARGET_FILTER = 0, LFO_TARGET_AMP,
    LFO_TARGET_PITCH,      LFO_TARGET_PAN,
    LFO_TARGET_SCAN,       /* AMY `duty`: wavetable cycle-scan position when
                               wave=WAVETABLE, pulse width when wave=PULSE */
    /* Distortion rails, one independent bit each - check either or both.
       They behave like every other target (ENGINE-SEMANTICS.md, "LFO: native
       carrier vs software stepper") and live on a second target-checklist tab
       in the LFO editor, so the panel stays 5 rows tall. */
    LFO_TARGET_DIST_DRIVE, /* pre-gain sweep (breathing distortion)        */
    LFO_TARGET_DIST_MIX,   /* wet/dry sweep on a preconfigured shaper      */
    LFO_TARGET_COUNT,
} lfo_target_t;
typedef enum {
    LFO_RATE_1_8 = 0, LFO_RATE_1_4,  LFO_RATE_1_2,
    LFO_RATE_1BAR,    LFO_RATE_2BAR, LFO_RATE_4BAR,
    /* APPEND-ONLY past this point: values are persisted in project snapshots.
     * Mirrors the arp's rate range; the fast end is frequency-capped in
     * lfo_rate_to_hz / the software stepper so a tempo-synced LFO can never
     * reach an audible rate. */
    LFO_RATE_1_16,    LFO_RATE_1_32,
    LFO_RATE_1_4T,    LFO_RATE_1_8T, LFO_RATE_1_16T, LFO_RATE_1_32T,
    LFO_RATE_COUNT,
} lfo_rate_t;
typedef enum {
    /* WOBBLE reach: which carrier rails the second-order LFO drives. One
     * persisted byte; 0 keeps the historic depth+rate meaning so zero-init
     * structs and pre-reach snapshot files keep their sound. APPEND-ONLY. */
    WOB_REACH_BOTH  = 0,   /* depth + rate (historic default)          */
    WOB_REACH_DEPTH = 1,   /* depth only (was wob_depth_only = true)   */
    WOB_REACH_RATE  = 2,   /* rate only - no depth breathing           */
    WOB_REACH_COUNT,
} wob_reach_t;
typedef struct {
    bool         enabled;
    lfo_mode_t   mode;
    lfo_wave_t   wave;
    lfo_rate_t   rate;
    uint8_t      depth;    /* 0..100 %, shared across all active targets      */
    uint8_t      targets;  /* bitmask of (1 << lfo_target_t): one AMY mod
                              carrier drives every checked target - osc1's
                              amplitude is the shared depth, each target's
                              COEF_MOD scaled by its own constant.            */
    uint8_t      wob_rate; /* lfo_rate_t of the WOBBLE (second-order) LFO:
                              osc2 modulating the osc1 carrier's depth AND
                              rate via chained mod_source.                    */
    uint8_t      wob_depth;/* 0..100 % of VOICE_WOB_DEPTH_AMP; 0 (zero-init) =
                              wobble off, osc2 dormant. The UI authors this in
                              whole dB of dip below the authored LFO depth
                              (see voice_config.h); the stored unit is
                              unchanged so project snapshots stay compatible. */
    uint8_t      wob_reach;/* wob_reach_t. Persisted as the former
                              wob_depth_only byte: 0 both, 1 depth-only,
                              2 rate-only; older firmware reads 2 as nonzero
                              = depth-only, a benign degrade.                 */
    uint8_t flt_oct_q;     /* FILTER-target swing in quarter-octaves (1..16 =
                              +/-0.25..4.0 oct), independent of the shared
                              depth % - octaves are what the ear and AMY's log2
                              filter rail work in. 0 = legacy sentinel: derive
                              depth% x VOICE_LFO_DEPTH_FILTER so older files
                              keep their sound. Never read raw - resolve via
                              voice_lfo_filter_octaves().                     */
} seq_lfo_t;

/* Target-set helpers: the single mod-source oscillator can feed any subset of
 * targets at once (AMY sums the COEF_MOD contributions), so the target is a
 * set, not a scalar. */
#define LFO_TGT_BIT(t)     ((uint8_t)(1u << (t)))
#define LFO_TGT_ALL        ((uint8_t)((1u << LFO_TARGET_COUNT) - 1u))
#define LFO_HAS_TGT(l, t)  (((l)->targets & LFO_TGT_BIT(t)) != 0)

/* The distortion targets as one mask: the PATCH-mode stepper gate works on the
 * pair, not on a single bit. Seven of the eight target bits are spoken for - a
 * future dist bits/rate target would take the last one, past which `targets`
 * has to widen (a snapshot format change, since it is persisted as one byte). */
#define LFO_TGT_DIST_MASK  ((uint8_t)(LFO_TGT_BIT(LFO_TARGET_DIST_DRIVE) | \
                                      LFO_TGT_BIT(LFO_TARGET_DIST_MIX)))

/* ── ADSR envelope (one AMY EG0 breakpoint set) ──
 * Stored as concrete ms/percent so it survives patch changes and stays
 * runtime-editable. Scoped PER ROW inside seq_layer_t's voice_params_t block;
 * extending to per-step means widening the storage and updating the single
 * accessor seq_layer_env(), with no other call-site changes. */
typedef struct {
    uint32_t attack_ms;
    uint32_t decay_ms;
    uint8_t  sustain_pct;   /* 0..100, sustain level as a percentage */
    uint32_t release_ms;
    uint8_t  eg_type;       /* AMY ENVELOPE_* (NORMAL/LINEAR/DX7/TRUE_EXP)   */
} seq_env_t;

/* ── Per-osc distortion stage (AMY DIST_*) ──
 * A static waveshaper ahead of the filter: soft-clip, wavefolder, or bit
 * crusher. Stored as concrete AMY units so it survives patch changes, and
 * scoped per voice like the filter - `type == 0` (OFF) is the disable, there is
 * no separate enable flag. `bits` tops out at 24 because AMY samples are s8.23
 * (S_FRAC_BITS, amy_fixedpoint.h): one sign bit plus 23 fraction bits is where
 * quantization becomes a no-op, whatever the output stage does. */
typedef struct {
    uint8_t type;   /* AMY stage mask: bit0 CLIP, bit1 FOLD, bit2 CRUSH;
                       0 = OFF.  Stages stack in clip->fold->crush order. */
    uint8_t drive;  /* 1..16  pre-gain (fold depth for FOLD)        */
    uint8_t bits;   /* 1..24  CRUSH bit depth; 24 = no-op           */
    uint8_t rate;   /* 1..64  CRUSH sample-hold length in samples   */
    uint8_t mix;    /* 0..100 wet %                                 */
} seq_dist_t;

/* Stage-set cycle for the dist Type rows (track editor and bus FX row cycle
 * identically): raw masks ordered singles-first, labels indexed by mask. */
#define SEQ_DIST_STAGE_STATES 8u
static inline uint8_t seq_dist_stage_mask(uint8_t idx) {
    static const uint8_t order[SEQ_DIST_STAGE_STATES] = {0,1,2,4,3,5,6,7};
    return order[idx & 7u];
}
static inline uint8_t seq_dist_stage_index(uint8_t mask) {
    for (uint8_t i = 0; i < SEQ_DIST_STAGE_STATES; ++i)
        if (seq_dist_stage_mask(i) == (mask & 7u)) return i;
    return 0;
}
static inline uint8_t seq_dist_stage_step(uint8_t mask, int step) {
    uint8_t i = seq_dist_stage_index(mask);
    return seq_dist_stage_mask((uint8_t)((i + SEQ_DIST_STAGE_STATES + (uint8_t)step)
                                         % SEQ_DIST_STAGE_STATES));
}
static inline const char *seq_dist_stage_label(uint8_t mask) {
    static const char *names[SEQ_DIST_STAGE_STATES] =
        { "OFF", "CLIP", "FOLD", "C+F", "CRSH", "C+H", "F+H", "ALL" };
    return names[mask & 7u];
}

/* ── Per-voice parameter block (shared voice-config layer) ──
 * Embedded by every engine's state: melodic layers (per track AND one shared
 * layer block, see seq_layer_t.vp_layer), the arp and the drone. Bundles the
 * runtime-editable env/EG1/filter/LFO/dist with their deferred-authority
 * flags (ENGINE-SEMANTICS.md, "Voice blocks and deferred authority") plus the
 * output trim. ALWAYS initialise with voice_params_init_defaults()
 * (voice_config.h): it is the single place amp_trim gets its unity default, so
 * a memset-zeroed block is a silent voice. */
typedef struct {
    seq_env_t    env;             /* ADSR (EG0)                             */
    seq_env_t    env1;            /* second envelope (EG1)                  */
    seq_filter_t filter;
    seq_lfo_t    lfo;
    seq_dist_t   dist;
    bool         env_authored;
    bool         env1_authored;
    bool         filter_authored;
    bool         lfo_authored;
    bool         dist_authored;
    float        amp_trim;        /* output trim 0..1, unity default        */
} voice_params_t;

/* ── Voice-parameter source selector (melodic layers) ──
 * Which voice_params_t block a track reads its env/EG1/filter/LFO/dist from:
 * its own row (vp[track]) or the layer's shared block (vp_layer). Semantics:
 * sequencer_core.h, "Voice-block source selector". */
typedef enum {
    SEQ_VP_SRC_TRACK = 0,
    SEQ_VP_SRC_LAYER = 1,
} seq_vp_src_t;

/* ── Unison spec (per melodic track: seq_layer_t.track_unison) ──
 * N detuned copies of the wave build's audible osc, fanned symmetrically in
 * pitch (detune_cents at the outermost copy) and stereo (spread_pct of full
 * width), blend_pct tapering the outer copies against the center. count = 1
 * disables and restores the single-osc build exactly. Raw-wave/wavetable
 * patches only (KS excluded). Storage, scope and the global backend:
 * sequencer_core.h, "Melodic per-track unison".
 *
 * Three layouts (`layout`): the unchained fan gives every copy its own
 * filter, envelope, dist stage and pan mixdown; the headed layout splits the
 * copies into an L and an R group, each chained under a SILENT head that
 * carries those stages once for the whole group; the engine layout renders
 * each group inside ONE AMY osc (the experimental unison cluster, a LOCAL
 * EDIT in components/amy), so a voice is two audible oscs whatever the
 * count. Headed runs even counts only - an odd count floors (3 -> 2,
 * 5 -> 4); the engine plays odd counts with one more copy in the L
 * cluster; count 1 is off in every layout. */
typedef enum {
    VOICE_UNISON_LAYOUT_FAN    = 0,
    VOICE_UNISON_LAYOUT_HEADED = 1,
    VOICE_UNISON_LAYOUT_ENGINE = 2,
    VOICE_UNISON_LAYOUT_COUNT
} voice_unison_layout_t;

typedef struct {
    uint8_t count;        /* audible copies per voice, 1..VOICE_UNISON_MAX_COPIES */
    uint8_t detune_cents; /* outermost copy's offset, 0..VOICE_UNISON_MAX_DETUNE */
    uint8_t spread_pct;   /* stereo width: 0 = all center, 100 = full L/R fan */
    uint8_t blend_pct;    /* outer-copy level: 100 = equal, 0 = center only */
    uint8_t layout;       /* voice_unison_layout_t; unused in storage (0), the
                             getter fills in the global backend */
} voice_unison_t;

#define VOICE_UNISON_MAX_COPIES 7u
#define VOICE_UNISON_MAX_DETUNE 50u

/* One editable parameter group of a voice_params_t; the granularity of the
 * authored flags and of "release to patch". */
typedef enum {
    SEQ_VP_GROUP_ENV = 0,
    SEQ_VP_GROUP_ENV1,
    SEQ_VP_GROUP_FILTER,
    SEQ_VP_GROUP_LFO,
    SEQ_VP_GROUP_DIST,
    SEQ_VP_GROUP_COUNT,
} seq_vp_group_t;

/* ── Melodic patch scope (seq_layer_t.patch_scope) ──
 * Which store the patch gesture writes on a melodic layer. LAYER keeps the
 * whole layer on one timbre: `patch` and every track_patch[] hold the same
 * number. TRACK lets each row carry its own, with `patch` mirroring
 * track_patch[0] as the header/display fallback - the drum convention.
 * track_patch[] is authoritative for melodic rows in BOTH scopes; switching
 * back to LAYER re-fans `patch` over all four rows (per-row choices are not
 * remembered). Drum layers are always per-track and ignore this. */
typedef enum {
    SEQ_PATCH_SCOPE_LAYER = 0,
    SEQ_PATCH_SCOPE_TRACK = 1,
} seq_patch_scope_t;

/* ── Per-layer data (display + audio shared) ── */
typedef struct {
    seq_layer_type_t type;
    uint8_t  num_steps;                              /* 16 or 32               */
    uint8_t  num_tracks;                             /* live rows, SEQ_TRACKS_DEFAULT..SEQ_TRACKS */
    bool     grid[SEQ_TRACKS][SEQ_MAX_STEPS];        /* step on/off state      */
    uint8_t  step_note[SEQ_TRACKS][SEQ_MAX_STEPS];   /* per-step MIDI pitch    */
    int8_t   step_pitch_ofs[SEQ_TRACKS][SEQ_MAX_STEPS]; /* semitones from step_note,
                                            +-SEQ_STEP_PITCH_OFS_MAX, 0 = neutral
                                            (memset default). Applied at emit;
                                            never written by pitch/bank rewrites. */
    uint8_t  track_base_note[SEQ_TRACKS];            /* current base note      */
    voice_params_t vp[SEQ_TRACKS];       /* per-row voice parameters (EG0, EG1,
                                            filter, LFO, authored flags, output
                                            trim). Initialise each row with
                                            voice_params_init_defaults() in
                                            sequencer_core_add_layer.            */
    voice_params_t vp_layer;             /* the layer's shared voice block: what
                                            every row with vp_src == LAYER reads
                                            and what the editors write when that
                                            row is selected. amp_trim unused.
                                            Same init as vp[].                   */
    uint8_t   vp_src[SEQ_TRACKS];        /* seq_vp_src_t per row; memset 0 =
                                            TRACK (today's behaviour). Resolved
                                            by seq_track_vp(); never read the
                                            blocks directly on a push path.      */
    uint8_t   repeat_rate[SEQ_TRACKS];   /* SEQ_REPEAT_*: fires every N bars   */
    bool      mute[SEQ_TRACKS];          /* true = track produces no note-ons */
    bool      solo[SEQ_TRACKS];          /* true = this track stays audible while
                                             solo is in force. Solo is global:
                                             a flag here silences the other
                                             layers too. Overrides mute on the
                                             soloed track(s) themselves.        */
    uint8_t   follow[SEQ_TRACKS];        /* seq_follow_t per row; memset 0 =
                                            CHORD (voiced onto the chord)      */
    bool      chord_mode;               /* false = scale quantizer (default) */
    uint8_t   chord_root;                /* chromatic 0-11 (C=0)              */
    chord_type_t chord_type;
    uint8_t   swing_pct;                 /* 0..SEQ_SWING_MAX: odd 16th steps are
                                            delayed by this % of one step at
                                            emit time. 0 = straight (memset
                                            default).                          */
    uint16_t  gate_pct;                  /* note-hold, the Layer page's Gate
                                            row, drum and melodic; ranges in
                                            the sequencer_core.h gate comment.
                                            MUST be initialised to the type's
                                            SEQ_*_GATE_DEFAULT_PCT in add_layer
                                            - memset 0 silences every note.    */
    uint16_t  portamento_ms;             /* glide between step pitches (0..100
                                            ms, Layer page Glide), 0 = off. AMY
                                            portamento_alpha; re-pushed on every
                                            voice rebuild since osc reset
                                            clears it.                         */
    uint8_t   groove_pct;                /* Layer page Groove: how much of the
                                            accent/humanize velocity curve
                                            applies (0..100, 0 = flat 1.0),
                                            scaled at emit time in
                                            sequencer_step_velocity(). MUST be
                                            initialised to 100 in add_layer -
                                            memset 0 flattens dynamics.        */
    uint8_t  synth_id[SEQ_TRACKS];   /* one AMY synth per row, melodic and drum
                                        alike                                   */
    uint16_t patch;                  /* the layer's header timbre. In LAYER
                                        scope every melodic row plays it; in
                                        TRACK scope (and on drums) it mirrors
                                        track_patch[0] as a display fallback.   */
    uint8_t  patch_scope;            /* seq_patch_scope_t, melodic only;
                                        memset 0 = LAYER (today's behaviour).   */
    uint8_t  fm_algo_override;       /* melodic, FM patches only: live FM
                                        algorithm shadowing the patch's baked
                                        one (Shift+Turn on the SEQ screen).
                                        SEQ_FM_ALGO_NONE = follow the patch;
                                        MUST be initialised to that in
                                        add_layer - memset 0 is a real
                                        algorithm. Cleared on patch change,
                                        re-pushed after every reconfigure.      */
    uint16_t track_patch[SEQ_TRACKS];/* per-track timbre, authoritative for
                                        every row of both layer kinds. The
                                        configure path reads THIS, never
                                        `patch`.                                */
    uint16_t track_pcm_preset[SEQ_TRACKS]; /* drum layer, PCM engine: UI mirror
                                        of the core's per-track selection,
                                        refreshed each frame by
                                        seq_view_signature(). Drives the row
                                        label and name banner. Only the UI's
                                        copy is refreshed: on the core side
                                        this and track_pcm_mode are stale
                                        (zeros) - read the core getters,
                                        sequencer_core_get_drum_pcm_preset()
                                        / _mode().                              */
    uint8_t  track_pcm_mode[SEQ_TRACKS]; /* drum layer, PCM engine: AMY wave
                                        sub-mode (PCM_PLAY/PCM_LOOP*...);
                                        0 = engine default (one-shot). Same
                                        mirror discipline as track_pcm_preset. */
    voice_unison_t track_unison[SEQ_TRACKS]; /* melodic, per row; count 0 =
                                        never set (the getter's defaults,
                                        memset default). Read through
                                        sequencer_core_get_track_unison().  */
    uint8_t  wt_frame[SEQ_TRACKS];   /* wavetable rows: frame position, 0 =
                                        Auto (the SCAN rest rule, seq_scan_rest),
                                        1..64 = frame 0..63. Rides each note-on
                                        (seq_track_frame_duty); memset default
                                        = Auto.                                 */
    uint32_t synth_flags;            /* shared flags across the layer's rows  */
    uint8_t  num_voices;             /* per-synth voice count                 */

    /* ── Per-step probability / ratchet / conditional trig ──
     * A step with all of these at neutral is "plain", anything else
     * "decorated" (seq_core_trig.c). MUST be initialised to prob=100,
     * ratchet=1, every=1 in sequencer_core_add_layer - memset's 0% probability
     * would silence every step (the trig engine treats every==0 as 1
     * defensively, prev=0 is the correct zeroed default). */
    uint8_t  step_prob[SEQ_TRACKS][SEQ_MAX_STEPS];       /* 0..100 %, trigger probability */
    uint8_t  step_ratchet[SEQ_TRACKS][SEQ_MAX_STEPS];    /* 1..SEQ_MAX_RATCHET sub-hits    */
    uint8_t  step_every[SEQ_TRACKS][SEQ_MAX_STEPS];      /* 1..SEQ_STEP_EVERY_MAX loop divisor, 1 = neutral */
    uint8_t  step_prev[SEQ_TRACKS][SEQ_MAX_STEPS];       /* 1 = fire only if prev attempt fired */
    /* ── Per-step note transform ──
     * Neutral at 0 (TRANSFORM_NONE; quant_bypass=0 re-snaps a transformed pitch
     * to the scale): a zeroed layer needs no init. */
    uint8_t  step_transform[SEQ_TRACKS][SEQ_MAX_STEPS];    /* seq_step_transform_t (0=NONE)  */
    uint8_t  step_quant_bypass[SEQ_TRACKS][SEQ_MAX_STEPS]; /* 1 = skip scale snap on transform */

    /* ── Per-step micro-timing / velocity / ratchet taper ──
     * 0 is each field's neutral value (on-grid, unchanged velocity, flat
     * ratchet), so add_layer's memset suffices - unlike step_prob/step_ratchet,
     * whose neutral values are 100/1 and must be initialised explicitly. */
    int8_t   step_nudge[SEQ_TRACKS][SEQ_MAX_STEPS];        /* signed ticks, +-SEQ_STEP_NUDGE_MAX, both emit paths */
    int8_t   step_velocity_adj[SEQ_TRACKS][SEQ_MAX_STEPS]; /* signed percentage points added to velocity (0=none) */
    int8_t   step_ratchet_taper[SEQ_TRACKS][SEQ_MAX_STEPS];/* %-per-sub-hit velocity decay across a ratchet (0=flat) */
    uint8_t  step_frame[SEQ_TRACKS][SEQ_MAX_STEPS];        /* wavetable frame lock: 0 = none, 1..64 = frame 0..63 */
} seq_layer_t;

/* Copy one track's first 16 steps over its second 16 (every per-step array),
 * so a layer grown from 16 to 32 steps keeps sounding as it did until edited.
 * Pure data; the caller re-emits the schedule. */
static inline void seq_layer_copy_first_half(seq_layer_t *layer, uint8_t track)
{
    for (uint8_t s = 0; s < SEQ_STEPS; s++) {
        uint8_t d = (uint8_t)(s + SEQ_STEPS);
        layer->grid[track][d]               = layer->grid[track][s];
        layer->step_note[track][d]          = layer->step_note[track][s];
        layer->step_pitch_ofs[track][d]     = layer->step_pitch_ofs[track][s];
        layer->step_prob[track][d]          = layer->step_prob[track][s];
        layer->step_ratchet[track][d]       = layer->step_ratchet[track][s];
        layer->step_every[track][d]         = layer->step_every[track][s];
        layer->step_prev[track][d]          = layer->step_prev[track][s];
        layer->step_transform[track][d]     = layer->step_transform[track][s];
        layer->step_quant_bypass[track][d]  = layer->step_quant_bypass[track][s];
        layer->step_nudge[track][d]         = layer->step_nudge[track][s];
        layer->step_velocity_adj[track][d]  = layer->step_velocity_adj[track][s];
        layer->step_ratchet_taper[track][d] = layer->step_ratchet_taper[track][s];
        layer->step_frame[track][d]         = layer->step_frame[track][s];
    }
}

#ifdef __cplusplus
}
#endif
