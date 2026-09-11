#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"     /* CONFIG_* gates on the virtual patch ranges below */
#include "seq_model.h"     /* seq_layer_type_t, seq_layer_t, SEQ_* defines */
#include "chord_types.h"
#include "seq_chords.h"    /* chord presets: seq_chord_t + sentinel macros */
#include "prog_gen.h"      /* prog_gen_params_t - the progression generator */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" /* TaskHandle_t — layers-applier registration below */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Built-in AMY patch banks (real patch strings, 0..256) ──────────────────
 * Named so FM-aware code (algorithm stepping) can range-check the DX7 bank
 * without magic numbers; the full bank map is the comment in
 * sequencer_core_set_melodic_patch(). */
#define SEQ_PATCH_DX7_BASE    128
#define SEQ_PATCH_DX7_MAX     255

/* ── Virtual wave-patch IDs ─────────────────────────────────────────────────
 * Patch numbers beyond the 0..256 built-in (Juno/DX7/piano) range, intercepted
 * before amy_send_patch() so they never collide with real patches. Melodic and
 * arp route the FULL 0..SEQ_PATCH_FULL_MAX range; the drone opts out of what
 * its excitation model can't play via drone_patch_excluded() (drone_core.c).
 * Exclusions are per-consumer predicates over one shared catalog
 * (patch_cycle.h / ui_patch_cycle.c) - never per-consumer copies. */
#define SEQ_PATCH_WAVE_BASE   257
#define SEQ_PATCH_SINE        257   /* AMY SINE     */
#define SEQ_PATCH_SAW_DOWN    258   /* AMY SAW_DOWN */
#define SEQ_PATCH_SAW_UP      259   /* AMY SAW_UP   */
#define SEQ_PATCH_PULSE       260   /* AMY PULSE    */
#define SEQ_PATCH_TRIANGLE    261   /* AMY TRIANGLE */
#define SEQ_PATCH_NOISE       262   /* AMY NOISE    - drone excludes */
#define SEQ_PATCH_KS          263   /* AMY KS       - drone excludes */
#define SEQ_PATCH_WAVE_MAX    263

/* ── Multi-osc bass presets (melodic only; oscs_per_voice=2) ────────────
 * Intercepted before amy_send_patch(). Osc 0 is the primary carrier, osc 1 the
 * secondary/sub layer. Osc 0's envelope is overwritten by
 * sequencer_configure_melodic_envelope() after configure; the character comes
 * from osc structure + filter. */
#define SEQ_PATCH_BASS_BASE   264
#define SEQ_PATCH_BASS_1      264   /* Classic Sub-Heavy Detune (PULSE + detuned SAW, LPF24) */
#define SEQ_PATCH_BASS_2      265   /* Solid Sine-Reinforced Acid/Pluck (SINE + SAW, LPF24) */
#define SEQ_PATCH_BASS_3      266   /* FM DX7-Style (SINE carrier + sub-octave SINE, DX7 env) */
#define SEQ_PATCH_BASS_MAX    266

/* ── Wavetable virtual patches (melodic, arp, drone; AMY_WAVETABLE only) ──
 * One virtual patch per built-in wavetable bank (pcm_tiny.h /
 * pcm_wavetable_base): wave=WAVETABLE, preset=pcm_wavetable_base+index,
 * intercepted like SEQ_PATCH_WAVE_BASE. Its own range so it could be added
 * without renumbering the bass presets. The numbers exist unconditionally, like
 * every virtual range, so sequencer_core_patch_compiled_out() can name them in
 * any build; only routability is Kconfig-gated. */
#define SEQ_PATCH_WAVETABLE_BASE  267
#define SEQ_PATCH_WAVETABLE_0     267   /* 111.WAV      */
#define SEQ_PATCH_WAVETABLE_1     268   /* BRAIDS01.WAV */
#define SEQ_PATCH_WAVETABLE_2     269   /* PPG_WA00.WAV */
#define SEQ_PATCH_WAVETABLE_3     270   /* SINE2SAW.WAV */
#define SEQ_PATCH_WAVETABLE_4     271   /* VIRAL.WAV    */
#define SEQ_PATCH_WAVETABLE_MAX   271

/* True for any raw-waveform virtual patch (SINE..KS, plus the wavetable banks
 * when AMY_WAVETABLE is compiled in). Shared by the melodic synth/LFO
 * configurators so both ranges route the same way: direct oscillator config,
 * no amy_send_patch(), native-LFO eligible. */
static inline bool sequencer_core_is_wave_patch(uint16_t patch)
{
    if (patch >= SEQ_PATCH_WAVE_BASE && patch <= SEQ_PATCH_WAVE_MAX) return true;
#if CONFIG_AMY_WAVETABLE
    if (patch >= SEQ_PATCH_WAVETABLE_BASE && patch <= SEQ_PATCH_WAVETABLE_MAX) return true;
#endif
    return false;
}

/* Native-LFO layout for a patch: which voice-relative osc carries the LFO pair
 * (carrier; wobble = carrier+1) and which audible oscs take the COEF_MOD
 * coupling (bitmask, bit n = osc n). True for every topology reserving a
 * trailing carrier pair (raw-wave/wavetable 3-osc, custom bass 4-osc); false
 * means the 20 Hz software stepper serves the LFO. THE single
 * native-vs-software predicate - every gate must use it, since a site left on
 * sequencer_core_is_wave_patch would stack the software stepper on a native
 * carrier (double modulation). Out-params may be NULL. */
static inline bool sequencer_core_lfo_native_layout(uint16_t patch,
                                                    uint8_t *carrier_osc,
                                                    uint8_t *coupled_mask)
{
    if (sequencer_core_is_wave_patch(patch)) {
        if (carrier_osc)  *carrier_osc  = 1;
        if (coupled_mask) *coupled_mask = 0x01;
        return true;
    }
    if (patch >= SEQ_PATCH_BASS_BASE && patch <= SEQ_PATCH_BASS_MAX) {
        if (carrier_osc)  *carrier_osc  = 2;    /* osc0/1 audible pair */
        if (coupled_mask) *coupled_mask = 0x03; /* couple BOTH audible oscs */
        return true;
    }
    return false;
}

