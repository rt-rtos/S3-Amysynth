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
 * without magic numbers; the full number map is the block below. */
#define SEQ_PATCH_DX7_BASE    128
#define SEQ_PATCH_DX7_MAX     255

/* ── Virtual patch numbering (the SEQ_PATCH_* ranges below) ─────────────────
 * The single map of patch numbers; other files cite it instead of restating
 * ranges. Numbers past the 0..256 built-in (Juno/DX7/piano) range are virtual:
 * intercepted before amy_send_patch(), so they never collide with real patches.
 * Every range is numbered unconditionally, above the previous one, so a number
 * never shifts under a build flag; a range whose feature is compiled out is
 * skipped by sequencer_core_patch_compiled_out(). Melodic and arp route the
 * FULL 0..SEQ_PATCH_FULL_MAX range; each consumer opts out of what it cannot
 * play with an `excluded` predicate over the one shared catalog (patch_cycle.h),
 * e.g. drone_patch_excluded() - never per-consumer copies. */
#define SEQ_PATCH_WAVE_BASE   257
#define SEQ_PATCH_SINE        257   /* AMY SINE     */
#define SEQ_PATCH_SAW_DOWN    258   /* AMY SAW_DOWN */
#define SEQ_PATCH_SAW_UP      259   /* AMY SAW_UP   */
#define SEQ_PATCH_PULSE       260   /* AMY PULSE    */
#define SEQ_PATCH_TRIANGLE    261   /* AMY TRIANGLE */
#define SEQ_PATCH_NOISE       262   /* AMY NOISE    - drone excludes */
#define SEQ_PATCH_KS          263   /* AMY KS       - drone excludes */
#define SEQ_PATCH_WAVE_MAX    263

/* ── Multi-osc bass presets (bass_presets.c) ────────────────────────────
 * Two audible oscs plus a reserved LFO carrier pair (oscs_per_voice=4). A bass
 * preset keeps its own envelope until the user authors one for the row. */
#define SEQ_PATCH_BASS_BASE   264
#define SEQ_PATCH_BASS_1      264   /* PULSE + detuned SAW_DOWN, LPF24 swept by EG1 */
#define SEQ_PATCH_BASS_2      265   /* Acid pluck: SINE + SAW_DOWN, LPF24 on osc 1 */
#define SEQ_PATCH_BASS_3      266   /* Bright synth bass: PULSE + SAW_DOWN sub-octave, no filter */
#define SEQ_PATCH_BASS_MAX    266

/* ── Wavetable virtual patches (melodic, arp, drone; AMY_WAVETABLE only) ──
 * One virtual patch per built-in wavetable bank (pcm_tiny.h /
 * pcm_wavetable_base): wave=WAVETABLE, preset=pcm_wavetable_base+index,
 * intercepted like SEQ_PATCH_WAVE_BASE. Only routability is Kconfig-gated. */
#define SEQ_PATCH_WAVETABLE_BASE  267
#define SEQ_PATCH_WAVETABLE_0     267   /* 111.WAV      */
#define SEQ_PATCH_WAVETABLE_1     268   /* BRAIDS01.WAV */
#define SEQ_PATCH_WAVETABLE_2     269   /* PPG_WA00.WAV */
#define SEQ_PATCH_WAVETABLE_3     270   /* SINE2SAW.WAV */
#define SEQ_PATCH_WAVETABLE_4     271   /* VIRAL.WAV    */
#define SEQ_PATCH_WAVETABLE_MAX   271

static inline bool sequencer_core_is_wavetable_patch(uint16_t patch);

/* True for any raw-waveform virtual patch (SINE..KS, plus the wavetable banks
 * when AMY_WAVETABLE is compiled in). Shared by the melodic synth/LFO
 * configurators so both ranges route the same way: direct oscillator config,
 * no amy_send_patch(), native-LFO eligible. */
static inline bool sequencer_core_is_wave_patch(uint16_t patch)
{
    if (patch >= SEQ_PATCH_WAVE_BASE && patch <= SEQ_PATCH_WAVE_MAX) return true;
#if CONFIG_AMY_WAVETABLE
    if (sequencer_core_is_wavetable_patch(patch)) return true;
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
 * carrier (double modulation). Out-params may be NULL. Both paths, the target
 * rails and the DIST behaviour: ENGINE-SEMANTICS.md, "LFO: native carrier vs
 * software stepper". */
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
 * relative oscs 1..6); oscs 1..6 are SINE operators (operator-index convention:
 * custompatches/fm_voice.h). Intercepted before amy_send_patch() like the bass
 * presets. FM_BASS/EPIANO/BELL/LEAD are fixed starter presets (fm_presets.c).
 * FM_CUSTOM is the single live-editable voice driven by the FM UI screen; its
 * parameters are global, so every melodic row on this patch shares one voice. */