/* ── DX7-style 6-operator FM/ALGO voices (melodic only; oscs_per_voice=7) ──
 * Osc 0 is the AMY ALGO control osc (algorithm + algo_source[0..5] wired to
 * relative oscs 1..6); oscs 1..6 are SINE operators. Intercepted before
 * amy_send_patch() like the bass presets. FM_BASS/EPIANO/BELL/LEAD are fixed
 * starter presets (fm_presets.c). FM_CUSTOM is the single live-editable voice
 * driven by the FM UI screen (custompatches/fm_voice.h); its parameters are
 * global, so every melodic row on this patch shares one voice. Numbered
 * unconditionally after the wavetable range rather than reusing it, so patch
 * numbers never shift under a build-flag change. */
#define SEQ_PATCH_FM_BASE     272
#define SEQ_PATCH_FM_BASS     272   /* FM Bass (2-op chain, algorithm 0) */
#define SEQ_PATCH_FM_EPIANO   273   /* FM E.Piano (2 carriers, algorithm 0) */
#define SEQ_PATCH_FM_BELL     274   /* FM Bell (inharmonic ratios, algorithm 0) */
#define SEQ_PATCH_FM_LEAD     275   /* FM Lead (brighter 2-op chain, algorithm 0) */
#define SEQ_PATCH_FM_CUSTOM   276   /* Live-editable voice — opens the FM screen */
#define SEQ_PATCH_FM_MAX      276

/* ── Additive/partials voices (melodic + arp; oscs_per_voice = N+1) ────────
 * Osc 0 is the AMY BYO_PARTIALS control osc (preset = partial count, shared
 * envelope); oscs 1..N are PARTIAL sines summed additively - no algo_source
 * routing, AMY derives the child set from preset + osc adjacency. Intercepted
 * before amy_send_patch() like the FM range. ORGAN/BELL are fixed presets
 * (additive_presets.c); ADDITIVE_CUSTOM is the single live-editable voice
 * (custompatches/additive_voice.h), global not per-layer, playing its
 * drawbar-organ default until an additive editor screen lands. Numbered
 * unconditionally after the FM range, same reason as FM above the wavetables. */
#define SEQ_PATCH_ADDITIVE_BASE    277
#define SEQ_PATCH_ADDITIVE_ORGAN   277   /* drawbar organ, 8 harmonics, 1/n  */
#define SEQ_PATCH_ADDITIVE_BELL    278   /* inharmonic free-bar bell ratios  */
#define SEQ_PATCH_ADDITIVE_CUSTOM  279   /* live-editable additive voice     */
#define SEQ_PATCH_ADDITIVE_MAX     279
#define SEQ_PATCH_ROUTABLE_MAX SEQ_PATCH_ADDITIVE_MAX

/* True when `patch` falls in a virtual range whose feature is NOT compiled in.
 * The single compile-awareness point for the whole patch space: browse domains
 * skip these via their `excluded` predicate (patch_cycle.h) and the patch-kind
 * dispatch snaps them to a safe fallback, so a new gated range needs only a
 * clause here - no ceiling cascade, no per-consumer hardcoded holes. Ranges
 * never renumber under build flags, which is what makes holes possible. */
static inline bool sequencer_core_patch_compiled_out(uint16_t patch)
{
#if !CONFIG_AMY_WAVETABLE
    if (patch >= SEQ_PATCH_WAVETABLE_BASE && patch <= SEQ_PATCH_WAVETABLE_MAX) return true;
#endif
#if !CONFIG_SYNTH_CUSTOM_FM
    if (patch >= SEQ_PATCH_FM_BASE && patch <= SEQ_PATCH_FM_MAX) return true;
#endif
#if !CONFIG_SYNTH_ADDITIVE
    if (patch >= SEQ_PATCH_ADDITIVE_BASE && patch <= SEQ_PATCH_ADDITIVE_MAX) return true;
#endif
    (void)patch;
    return false;
}

/* Browse/clamp ceiling: always the top of the numbering space. Compiled-out
 * ranges are skipped dynamically by sequencer_core_patch_compiled_out(), not by
 * shrinking this, which could not express interior holes (additive on, FM
 * off). */
#define SEQ_PATCH_FULL_MAX SEQ_PATCH_ROUTABLE_MAX

/* ── BPM range & default (shared with synth_ui for boot initialisation) ── */
#define SEQ_DEFAULT_BPM  108

/* ── Shared LFO helper ────────────────────────────────────────────────────
 * Convert a tempo-synced lfo_rate_t to a frequency in Hz for the given BPM.
 * Defined in sequencer_core.c; also used by arp_core.c (avoids duplication). */
float lfo_rate_to_hz(lfo_rate_t rate, uint16_t bpm);

/* ── Core lifecycle ── */
void sequencer_core_init(void);
void sequencer_core_set_playing(bool playing);
/* Absolute tick of the next bar line on the sequencer's bar grid (a multiple
 * of SEQ_TICKS_PER_BAR strictly after now), or 0 while the transport is
 * stopped. UI task. */
uint32_t sequencer_core_next_bar_tick(void);

/* ── Freeze (loop bounce) ────────────────────────────────────────────────
 * From end_tick on, nothing periodic fires: AMY's repeating entries (every
 * sequencer track and the arp) and the trig engine's decorated one-shots
 * both stop on that tick exactly, wherever the UI task happens to be.
 * Already-scheduled one-shots still fire. Set at arm, cleared by the commit
 * or a cancel; clearing lets the entries resume at their next due tick.
 * UI task. */
void sequencer_core_freeze_set(uint32_t end_tick);
void sequencer_core_freeze_clear(void);
/* Render task, non-blocking: queue a release of every source voice (all
 * layer tracks, the arp, the four drone synths) on the trig pump; the pump
 * task sends the note-offs one block later. */
void sequencer_core_freeze_release_enqueue(void);
/* Mute every track of every layer (the ordinary per-track mute; visible and
 * reversible in Trackopts). UI task. */
void sequencer_core_mute_all_tracks(void);
/* Snapshot / restore of every track's mute flag, one bit per track per
 * layer (bit t of out[layer]). Restore applies through the ordinary setter,
 * only to layers and tracks that exist now. UI task. */
void sequencer_core_get_mute_masks(uint8_t out[MAX_LAYERS]);
void sequencer_core_set_mute_masks(const uint8_t in[MAX_LAYERS]);
/* The pattern's period in bars: the progression's pass length while one is
 * enabled, else the longest track period (steps x repeat rate) over every
 * layer; never 0. What a loop must be a multiple of to stay in step. */
uint8_t sequencer_core_pattern_period_bars(void);
void sequencer_core_set_bpm(uint16_t bpm);
uint16_t sequencer_core_get_bpm(void);
void sequencer_core_set_quantizer_enabled(bool enabled);
void sequencer_core_set_quantizer_root_note(uint8_t root_note);
void sequencer_core_set_quantizer_scale(uint8_t scale_index);
bool sequencer_core_get_quantizer_enabled(void);
uint8_t sequencer_core_get_quantizer_root_note(void);
uint8_t sequencer_core_get_quantizer_scale(void);
void     sequencer_core_set_layer_patch(uint8_t layer_idx, uint16_t patch_number);
uint16_t sequencer_core_get_layer_patch(uint8_t layer_idx);

/* Re-push the live custom FM voice (s_fm_voice) to every melodic row on
 * SEQ_PATCH_FM_CUSTOM (and the arp). `what` is an FM_PUSH_* scope or an
 * operator index (fm_voice.h). Called by the FM UI screen / operator ADSR
 * editor after any edit. UI task only. */
void sequencer_core_fm_voice_changed(uint8_t what);

/* Step the FM algorithm of layer_idx's patch by dir (+1/-1), wrapping over
 * AMY's full algorithm table (amy_num_algorithms), and push it live to every
 * row of the layer - operator setup untouched, audible on the next render
 * block even mid-note. Returns the applied algorithm index, or -1 when the
 * layer is not melodic or its patch has no ALGO osc (only the DX7 bank and the
 * FM range qualify). The value shadows the patch (fm_algo_override) and is
 * re-pushed after every reconfigure; changing the layer's patch clears it. On
 * SEQ_PATCH_FM_CUSTOM it instead steps s_fm_voice.algorithm - the voice store
 * is the source of truth there, shared with the FM screen and the arp.
 * Call context: Core 0 input/UI path only; events go through the ingest pump,
 * never call from the render task. */
int sequencer_core_cycle_layer_fm_algo(uint8_t layer_idx, int dir);

/* Re-push the live custom additive voice (s_additive_voice) to every melodic
 * row on SEQ_PATCH_ADDITIVE_CUSTOM, plus the arp if it is playing it. For the
 * future additive UI screen. */
void sequencer_core_additive_voice_changed(void);

/* ── Drum per-track patch (curated Juno list) ──
 * Each drum track owns its own patch. cycle steps the curated list by `dir`
 * and returns the newly-applied patch. No-ops for non-drum/out-of-range. */
void     sequencer_core_set_drum_patch(uint8_t layer_idx, uint8_t track,
                                       uint16_t patch_number);
uint16_t sequencer_core_get_drum_patch(uint8_t layer_idx, uint8_t track);
uint16_t sequencer_core_cycle_drum_patch(uint8_t layer_idx, uint8_t track,
                                         int dir);

/* ── Drum sound source (whole-layer Synth vs PCM) ──
 * SEQ_DRUM_SYNTH = tonal AMY patches per track; SEQ_DRUM_PCM = built-in 808
 * samples. Both route through the SAME velocity + pitch path (render_pcm tunes
 * the sample by midi_note), so PCM hits are humanized too. Whole-layer; set
 * reconfigures the drum synth slots in place, safe while playing. */
typedef enum {
    SEQ_DRUM_SYNTH = 0,
    SEQ_DRUM_PCM   = 1,
} seq_drum_engine_t;

void              sequencer_core_set_drum_engine(seq_drum_engine_t engine);
seq_drum_engine_t sequencer_core_get_drum_engine(void);

/* ── Sequencer state dump (DEV menu) ──
 * One-shot console print of every layer's track config plus step exceptions,
 * for reading runtime-tuned values (drum pitch/preset) back off the device.
 * Obligations: UI/menu task only (reads live layer state, which that task
 * owns; never render path or ISR). Blocks the caller for the console write
 * of a few dozen lines. Compiled only with CONFIG_SYNTH_DEV_MENU. */
void sequencer_core_dump_state(void);

/* ── Drum per-track PCM preset override ──
 * PCM mode defaults to the boot bank's role preset; this lets a runtime-recorded
 * sample (custompatches/sample_rec) replace one track's preset without a
 * shared-struct field. Live-reloads the track's osc when PCM is already active,
 * otherwise takes effect on the next set_drum_engine(SEQ_DRUM_PCM). No-op for
 * non-drum/out-of-range layers. */
void     sequencer_core_set_drum_pcm_preset(uint8_t layer_idx, uint8_t track,
                                            uint16_t preset_number);
uint16_t sequencer_core_get_drum_pcm_preset(uint8_t layer_idx, uint8_t track);