#define SEQ_PATCH_FM_BASE     272
#define SEQ_PATCH_FM_BASS     272   /* FM Bass (2-op chain, algorithm 0) */
#define SEQ_PATCH_FM_EPIANO   273   /* FM E.Piano (2 carriers, algorithm 0) */
#define SEQ_PATCH_FM_BELL     274   /* FM Bell (inharmonic ratios, algorithm 0) */
#define SEQ_PATCH_FM_LEAD     275   /* FM Lead (brighter 2-op chain, algorithm 0) */
#define SEQ_PATCH_FM_CUSTOM   276   /* Live-editable voice — opens the FM screen */
#define SEQ_PATCH_FM_MAX      276

/* ── Additive/partials voices (melodic + arp; oscs_per_voice = N+1) ────────
 * Osc topology: custompatches/additive_voice.h. Intercepted before
 * amy_send_patch() like the FM range. ORGAN/BELL are fixed presets
 * (additive_presets.c); ADDITIVE_CUSTOM is the single live-editable voice,
 * global not per-layer, playing its drawbar-organ default. */
#define SEQ_PATCH_ADDITIVE_BASE    277
#define SEQ_PATCH_ADDITIVE_ORGAN   277   /* drawbar organ, 8 harmonics, 1/n  */
#define SEQ_PATCH_ADDITIVE_BELL    278   /* inharmonic free-bar bell ratios  */
#define SEQ_PATCH_ADDITIVE_CUSTOM  279   /* live-editable additive voice     */
#define SEQ_PATCH_ADDITIVE_MAX     279

/* ── App-side wavetable bank (custompatches/wavetable_bank.h) ─────────────
 * Tables generated from Vital exports, served as AMY memory presets. A fixed
 * block of slots so the numbering space stays positional; slots past
 * wavetable_bank_count() are permanent holes skipped like a compiled-out
 * range. */
#define SEQ_PATCH_WAVETABLE_APP_BASE  280
#define SEQ_PATCH_WAVETABLE_APP_SLOTS 8
#define SEQ_PATCH_WAVETABLE_APP_MAX   (SEQ_PATCH_WAVETABLE_APP_BASE + SEQ_PATCH_WAVETABLE_APP_SLOTS - 1)

/* ── Custom wavetable (custompatches/wt_builder.h; CONFIG_SYNTH_CUSTOM_WT) ──
 * One global table built on the device from sixteen parameters and edited on
 * the WT screen; every row, the arp and the drones on this patch share it.
 * Numbered past the app bank, whose slots stay manifest-only. */
#define SEQ_PATCH_WAVETABLE_CUSTOM    288
#define SEQ_PATCH_ROUTABLE_MAX SEQ_PATCH_WAVETABLE_CUSTOM
uint8_t wavetable_bank_count(void);   /* custompatches/wavetable_bank.c */

/* True for a wavetable virtual patch (vendored range, app bank or the custom
 * table), regardless of whether the feature is compiled in - pair with
 * sequencer_core_patch_compiled_out() for routability. */
static inline bool sequencer_core_is_wavetable_patch(uint16_t patch)
{
    return (patch >= SEQ_PATCH_WAVETABLE_BASE && patch <= SEQ_PATCH_WAVETABLE_MAX)
        || (patch >= SEQ_PATCH_WAVETABLE_APP_BASE && patch <= SEQ_PATCH_WAVETABLE_APP_MAX)
        || patch == SEQ_PATCH_WAVETABLE_CUSTOM;
}

/* True when `patch` falls in a virtual range whose feature is NOT compiled in.
 * The single compile-awareness point for the whole patch space: browse domains
 * skip these via their `excluded` predicate (patch_cycle.h) and the patch-kind
 * dispatch snaps them to a safe fallback, so a new gated range needs only a
 * clause here - no ceiling cascade, no per-consumer hardcoded holes. Ranges
 * never renumber under build flags, which is what makes holes possible. */