/* ── Drum per-track PCM playback mode ──
 * AMY wave sub-mode for the track's PCM osc (PCM_PLAY / PCM_LOOP /
 * PCM_LOOP_FOREVER..., amy.h); 0 = engine default (one-shot), and only a
 * nonzero mode is ever pushed to the osc. PCM_LOOP_FOREVER ends notes via the
 * amp EG release instead of a sample-level hard stop - loop modes only sound
 * right on presets whose loopstart/loopend are musically set. Live-reloads
 * the track's osc (via the preset reload path, so envelope/HPF re-apply) when
 * PCM is active; otherwise takes effect on the next engine toggle. Call from
 * the UI task only; no-op for non-drum/out-of-range layers. Persisted in
 * LAYR v10+. No UI writer yet - groundwork for a loop toggle. */
void    sequencer_core_set_drum_pcm_mode(uint8_t layer_idx, uint8_t track,
                                         uint8_t pcm_mode);
uint8_t sequencer_core_get_drum_pcm_mode(uint8_t layer_idx, uint8_t track);

/* ââ Melodic per-layer unison (PROTOTYPE - dev menu) ââ
 * N detuned/spread copies of the wave build's audible osc, per melodic layer,
 * applied to all of its rows. count 1 = off (the exact single-osc build).
 * Raw-wave/wavetable patches only (KS excluded), native-LFO builds only; a
 * spec set while another patch is loaded is stored and applies on the next
 * wave build. `layout` (voice_unison_layout_t) picks the form: FAN spreads
 * the copies unchained (a filter, envelope, dist stage and pan mixdown each),
 * HEADED chains them under two SILENT L/R heads that carry those stages once
 * per group, ENGINE renders each group inside one AMY unison-cluster osc (the
 * experimental LOCAL EDIT; two audible oscs per voice at any count). Headed
 * and engine take even counts only, so an odd count floors (3 -> 2, 5 -> 4)
 * and 1 stays off in every layout. A count or layout change rebuilds the
 * layer's voices (sounding notes stop, like a wave change);
 * detune/spread/blend changes push live. The getter serves
 * defaults until the first set (count 1 = inactive). Volatile - not
 * serialized. UI task only; no-op for the drum layer / out-of-range. */
void           sequencer_core_set_unison(uint8_t layer_idx,
                                         const voice_unison_t *u);
voice_unison_t sequencer_core_get_unison(uint8_t layer_idx);

/* Step one drum track's PCM preset by `dir` through the combined drum sample
 * space: the ROM bank (0 .. pcm_wavetable_base-1) then the gamma9001 banks
 * (256..391) when their blob is mounted, wrapping, with wavetable and memory
 * presets excluded. A track on a memory preset (sample_rec override) steps back
 * in at the near end. Live-reloads the osc when PCM is active. Returns the new
 * preset, or 0 for non-drum/out-of-range layers. */
uint16_t sequencer_core_cycle_drum_pcm_preset(uint8_t layer_idx, uint8_t track,
                                              int dir);

/* ── Drum source selector (menu "Drum Bank") ──
 * One flat domain: optionally the Synth engine (CONFIG_SYNTH_DRUM_SYNTH_MODE),
 * then the ROM 808 bank, then each gamma9001 bank (listed only while the drums
 * partition is mounted). Selecting a PCM bank forces the PCM engine and
 * re-seeds every drum track with that bank's role defaults; the pattern keeps
 * playing. The selector shows the last applied source, and per-track presets
 * stay freely cyclable across banks afterwards. */
uint8_t     sequencer_core_drum_source_count(void);
const char *sequencer_core_drum_source_name(uint8_t idx);
uint8_t     sequencer_core_get_drum_source(void);
void        sequencer_core_set_drum_source(uint8_t idx);

/* ── Voice-block source selector (melodic layers) ──
 * Each row reads its env/EG1/filter/LFO/dist from ONE of two stored blocks:
 * its own (SEQ_VP_SRC_TRACK) or the layer's shared block (SEQ_VP_SRC_LAYER).
 * Every get/set/preview/authored call below addresses the block the row
 * currently reads, so an editor opened on a LAYER-source row shows and edits
 * the layer block, and a commit there reaches every LAYER-source row. The
 * deselected block keeps its contents; flipping back restores it. amp_trim is
 * always the row's own. Drum layers are always TRACK (set returns false).
 *
 * set re-pushes the row so it sounds like the new block at once: direct
 * pushes when every group is authored, otherwise a layer reload (brief voice
 * restart on the whole layer - the patch's own values for the unauthored
 * groups live only in the patch string). Both blocks keep the deferred-
 * authority rule: an unauthored group falls through to the patch. UI task
 * only; never from the render path. */
seq_vp_src_t sequencer_core_get_melodic_vp_source(uint8_t layer_idx, uint8_t track);
bool         sequencer_core_set_melodic_vp_source(uint8_t layer_idx, uint8_t track,
                                                  seq_vp_src_t src);
/* The rows that read the same block as `track` (always includes `track`;
 * {track} alone for a TRACK-source row). Returns the count written to peers[].
 * Editors preview and cancel-restore over this set; commits fan out to it
 * inside the setters. */
uint8_t sequencer_core_melodic_vp_peers(uint8_t layer_idx, uint8_t track,
                                        uint8_t peers[SEQ_TRACKS]);
/* Per-group authority of the block `track` reads (false = the patch owns it). */
bool sequencer_core_melodic_group_authored(uint8_t layer_idx, uint8_t track,
                                           seq_vp_group_t group);
/* Hand one group back to the patch: clears the authored flag on the block
 * `track` reads (so every peer follows) and reloads the layer so the patch's
 * own values sound again. The stored values are kept for a later re-commit.
 * No-op on drum layers. */
void sequencer_core_release_melodic_group(uint8_t layer_idx, uint8_t track,
                                          seq_vp_group_t group);

/* ── Per-row melodic ADSR envelope (runtime-editable) ──
 * Per track; each row has its own AMY synth, so envelopes are independent (see
 * seq_env_t in seq_model.h for the per-step extension path). get returns false
 * for non-melodic/out-of-range; set clamps, stores into the block the row
 * reads (see the source selector above) and pushes to every row reading it. */
bool sequencer_core_get_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                         seq_env_t *out);
void sequencer_core_set_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                         const seq_env_t *env);

/* ── Per-row second envelope (EG1, runtime-editable) ──
 * Independent AMY breakpoint generator, parallel to the EG0 accessors above.
 * Audible only if some coef (typically filter_freq_coefs) targets COEF_EG1,
 * whether baked into a patch string or into one of our custom presets (see
 * AMY-EDITS.md / bass_presets.c). Same deferred-authority model as EG0. */
bool sequencer_core_get_melodic_envelope2(uint8_t layer_idx, uint8_t track,
                                          seq_env_t *out);
void sequencer_core_set_melodic_envelope2(uint8_t layer_idx, uint8_t track,
                                          const seq_env_t *env);

/* ── Per-track amplitude trim (graph editor amp mode) ──
 * 0..1 multiplier on note velocity, default 1.0 (initialised in add_layer).
 * get returns 1.0 for invalid layer/track. set is store-only: the value applies
 * on that track's next sequencer_emit_step(). */
float sequencer_core_get_melodic_amp_scale(uint8_t layer_idx, uint8_t track);
void  sequencer_core_set_melodic_amp_scale(uint8_t layer_idx, uint8_t track,
                                           float v);

/* ── Per-row melodic filter (runtime-editable) ──
 * Parallel to the envelope system. Default: enabled=false (bypass).
 * set stores + pushes to that row's synth immediately.
 * get returns false for non-melodic/out-of-range layers. */
bool sequencer_core_get_melodic_filter(uint8_t layer_idx, uint8_t track,
                                       seq_filter_t *out);
void sequencer_core_set_melodic_filter(uint8_t layer_idx, uint8_t track,
                                       const seq_filter_t *f);

/* ── Per-row melodic distortion (runtime-editable) ──
 * Parallel to the filter. Default: type OFF (bypass), secondary params seeded
 * audible by voice_params_init_defaults(). set stores + pushes to that row's
 * synth immediately; preview pushes without storing; reapply re-pushes the
 * store (editor cancel, and after anything that rebuilds the voice).
 * get returns false for non-melodic/out-of-range layers. */
bool sequencer_core_get_melodic_dist(uint8_t layer_idx, uint8_t track,
                                     seq_dist_t *out);
void sequencer_core_set_melodic_dist(uint8_t layer_idx, uint8_t track,
                                     const seq_dist_t *d);
void sequencer_core_preview_melodic_dist(uint8_t layer_idx, uint8_t track,
                                         const seq_dist_t *d);
void sequencer_core_reapply_melodic_dist(uint8_t layer_idx, uint8_t track);

/* Push a filter directly to an arbitrary AMY synth slot (shared by arp/drone).
 * is_ks also pushes f->resonance through sequencer_core_ks_feedback_from_q()
 * into the synth's KS feedback field, independent of f->enabled: KS feedback is
 * intrinsic to the oscillator, not the optional post-render filter. */
void sequencer_core_push_filter(uint8_t synth, const seq_filter_t *f, bool is_ks);

/* ── Editor live-preview (AMY only; store and authored flags untouched) ──
 * Audition scratch editor values against the running engine. Cancel re-pushes
 * the stored state through these same calls - or, for never-authored rows whose
 * live state came from the patch itself, calls
 * sequencer_core_reload_layer_synth() (brief voice restart). */
void sequencer_core_preview_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                             const seq_env_t *env);
void sequencer_core_preview_melodic_envelope2(uint8_t layer_idx, uint8_t track,
                                              const seq_env_t *env);
void sequencer_core_preview_melodic_filter(uint8_t layer_idx, uint8_t track,
                                           const seq_filter_t *f);
bool sequencer_core_melodic_env_authored(uint8_t layer_idx, uint8_t track,
                                         uint8_t eg_index);
bool sequencer_core_melodic_filter_authored(uint8_t layer_idx, uint8_t track);
void sequencer_core_reload_layer_synth(uint8_t layer_idx);

/* Map a Q value (the [0.51, 8.0] range set_melodic_filter enforces) linearly
 * onto AMY's KS feedback range [0.0, 1.0]. Q=8.0 -> feedback=1.0 is the safe
 * ceiling (lossless two-tap average, classic infinite-sustain Karplus-Strong);
 * above 1.0 the KS buffer diverges. */
float sequencer_core_ks_feedback_from_q(float q);

/* ── Per-track melodic LFO (tempo-synced software modulator) ─────────────
 * Modulates filter cutoff, amp, pitch, or pan at a rate derived from BPM.
 * sequencer_core_lfo_service() must be called periodically (~20 Hz). */
void sequencer_core_set_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    const seq_lfo_t *lfo);
bool sequencer_core_get_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    seq_lfo_t *out);
void sequencer_core_lfo_service(void);

#if CONFIG_SEQ_OOM_RESYNC
/* Self-heal service (UI task, same cadence as the LFO service): once an AMY
 * OOM burst stops growing, re-emit every layer's schedule and re-arm the arp.
 * See seq_core_engine.c for why dropped wire events otherwise stay silent. */
void sequencer_core_oom_service(void);
#endif

/* LFO live preview: apply the editor's scratch to the engine per detent
 * WITHOUT storing (native tracks hear it immediately; software tracks are
 * served from the preview slot by lfo_service). Store untouched, authored
 * flag untouched. Cancel = reapply (re-push the committed state); commit =
 * the normal setter. Editors must call preview_clear on every close path
 * (commit AND cancel) so the service returns to reading the store. */
void sequencer_core_preview_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                        const seq_lfo_t *lfo);
void sequencer_core_reapply_melodic_lfo(uint8_t layer_idx, uint8_t track);
void sequencer_core_preview_melodic_clear(void);

/* Returns the current playhead step for the given layer (0..num_steps-1).
 * When paused the last computed step is returned (display freezes). */
uint8_t sequencer_core_get_current_step(uint8_t layer_idx);

/* ── Layer management ── */