static inline bool sequencer_core_patch_compiled_out(uint16_t patch)
{
#if !CONFIG_AMY_WAVETABLE
    if (sequencer_core_is_wavetable_patch(patch)) return true;
#else
    if (patch >= SEQ_PATCH_WAVETABLE_APP_BASE && patch <= SEQ_PATCH_WAVETABLE_APP_MAX &&
        patch - SEQ_PATCH_WAVETABLE_APP_BASE >= wavetable_bank_count()) return true;
#endif
#if !CONFIG_SYNTH_CUSTOM_FM
    if (patch >= SEQ_PATCH_FM_BASE && patch <= SEQ_PATCH_FM_MAX) return true;
#endif
#if !CONFIG_SYNTH_ADDITIVE
    if (patch >= SEQ_PATCH_ADDITIVE_BASE && patch <= SEQ_PATCH_ADDITIVE_MAX) return true;
#endif
#if !CONFIG_SYNTH_CUSTOM_WT
    if (patch == SEQ_PATCH_WAVETABLE_CUSTOM) return true;
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
 * LFO frequency in Hz for a note division (note_div.h) at the given BPM,
 * capped at SEQ_LFO_NATIVE_MAX_HZ. Defined in seq_core_tempo.c; shared by
 * every voice block's LFO. */
float lfo_rate_to_hz(note_div_t rate, uint16_t bpm);

/* ── Core lifecycle ── */
void sequencer_core_init(void);
void sequencer_core_set_playing(bool playing);

/* Fired on every transport change, after the core has re-armed (play) or
 * silenced (stop) its own voices; `playing` is the new state. The handler
 * owns pausing the voices sequencer_core does not drive, as for the solo hook
 * (seq_solo_change_cb_t). Runs on the caller of sequencer_core_set_playing(),
 * which is a Core 0 task. */
typedef void (*seq_transport_change_cb_t)(bool playing);
void sequencer_core_set_transport_change_cb(seq_transport_change_cb_t cb);

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
/* ── Melodic patch, per layer and per row ──
 * A melodic layer's patch_scope decides which store the patch gesture writes;
 * track_patch[] is what the configure path reads in either scope, so LAYER
 * scope simply holds all four rows on the same number.
 *
 * Obligations for the whole group: UI task only (they reconfigure layer
 * topology and send AMY events - never the render task, never an ISR); the
 * setters reject a non-melodic or out-of-range layer and clamp the patch to
 * SEQ_PATCH_ROUTABLE_MAX.
 * Guarantees: an unchanged value is a no-op (no note kill); any real change
 * clears the layer's fm_algo_override, updates the new-layer seed, and
 * reconfigures the layer with its schedule paused, so no note-on resolves
 * against a half-rebuilt osc pool.
 *
 * set_layer_patch writes `patch` AND every track_patch[], so it is both the
 * LAYER-scope writer and the TRACK -> LAYER re-apply; get_layer_patch returns
 * the header value. The per-row setter writes one row and mirrors row 0 into
 * `patch` (the display fallback, the drum convention); it reconfigures the
 * whole layer, per-row rebuild being a later optimisation. Setting the scope
 * to LAYER re-fans the header patch over all four rows - per-row choices are
 * not remembered across the round trip. */
void     sequencer_core_set_layer_patch(uint8_t layer_idx, uint16_t patch_number);
uint16_t sequencer_core_get_layer_patch(uint8_t layer_idx);
void     sequencer_core_set_melodic_track_patch(uint8_t layer_idx, uint8_t track,
                                                uint16_t patch_number);
uint16_t sequencer_core_get_melodic_track_patch(uint8_t layer_idx, uint8_t track);
/* seq_patch_scope_t (seq_model.h); the getter answers LAYER for a drum or
 * out-of-range layer. */
void     sequencer_core_set_patch_scope(uint8_t layer_idx, uint8_t scope);
uint8_t  sequencer_core_get_patch_scope(uint8_t layer_idx);

/* Re-push the live custom FM voice (s_fm_voice) to every melodic row on
 * SEQ_PATCH_FM_CUSTOM (and the arp). `what` is an FM_PUSH_* scope or an
 * operator index (fm_voice.h). Called by the FM UI screen / operator ADSR
 * editor after any edit. UI task only. */
void sequencer_core_fm_voice_changed(uint8_t what);

/* Step the FM algorithm of layer_idx by dir (+1/-1), wrapping over AMY's full
 * algorithm table (amy_num_algorithms), and push it live - operator setup
 * untouched, audible on the next render block even mid-note. There is ONE
 * override per layer, applied to every ALGO row: the layer acts if any row has
 * an ALGO osc (only the DX7 bank and the FM range qualify), the first such row
 * supplies the baked baseline, and rows on other patches are skipped. Returns
 * the applied algorithm index, or -1 when the layer is not melodic or no row
 * qualifies. The value shadows the patch (fm_algo_override) and is re-pushed
 * after every reconfigure; any patch change on the layer clears it. With a row
 * on SEQ_PATCH_FM_CUSTOM it instead steps s_fm_voice.algorithm - the voice
 * store is the source of truth there, shared with the FM screen and the arp.
 * Call context: Core 0 input/UI path only; events go through the ingest pump,
 * never call from the render task. */
int sequencer_core_cycle_layer_fm_algo(uint8_t layer_idx, int dir);

/* Re-push the live custom additive voice (s_additive_voice) to every melodic
 * row on SEQ_PATCH_ADDITIVE_CUSTOM, plus the arp if it is playing it. */
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
 * the UI task only; no-op for non-drum/out-of-range layers. Persisted. */
void    sequencer_core_set_drum_pcm_mode(uint8_t layer_idx, uint8_t track,
                                         uint8_t pcm_mode);
uint8_t sequencer_core_get_drum_pcm_mode(uint8_t layer_idx, uint8_t track);

/* ── Melodic per-track unison ──
 * Spec and layouts: voice_unison_t in seq_model.h. One spec per melodic track
 * (seq_layer_t.track_unison[], serialized with the layer); count 1 = off.
 * Raw-wave/wavetable patches only (KS excluded), native-LFO builds only; a
 * spec stored on a row playing another patch applies on its next wave build.
 * The Layer menu's Unison page follows the layer's patch scope: LAYER scope
 * edits every track (set_layer_unison), TRACK scope one (set_track_unison).
 * A change that moves any row's EFFECTIVE copy count rebuilds the whole layer
 * (sounding notes stop, like a wave change); detune/spread/blend changes push
 * live to the rows that carry copies. The getter serves {1, 12, 50, 100}
 * until the row is first set, and fills `.layout` with the global backend;
 * setters ignore `u->layout` and clamp count 1..VOICE_UNISON_MAX_COPIES,
 * detune 0..VOICE_UNISON_MAX_DETUNE, spread/blend 0..100.
 * The backend (voice_unison_layout_t, default ENGINE) is one global DEV knob,
 * not serialized; changing it rebuilds every melodic layer with a row carrying
 * unison under the old or the new backend.
 * track_unison_max: the highest effective count the row's patch and as-built
 * voice count allow (the UI caps Count with it), 0 = the row cannot carry
 * unison.
 * UI task only; no-op for drum / out-of-range layers. */
voice_unison_t sequencer_core_get_track_unison(uint8_t layer_idx, uint8_t track);

/* ── Melodic per-track wavetable frame ──
 * seq_layer_t.wt_frame[]: 0 = Auto (the SCAN rest rule), 1..64 = frame
 * 0..63 (clamped to 64). Applies to wavetable rows only, carried by each
 * note-on (step locks win, sequencer_core_set_step_frame). The Layer menu's
 * Frame row follows patch scope like Unison: LAYER scope set_layer_wt_frame
 * (every track), TRACK scope set_track_wt_frame. A change stores and re-emits
 * the track(s); unchanged values are no-ops. UI task only; no-op for drum /
 * out-of-range layers. Persisted. */
void    sequencer_core_set_track_wt_frame(uint8_t layer_idx, uint8_t track, uint8_t frame);
void    sequencer_core_set_layer_wt_frame(uint8_t layer_idx, uint8_t frame);
uint8_t sequencer_core_get_track_wt_frame(uint8_t layer_idx, uint8_t track);
void           sequencer_core_set_track_unison(uint8_t layer_idx, uint8_t track,
                                               const voice_unison_t *u);
void           sequencer_core_set_layer_unison(uint8_t layer_idx,
                                               const voice_unison_t *u);
uint8_t        sequencer_core_get_unison_layout(void);
void           sequencer_core_set_unison_layout(uint8_t layout);
uint8_t        sequencer_core_track_unison_max(uint8_t layer_idx, uint8_t track);

/* KS voices per melodic row, 1..CONFIG_SEQ_KS_VOICES_MAX (clamped): every row
 * whose patch is KS plays exactly this many voices, chord rows included,
 * overriding the layer's own voice count. Default 2, capped by the ceiling. A
 * change rebuilds each
 * layer holding a KS row (sounding notes on it stop, like a wave change).
 * Volatile - not serialized. UI task only. */
void    sequencer_core_set_ks_voices(uint8_t n);
uint8_t sequencer_core_get_ks_voices(void);

/* KS rings `layers` melodic layers need with every row KS at the voice
 * ceiling: layers x SEQ_TRACKS x CONFIG_SEQ_KS_VOICES_MAX. Pure; for sizing
 * amy_cfg.ks_oscs before amy_start(). */
uint8_t sequencer_core_ks_row_demand(uint8_t layers);

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
 * whether baked into a patch string or a custom preset (bass_presets.c).
 * Same deferred-authority model as EG0. */
bool sequencer_core_get_melodic_envelope2(uint8_t layer_idx, uint8_t track,
                                          seq_env_t *out);
void sequencer_core_set_melodic_envelope2(uint8_t layer_idx, uint8_t track,
                                          const seq_env_t *env);

/* ── Per-track amplitude trim (Layer menu Level row) ──
 * 0..1 multiplier on note velocity, default 1.0 (initialised in add_layer).
 * get returns 1.0 for invalid layer/track. set re-emits the track's scheduled
 * steps so the new level is heard from the next trig. */
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
 * is_ks also pushes the authored KS feedback (when set) and the pluck duty,
 * independent of f->enabled: they belong to the oscillator, not the optional
 * post-render filter. */
void sequencer_core_push_filter(uint8_t synth, const seq_filter_t *f, bool is_ks);

/* The filter's COEF_CONST for f->cutoff_hz under key tracking: AMY adds
 * key_track x (note - 69) / 12 octaves (COEF_NOTE, referenced to A4), so CONST
 * is cutoff_hz x 2^(key_track x (69 - pivot) / 12) and cutoff_hz is the cutoff
 * at the pivot note, the quantizer root in octave 4 (60 + root % 12). Returns
 * cutoff_hz unchanged when key_track is 0. Every app site that writes a
 * seq_filter_t's cutoff as COEF_CONST goes through this.
 * Obligations: f non-NULL. Reads the quantizer root and nothing else; pure
 * otherwise, callable from any task. */
float sequencer_core_filter_const_hz(const seq_filter_t *f);

/* Seed a never-authored filter (cutoff_hz <= 0, the zero-init sentinel) with
 * the editors' starting values: LPF24, 800 Hz, Q 1.0, disabled. Every other
 * field, key_track included, is left untouched; an authored filter is not
 * changed at all. Obligations: f non-NULL. Pure apart from writing *f; any
 * task. */
void sequencer_core_filter_seed_default(seq_filter_t *f);

/* ── Editor live-preview (AMY only; store and authored flags untouched) ──
 * Audition scratch editor values against the running engine. The full
 * preview/cancel/commit contract: ENGINE-SEMANTICS.md, "Editor preview and
 * cancel". */
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
 * above 1.0 the KS buffer diverges. Predates seq_filter_t.feedback; no caller
 * uses it. */
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

/* LFO live preview: apply the editor's scratch per detent WITHOUT storing
 * (native tracks hear it immediately; software tracks are served from the
 * preview slot by lfo_service). Cancel = reapply; commit = the normal setter.
 * Editors must call preview_clear on every close path (commit AND cancel) so
 * the service returns to reading the store. */
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
 * synth_ui_request_delete_active_layer(). Registering the applier here lets debug
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
/* Set a layer's live row count, SEQ_TRACKS_DEFAULT..SEQ_TRACKS, melodic or
 * drum. Growing seeds each new row (melodic: the top row's voice block and,
 * in TRACK scope, its patch, the next default base note and a copy of the top
 * row's grid and step decoration; drum: the top row's sound over an empty
 * grid), rebuilds the whole layer (sounding notes on it stop) and raises
 * num_tracks only once the new rows' synths exist. Shrinking cancels the
 * dropped rows' scheduled tags, kills their voices and releases their synths;
 * their stored data stays. True on success or no change, false
 * for an out-of-range layer or count. The UI mirror is the caller's to
 * re-export. Applier-task only (see above). */
bool             sequencer_core_set_layer_tracks(uint8_t layer_idx, uint8_t num_tracks);
/* Live row count of a layer; SEQ_TRACKS_DEFAULT out of range. */
uint8_t          sequencer_core_get_layer_tracks(uint8_t layer_idx);
uint8_t          sequencer_core_get_num_layers(void);
seq_layer_type_t sequencer_core_get_layer_type(uint8_t layer_idx);

/* ── Project snapshot support ──
 * Bulk export/import of one layer's persistable state, so the project loader
 * (components/project_store) need not go through per-field setters.
 * Applier-task only - same single-writer contract as add/delete_layer. */

/* Copy layer layer_idx's persistable state into *out. False if out of range. */
bool sequencer_core_export_layer(uint8_t layer_idx, seq_layer_t *out);

/* Overwrite the layer's persistable fields from *src, re-push
 * patches/envelopes/filters/LFO, and resync scheduled steps. synth_id keeps
 * its live values; num_tracks is taken from *src, clamped to
 * SEQ_TRACKS_DEFAULT..SEQ_TRACKS. Applier-task only. */
bool sequencer_core_import_layer(uint8_t layer_idx, const seq_layer_t *src);

/* ── Per-layer step / note control ── */
/* Resize a layer's loop to 16 or 32 steps. Growing copies each track's first
 * half over its second so the layer keeps sounding the same until edited;
 * shrinking cancels the upper half's schedule and releases voices it may be
 * holding, but keeps its data for a later grow. Every step is re-emitted,
 * since the loop period changed; a playing layer's playhead may jump once.
 * False for an out-of-range layer or a count other than 16/32.
 * Call context: Core 0 input/UI path only; events go through the ingest pump,
 * never call from the render task. */
bool    sequencer_core_set_layer_steps(uint8_t layer_idx, uint8_t num_steps);
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
 * A step with any of these set is "decorated" and takes the one-shot per-loop
 * path instead of the always-on periodic tag (definition: seq_core_trig.c).
 * Setters clamp and re-emit the step immediately; getters return the neutral
 * default when out of range. */
/* Per-step pitch offset in chromatic semitones from the step's stored pitch,
 * clamped to +-SEQ_STEP_PITCH_OFS_MAX; 0 is neutral (plain path unaffected).
 * Relative by design: survives track pitch edits and drum bank changes.
 * Deliberately not re-quantized. The setter re-emits the step and kills a
 * melodic track's ringing note (the rewritten off tag would no longer match the
 * sounding pitch). */
void    sequencer_core_set_step_pitch_ofs(uint8_t layer_idx, uint8_t track,
                                          uint8_t step, int8_t ofs);
int8_t  sequencer_core_get_step_pitch_ofs(uint8_t layer_idx, uint8_t track,
                                          uint8_t step);
/* Per-step velocity offset in signed percentage points of full scale, clamped
 * to +-SEQ_STEP_VEL_ADJ_MAX; 0 is neutral. Added after the track's amp_trim on
 * both emit paths, the sum clamped to silence..full. Re-emits the step. */
void    sequencer_core_set_step_velocity_adj(uint8_t layer_idx, uint8_t track,
                                             uint8_t step, int8_t pts);
int8_t  sequencer_core_get_step_velocity_adj(uint8_t layer_idx, uint8_t track,
                                             uint8_t step);
/* Per-step micro-timing in signed sequencer ticks, clamped to
 * +-SEQ_STEP_NUDGE_MAX; 0 is on-grid. Folded into the step's absolute tick on
 * the plain and decorated paths alike. Re-emits the step. */
void    sequencer_core_set_step_nudge(uint8_t layer_idx, uint8_t track,
                                      uint8_t step, int8_t ticks);
int8_t  sequencer_core_get_step_nudge(uint8_t layer_idx, uint8_t track,
                                      uint8_t step);
/* Per-step wavetable frame lock: 0 = none, 1..64 = frame 0..63 (clamped to
 * 64). Read only on a wavetable row, where it overrides the track's frame on
 * that step's note-on (both emit paths; ratchet sub-hits share it). No-op on
 * an unchanged value; otherwise stores and re-emits the track, since the plain
 * path bakes the frame into the stored entry. */
void    sequencer_core_set_step_frame(uint8_t layer_idx, uint8_t track,
                                      uint8_t step, uint8_t frame);
uint8_t sequencer_core_get_step_frame(uint8_t layer_idx, uint8_t track,
                                      uint8_t step);
/* Ratchet velocity taper in signed percent per sub-hit, clamped to
 * +-SEQ_STEP_TAPER_MAX; 0 is flat, positive decays toward the tail, negative
 * ramps up. Only the decorated path reads it (a ratchet of 1 hears nothing),
 * but the setter re-emits the step like its neighbours. */
void    sequencer_core_set_step_ratchet_taper(uint8_t layer_idx, uint8_t track,
                                              uint8_t step, int8_t pct);
int8_t  sequencer_core_get_step_ratchet_taper(uint8_t layer_idx, uint8_t track,
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
 * drone enables it explicitly. No-op for an out-of-range eg_type.
 * sequencer_core_push_envelope() sends it to every osc of each voice (an AMY
 * event naming no osc fans out); the _osc variant addresses one voice-relative
 * osc, or every osc when osc < 0. */
void sequencer_core_push_envelope(uint8_t synth, const seq_env_t *env);
void sequencer_core_push_envelope_osc(uint8_t synth, int osc, const seq_env_t *env);

/* The osc that a slot's voice-wide settings (the row/arp/live envelope, the
 * filter and its EG-routing depths) address for `patch`: 0 for the FM/ALGO
 * voices (SEQ_PATCH_FM_BASE..SEQ_PATCH_FM_MAX), whose osc 0 is the ALGO control
 * osc - a VCA over the carriers - while the operators keep their own
 * envelopes and follow its pitch; -1 (every osc of the voice) otherwise. */
int sequencer_core_patch_voice_osc(uint16_t patch);

/* The osc a voice-level amp CONST write (the software LFO's AMP target and its
 * neutral restore) addresses for `patch`: 0 on every ALGO voice - the FM range
 * and the DX7 bank - where an operator's amp CONST is its output level and
 * osc 0's scales the carriers; -1 (every osc) otherwise. Wider than
 * sequencer_core_patch_voice_osc(): a DX7 bank osc 0 has no amp EG0, so its
 * row envelope keeps the fan-out, but its amp CONST is still the VCA. */
int sequencer_core_patch_amp_osc(uint16_t patch);

/* Push env into the given synth/osc's EG1 breakpoint set (bp_is_set[1]).
 * `osc` lets a caller target a non-zero oscillator (a bass preset whose filter
 * lives on osc 1); melodic rows, arp and drone all target osc 0. This supplies
 * only the timing - whatever coef is wired to COEF_EG1 is what moves.
 *
 * Any coef wired to COEF_EG1 needs this push: AMY reads a never-configured
 * breakpoint set as a constant 1.0 (an open gate), so the coef would sit at its
 * full depth. Callers that route depths (sequencer_core_push_eg_depths) push
 * the set whenever seq_filter_eg1_live(f); a preset that wires COEF_EG1 itself
 * sends the set in the same event. This is the one statement of the rule; other
 * sites cite it. */
void sequencer_core_push_envelope_eg1(uint8_t synth, uint8_t osc, const seq_env_t *env);

/* Push f's envelope routing depths into one synth as a single event: osc < 0
 * broadcasts to every osc of the synth, otherwise it addresses that one.
 * `own` true writes all eight slots, so a 0 clears the rail; false writes the
 * nonzero ones only, leaving a patch string's own routing alone. The cutoff
 * slots go out only while f->enabled. Supplies the coef rails only - pair it
 * with sequencer_core_push_envelope_eg1() whenever seq_filter_eg1_live(f).
 * Same execution context as sequencer_core_push_envelope_eg1(). */
void sequencer_core_push_eg_depths(uint8_t synth, int osc, const seq_filter_t *f, bool own);

/* Push f's SCAN rails (duty EG0/EG1) to each osc in `mask`, the oscs that render the wave; UI-task context, via amy_helpers, ownership as above. */
void sequencer_core_push_eg_scan(uint8_t synth, uint8_t mask, const seq_filter_t *f, bool own);

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
                                  bool filter_authored, float ks_feedback,
                                  float ks_duty_ofs);

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

/* ── Per-track chord progression follow (melodic rows) ────────────────────
 * How a row reacts while its layer is in chord mode (the progression forces
 * chord mode on every melodic layer). The transpose below is entry 0's root
 * to the live chord root, and 0 whenever the progression is off or empty or
 * the layer is not in chord mode.
 *   CHORD  plain rows are voiced together onto the live chord; chord-slot
 *          rows transpose rigidly by the raw root delta. The default.
 *   ROOT   plain rows resolve against the global scale quantizer and keep
 *          that line, moved per fire by the root delta folded to the nearest
 *          interval (-6..+5); chord-slot rows take the same folded delta.
 *   OFF    plain rows resolve against the global scale quantizer as on a
 *          non-chord layer; chord-slot rows are not transposed.
 * Setter: same execution context as sequencer_core_set_track_mute (the UI /
 * input tasks). No-op for an invalid layer or track, a drum layer, a mode >=
 * SEQ_FOLLOW_COUNT, or an unchanged value. On a change the layer's rows are
 * re-resolved and the row's steps re-emitted (ROOT rows take the decorated
 * path). Getter: SEQ_FOLLOW_CHORD for invalid indexes. */
void         sequencer_core_set_track_follow(uint8_t layer_idx, uint8_t track,
                                             seq_follow_t mode);
seq_follow_t sequencer_core_get_track_follow(uint8_t layer_idx, uint8_t track);

/* ── Per-layer swing / shuffle ────────────────────────────────────────────
 * Delays odd 16th-steps by swing_pct% of one step (0..SEQ_SWING_MAX), whole
 * layer, 0 = straight. Re-emits the layer so AMY reschedules every step at its
 * swung tick; applies to plain and decorated steps alike. Mirrors
 * drone_set_swing. */
void    sequencer_core_set_layer_swing(uint8_t layer_idx, uint8_t swing_pct);
uint8_t sequencer_core_get_layer_swing(uint8_t layer_idx);

/* ── Per-layer note FX (gate length + glide + groove) ─────────────────────────
 * GATE: note-hold per layer, drum and melodic, applied at emit time.
 * 10..100 = % of one step. 101..SEQ_GATE_PCT_MAX = % of one step, for holds
 * across steps. SEQ_GATE_HOLD = held until the row's next trig. Any value: the
 * note-off lands at least one tick before the row's next active step fires (by
 * fire tick, swing and nudge included, wrapping the loop), whether or not that
 * step's conditions let it sound. GLIDE: AMY-native portamento between step
 * pitches (0..SEQ_MELODIC_PORTAMENTO_MAX_MS, 0 = off). GROOVE: how much of the
 * accent/humanize velocity curve applies (0..100, 0 = flat 1.0), scaled at emit
 * time in sequencer_step_velocity(). All per-layer; glide and groove are no-ops
 * on drum layers. Edited from the Layer menu page (ui_screen_layermenu.c).
 *
 * Gate setter: SEQ_GATE_HOLD is stored as is; any other value is clamped to
 * SEQ_GATE_PCT_MIN..SEQ_GATE_PCT_MAX. Re-emits the layer. When the previous
 * value was above 100 and the new one is lower, the layer's row voices are
 * killed, since a sounding note's rescheduled note-off may already be in the
 * past. UI task only. Gate getter: 0 for an invalid layer. */
void     sequencer_core_set_layer_gate_pct(uint8_t layer_idx, uint16_t gate_pct);
uint16_t sequencer_core_get_layer_gate_pct(uint8_t layer_idx);
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
 * When enabled, all melodic layer quantizers follow the active chord, and so
 * does an arp in CHORD quant mode (sequencer_core_progression_arp_chord) and
 * a drone with follow on (sequencer_core_progression_applied_chord).
 * progression_service() must be called at ~20 Hz from the UI task. */
void    sequencer_core_progression_set_enabled(bool en);
bool    sequencer_core_progression_get_enabled(void);
/* Launch quantization for chord applies: false (default) = instant, true =
 * musical edits hold until the next bar line while playing. */
void    sequencer_core_progression_set_apply_at_bar(bool at_bar);
bool    sequencer_core_progression_get_apply_at_bar(void);
/* The chord the melodic rows are playing, as an arp scale: its root pitch
 * class (0-11) and the quantizer scale index its chord type maps to.
 * Obligations: call from synth_ui_task only - the value is written by the
 * progression service on that task and has no cross-task guard. NULL
 * out-pointers are skipped.
 * Guarantees: true with both outputs written once a chord apply has landed;
 * false with the outputs untouched while no progression chord is applied
 * (from boot until the first apply, and after the service drains a disable or
 * an emptied progression). The value changes only on a chord apply or such a
 * drain, each of which marks the arp dirty. */
bool    sequencer_core_progression_arp_chord(uint8_t *root_pc, uint8_t *scale_idx);
/* The same applied chord as its root pitch class (0-11) and chord type, plus
 * land_tick: the absolute sequencer tick of the bar line it sounds from, or 0
 * when it applied immediately (a non-held drain, or stopped transport). The
 * drones' progression follow reads it.
 * Obligations: call from synth_ui_task only, as for
 * sequencer_core_progression_arp_chord(). NULL out-pointers are skipped.
 * Guarantees: true with the outputs written once a chord apply has landed;
 * false with the outputs untouched while no progression chord is applied. The
 * value changes only on a chord apply or a disable/empty drain, each of which
 * calls drone_core_follow_changed() and drone_std_core_follow_changed(). */
bool    sequencer_core_progression_applied_chord(uint8_t *root_pc, chord_type_t *type,
                                                 uint32_t *land_tick);
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
/* Ask for every melodic layer to hold at least `rows` rows (clamped to
 * SEQ_TRACKS; layers never shrink). A byte store, any task; consumed by the
 * next progression_service() drain before the chord apply. */
void    sequencer_core_progression_request_rows(uint8_t rows);
/* Returns a bitmask of the layers whose row count the drain changed (bit i =
 * layer i), for the caller to re-export into its UI mirror. */
uint8_t sequencer_core_progression_service(void);

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