/* Single-applier contract for structural s_layers edits: after boot init every
 * add_layer/delete_layer must run on ONE task (synth_ui_task, which drains the
 * UI's deferred requests) - the same discipline s_prog_apply_pending uses.
 * Other contexts go through synth_ui_request_add_layer() /
 * synth_ui_request_delete_to_layer(). Registering the applier here lets debug
 * builds assert the contract; before registration the assert is skipped. */
void             sequencer_core_set_layers_applier(TaskHandle_t applier);

/* Add a new layer, returning its index or 0xFF when the table is full.
 * Configures the AMY synth immediately. Applier-task only (see above). */
uint8_t          sequencer_core_add_layer(seq_layer_type_t type, uint8_t num_steps);
/* Delete a melodic layer. False for the drum layer (idx 0), the last remaining
 * layer, or out of range. Cancels all scheduled tags, frees the layer's AMY
 * oscillator slots, compacts the array and resyncs survivors if playing.
 * Applier-task only (see above). */
bool             sequencer_core_delete_layer(uint8_t layer_idx);
uint8_t          sequencer_core_get_num_layers(void);
seq_layer_type_t sequencer_core_get_layer_type(uint8_t layer_idx);

/* ── Project snapshot support ──
 * Bulk export/import of one layer's persistable state, so the project loader
 * (components/project_store) need not go through per-field setters.
 * Applier-task only - same single-writer contract as add/delete_layer. */

/* Copy layer layer_idx's persistable state into *out. False if out of range. */
bool sequencer_core_export_layer(uint8_t layer_idx, seq_layer_t *out);

/* Overwrite the layer's persistable fields from *src, re-push
 * patches/envelopes/filters/LFO, and resync scheduled steps. Runtime-owned
 * fields (synth_id, num_tracks) keep their live values; step_page resets to 0.
 * Applier-task only. */
bool sequencer_core_import_layer(uint8_t layer_idx, const seq_layer_t *src);

/* ── Per-layer step / note control ── */
void    sequencer_core_set_step(uint8_t layer_idx, uint8_t track,
                                uint8_t step, bool state);
/* Empty one track's pattern: every step off, every per-step decoration and
 * offset back to neutral (pitch offset, prob/ratchet/every/prev, transform,
 * nudge, velocity, taper). The track's pitch lane (step_note), patch, voice
 * block, repeat rate, mute and solo stay. Re-emits every step and releases
 * the track's voices, since a sounding note's off tag goes with the step.
 * Applier-task only. */
void    sequencer_core_clear_track_pattern(uint8_t layer_idx, uint8_t track);
void    sequencer_core_set_track_midi_note(uint8_t layer_idx, uint8_t track,
                                           uint8_t midi_note);
uint8_t sequencer_core_get_track_midi_note(uint8_t layer_idx, uint8_t track);
uint8_t sequencer_core_get_track_source_note(uint8_t layer_idx, uint8_t track);

/* ── Chord presets (seq_chords.h) - engine hooks ──
 * chord_slot_changed: after a table edit, sweep every melodic track referencing
 * slot `idx` - re-emit, reconfigure the voice count when it changed, fall back
 * to the track's last plain note when the slot went undefined. Called
 * automatically by seq_chords_set/clear; UI-task (single-applier) only.
 * audition_chord: one-shot preview of an arbitrary, possibly unstored chord
 * through that track's synth. */
void sequencer_core_chord_slot_changed(uint8_t idx);
void sequencer_core_audition_chord(uint8_t layer_idx, uint8_t track,
                                   const seq_chord_t *chord);

/* ── Per-step probability / ratchet / conditional trig ────────────────────
 * A step with prob==100 && ratchet==1 && cond==NONE is "plain" and costs
 * nothing extra: an always-on periodic AMY tag. Any other combination routes
 * that step through sequencer_core_service_tick()'s one-shot per-loop
 * scheduling instead (engine in seq_core_trig.c). Setters clamp and re-emit the
 * step immediately; getters return the "plain" default when out of range. */
/* Per-step pitch offset in chromatic semitones from the step's stored pitch,
 * clamped to +-SEQ_STEP_PITCH_OFS_MAX; 0 is neutral (plain path unaffected).
 * Relative by design: survives track pitch edits and drum bank changes.
 * Deliberately not re-quantized (TODO: revisit quantizer interplay). The
 * setter re-emits the step and kills a melodic track's ringing note (the
 * rewritten off tag would no longer match the sounding pitch). */
void    sequencer_core_set_step_pitch_ofs(uint8_t layer_idx, uint8_t track,
                                          uint8_t step, int8_t ofs);
int8_t  sequencer_core_get_step_pitch_ofs(uint8_t layer_idx, uint8_t track,
                                          uint8_t step);
void    sequencer_core_set_step_prob(uint8_t layer_idx, uint8_t track,
                                     uint8_t step, uint8_t prob_pct);
uint8_t sequencer_core_get_step_prob(uint8_t layer_idx, uint8_t track,
                                     uint8_t step);
void    sequencer_core_set_step_ratchet(uint8_t layer_idx, uint8_t track,
                                        uint8_t step, uint8_t count);
uint8_t sequencer_core_get_step_ratchet(uint8_t layer_idx, uint8_t track,
                                        uint8_t step);
/* EVERY and PREV are independent, composable conditions (both must hold, and
 * only then is probability rolled). n clamps to 1..SEQ_STEP_EVERY_MAX; 1 is
 * neutral (fires every loop). */
void    sequencer_core_set_step_every(uint8_t layer_idx, uint8_t track,
                                      uint8_t step, uint8_t n);
uint8_t sequencer_core_get_step_every(uint8_t layer_idx, uint8_t track,
                                      uint8_t step);
void    sequencer_core_set_step_prev(uint8_t layer_idx, uint8_t track,
                                     uint8_t step, bool on);
bool    sequencer_core_get_step_prev(uint8_t layer_idx, uint8_t track,
                                     uint8_t step);

/* ── Per-step note transform + quantize bypass (OP-Z step-component subset) ──
 * transform==SEQ_STEP_TRANSFORM_NONE keeps the plain path. Any other mode
 * offsets the emitted pitch per fire and routes the step through the decorated
 * one-shot scheduler. quant_bypass rides on the transform: it makes the
 * transformed pitch skip the scale/chord snap, and does nothing under NONE.
 * The setter clamps and re-emits the step. */
void    sequencer_core_set_step_transform(uint8_t layer_idx, uint8_t track,
                                          uint8_t step, seq_step_transform_t mode,
                                          bool quant_bypass);
void    sequencer_core_get_step_transform(uint8_t layer_idx, uint8_t track,
                                          uint8_t step, seq_step_transform_t *mode,
                                          bool *quant_bypass);

/* One AMY sequencer tick's worth of decorated-step bookkeeping: detect
 * step-boundary crossings per layer and, for any decorated step, roll its
 * conditional trig + probability and one-shot schedule its ratchet sub-hits.
 * Must run once per AMY sequencer tick - main.c wires it into
 * amy_cfg.amy_external_sequencer_hook, which runs at that cadence. No-op while
 * paused. */
void sequencer_core_service_tick(void);

/* ── Generic envelope push ────────────────────────────────────────────────
 * Push an ADSR (EG0 breakpoint set) to an arbitrary AMY synth slot's voices, so
 * the arp and drone reuse the melodic layers' EG0 delta path. The synth's osc0
 * must have its amp EG0 coef enabled - patch-loaded synths do by default, the
 * drone enables it explicitly. No-op for an out-of-range eg_type. */
void sequencer_core_push_envelope(uint8_t synth, const seq_env_t *env);

/* Push env into the given synth/osc's EG1 breakpoint set (bp_is_set[1]).
 * `osc` lets a caller target a non-zero oscillator (a bass preset whose filter
 * lives on osc 1); melodic rows, arp and drone all target osc 0. This supplies
 * only the timing - whatever coef is wired to COEF_EG1 is what moves. */
void sequencer_core_push_envelope_eg1(uint8_t synth, uint8_t osc, const seq_env_t *env);

/* ── Arpeggiator support ──────────────────────────────────────────────────
 * The arp lives in arp_core but routes all AMY traffic through these helpers,
 * so it shares the one event buffer + mutex and never races the sequencer. */

/* Dedicated AMY synth slot reserved for the arp. */
uint8_t sequencer_core_arp_synth(void);

/* Default voice count for the arp synth. */
uint8_t sequencer_core_arp_voices(void);

/* Clamp a MIDI note to the melodic playable range (C1..C7). */
uint8_t sequencer_core_clamp_melodic_note(int32_t midi_note);

/* (Re)configure the arp synth with a patch + voice count (flags = 0). */
void sequencer_core_arp_configure(uint16_t patch_number, uint8_t num_voices,
                                  bool filter_authored, float ks_feedback);

/* (Re)configure an out-of-band dedicated synth slot with any routable patch
 * (flags = 0, neutral filter/KS authoring). Kills sounding voices first and
 * reasserts global FX for string patches. For slot owners outside the track
 * allocator (live play); track and arp slots have their own entry points. */
void sequencer_core_configure_synth_slot(uint8_t synth_id, uint16_t patch_number,
                                         uint8_t num_voices);

/* AMY synth slot backing a melodic row, or 0 when layer_idx/track are out of
 * range (0 is never a melodic slot). The mapping is assigned at layer build
 * time, so it cannot be derived from the indices by the caller. */
uint8_t sequencer_core_get_track_synth(uint8_t layer_idx, uint8_t track);

/* Schedule a repeating note-on + note-off pair on the arp synth.
 *  tag_base  : unique tag for this arp step (off uses tag_base+1)
 *  midi_note : already-snapped/clamped pitch to play
 *  velocity  : 0..1 note-on velocity
 *  tick_on   : sequence tick for the note-on (must be >=1)
 *  gate_ticks: ticks the note is held before the note-off
 *  period    : repeat period in ticks (full arp cycle length) */
void sequencer_core_arp_emit_note(uint32_t tag_base, uint8_t midi_note,
                                  float velocity, uint32_t tick_on,
                                  uint32_t gate_ticks, uint32_t period);

/* Cancel a previously-scheduled arp note (clears tag_base and tag_base+1). */
void sequencer_core_arp_clear_note(uint32_t tag_base);

/* Base tag for arp events — well above the sequencer's tag space. */
uint32_t sequencer_core_arp_tag_base(void);

/* ── Per-track repeat rate ────────────────────────────────────────────────
 * A track with repeat_rate=N fires every N bars instead of every bar.
 * Re-emits all steps immediately so AMY picks up the new period. */
void              sequencer_core_set_track_repeat_rate(uint8_t layer_idx,
                                                       uint8_t track,
                                                       seq_repeat_rate_t rate);
seq_repeat_rate_t sequencer_core_get_track_repeat_rate(uint8_t layer_idx,
                                                       uint8_t track);

/* ── Per-layer swing / shuffle ────────────────────────────────────────────
 * Delays odd 16th-steps by swing_pct% of one step (0..SEQ_SWING_MAX), whole
 * layer, 0 = straight. Re-emits the layer so AMY reschedules every step at its
 * swung tick. Mirrors drone_set_swing. Engine + API only; no UI wiring yet. */
void    sequencer_core_set_layer_swing(uint8_t layer_idx, uint8_t swing_pct);
uint8_t sequencer_core_get_layer_swing(uint8_t layer_idx);

/* ── Per-layer melodic NoteFX (gate length + glide + groove) ──────────────────
 * GATE: note-hold as a % of the step (10..100, 100 = legato), applied at emit
 * time. GLIDE: AMY-native portamento between step pitches
 * (0..SEQ_MELODIC_PORTAMENTO_MAX_MS, 0 = off). GROOVE: how much of the
 * accent/humanize velocity curve applies (0..100, 0 = flat 1.0), scaled at emit
 * time in sequencer_step_velocity(). All per-layer, all no-ops on drum layers.
 * Edited from the NoteFX menu page (ui_screen_notefx.c). */
void     sequencer_core_set_melodic_gate_pct(uint8_t layer_idx, uint8_t gate_pct);
uint8_t  sequencer_core_get_melodic_gate_pct(uint8_t layer_idx);
void     sequencer_core_set_melodic_portamento_ms(uint8_t layer_idx, uint16_t ms);
uint16_t sequencer_core_get_melodic_portamento_ms(uint8_t layer_idx);
void     sequencer_core_set_melodic_groove_pct(uint8_t layer_idx, uint8_t groove_pct);
uint8_t  sequencer_core_get_melodic_groove_pct(uint8_t layer_idx);

/* ── Per-track mute / solo ────────────────────────────────────────────────
 * Mute is per track. Solo is GLOBAL: any solo anywhere silences every track
 * that is not itself soloed, across all layers, and solo overrides mute even on
 * a track that is both. Solos stack - soloing rows on two layers leaves both
 * audible. Gated in sequencer_emit_step(): an inaudible track's steps are
 * cancelled rather than scheduled, so it never produces a note-on. Both setters
 * hard-kill the affected slots' live voices so the change is heard immediately.
 *
 * The arp and the drones are not sequencer tracks - they are silenced through
 * the solo-change hook below, which the app layer wires to their duck setters.
 * The live-play voice is deliberately left alone: playing against a soloed row
 * is the point of soloing it.
 *
 * Call from the UI task. sequencer_core_set_track_solo() and
 * sequencer_core_clear_all_solos() invoke the hook synchronously. */
void sequencer_core_set_track_mute(uint8_t layer_idx, uint8_t track, bool mute);
bool sequencer_core_get_track_mute(uint8_t layer_idx, uint8_t track);
void sequencer_core_set_track_solo(uint8_t layer_idx, uint8_t track, bool solo);
bool sequencer_core_get_track_solo(uint8_t layer_idx, uint8_t track);

/* True when any track of any layer is soloed - i.e. when the solo mode that
 * silences everything else is in force. Drives the Trackopts clear affordance. */
bool sequencer_core_any_solo(void);

/* Drop every solo flag in the project and restore normal mute-only gating.
 * No-op (and no hook call) when nothing was soloed. */
void sequencer_core_clear_all_solos(void);

/* Fired whenever the set of soloed tracks changes; `any_solo` is the new value
 * of sequencer_core_any_solo(). The handler owns silencing/restoring the voices
 * sequencer_core does not drive. Runs on the caller's task (the UI task). */
typedef void (*seq_solo_change_cb_t)(bool any_solo);
void sequencer_core_set_solo_change_cb(seq_solo_change_cb_t cb);

/* Re-apply the current solo state to every layer and to the hook. For paths
 * that write layer->solo[] wholesale rather than through the setter - snapshot
 * load being the only one - so the audible result matches what was loaded. */
void sequencer_core_notify_solo_changed(void);

/* ── Global chord progression ─────────────────────────────────────────────
 * A list of (root, chord_type, duration_bars) entries that auto-advances.
 * When enabled, all melodic layer quantizers and the arp follow the active
 * chord. progression_service() must be called at ~20 Hz from the UI task. */
void    sequencer_core_progression_set_enabled(bool en);
bool    sequencer_core_progression_get_enabled(void);
/* Launch quantization for chord applies: false (default) = instant, true =
 * musical edits hold until the next bar line while playing. */
void    sequencer_core_progression_set_apply_at_bar(bool at_bar);
bool    sequencer_core_progression_get_apply_at_bar(void);
/* Drop the progression's captured pre-enable arp root/scale without restoring
 * it. Project load only: the freshly loaded arp values are the new baseline, so
 * a stale capture must not be restored over them on disable. */
void    sequencer_core_progression_reset_arp_capture(void);
/* True while the progression drives the arp's root/scale (first chord apply
 * until disable restores the capture). The arp's follow-global-quantizer mode
 * uses this so an active progression chord wins over the global scale,
 * mirroring melodic chord_mode precedence. */
bool    sequencer_core_progression_arp_owned(void);
void    sequencer_core_progression_set_entry(uint8_t idx, uint8_t root,
                                             chord_type_t chord_type,
                                             uint8_t duration_bars);
void    sequencer_core_progression_get_entry(uint8_t idx, uint8_t *root,
                                             chord_type_t *chord_type,
                                             uint8_t *duration_bars);
void    sequencer_core_progression_set_count(uint8_t count);
uint8_t sequencer_core_progression_get_count(void);
uint8_t sequencer_core_progression_get_current(void);
uint8_t sequencer_core_progression_get_max(void);
uint8_t sequencer_core_progression_bars_in_current(void);
bool    sequencer_core_progression_add_entry(void);
void    sequencer_core_progression_delete_entry(uint8_t idx);
void    sequencer_core_progression_service(void);

/* The generator's parameters, stored beside the progression they generated so
 * a project carries both. Plain struct copies; a NULL argument is ignored.
 * Callable from any task: the writers are the input tasks and project load,
 * the reader is the UI item build, and a torn read shows one stale display
 * value for one frame and nothing else - so no lock. */
void    sequencer_core_progression_gen_params_set(const prog_gen_params_t *p);
void    sequencer_core_progression_gen_params_get(prog_gen_params_t *out);

/* Manual per-layer chord override (used when global progression is off). */
void sequencer_core_progression_set_layer_chord(uint8_t layer_idx,
                                                uint8_t root,
                                                chord_type_t chord_type);
void sequencer_core_progression_clear_layer_chord(uint8_t layer_idx);
void sequencer_core_get_layer_chord(uint8_t layer_idx, bool *chord_mode,
                                    uint8_t *root, chord_type_t *chord_type);

#ifdef __cplusplus
}
#endif
