#include "sequencer_core.h"
#include "amy.h"
#include "sequencer.h"
#include "quantizer.h"
#include "seq_clamp.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/semphr.h"

#ifndef CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED
#define CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED 1
#endif
#ifndef CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE
#define CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE 60
#endif
#ifndef CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE
#define CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE 0
#endif
#ifndef CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS
#define CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS 1
#endif
#ifndef CONFIG_SEQ_MELODIC_GATE_NUMERATOR
#define CONFIG_SEQ_MELODIC_GATE_NUMERATOR 5
#endif
#ifndef CONFIG_SEQ_MELODIC_GATE_DENOMINATOR
#define CONFIG_SEQ_MELODIC_GATE_DENOMINATOR 6
#endif
#ifndef CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED
#define CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED 1
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_EG0_TYPE
#define CONFIG_SEQ_MELODIC_ENV_EG0_TYPE 0
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_ATTACK_MS
#define CONFIG_SEQ_MELODIC_ENV_ATTACK_MS 12
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_DECAY_MS
#define CONFIG_SEQ_MELODIC_ENV_DECAY_MS 220
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT
#define CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT 58
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_RELEASE_MS
#define CONFIG_SEQ_MELODIC_ENV_RELEASE_MS 280
#endif
#ifndef CONFIG_SEQ_MELODIC_PATCH
#define CONFIG_SEQ_MELODIC_PATCH 138
#endif

static const char *TAG = "seq_core";

extern uint32_t sequencer_ticks(void);

/* ── Timing ──────────────────────────────────────────────────────────── */
#define SEQ_TICKS_PER_STEP    (AMY_SEQUENCER_PPQ / 4)
#define SEQ_GATE_DRUM         (SEQ_TICKS_PER_STEP / 3)
#if CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS
#define SEQ_GATE_MELODIC      ((SEQ_TICKS_PER_STEP * CONFIG_SEQ_MELODIC_GATE_NUMERATOR) / CONFIG_SEQ_MELODIC_GATE_DENOMINATOR)
#else
#define SEQ_GATE_MELODIC      ((SEQ_TICKS_PER_STEP * 2) / 3)
#endif
#define SEQ_MIN_BPM           40
#define SEQ_MAX_BPM           300

/* ── Drum synth slot ─────────────────────────────────────────────────── */
#define SEQ_DRUM_SYNTH        10
#define SEQ_DRUM_PATCH        1025
#define SEQ_DRUM_VOICES       16 /*these voice defines control how many notes can ring at once before AMY steals/reuses a voice. More voices usually means fewer chopped notes, at the cost of more CPU.*/
#define SEQ_MIDI_NOTE_MIN     27
#define SEQ_MIDI_NOTE_MAX     87

/* ── Melodic synth defaults ──────────────────────────────────────────── */
#define SEQ_MEL_PATCH         CONFIG_SEQ_MELODIC_PATCH
#define SEQ_MEL_VOICES        16
#define SEQ_MEL_NOTE_MIN      24    /* C1 */
#define SEQ_MEL_NOTE_MAX      96    /* C7 */

/* One-shot preview fires this many ticks after an adjustment */
#define SEQ_PREVIEW_DELAY_TICKS 4

/* ── State ───────────────────────────────────────────────────────────── */
static seq_layer_t s_layers[MAX_LAYERS];
static uint8_t     s_num_layers   = 0;
static uint8_t     s_cached_step[MAX_LAYERS];
static bool        s_playing      = true;
static uint16_t    s_bpm          = 120;
static uint16_t    s_melodic_patch = SEQ_MEL_PATCH;
static uint8_t     s_track_source_note[MAX_LAYERS][SEQ_TRACKS];
static quantizer_state_t s_quantizer = {
    .root_note  = CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE,
    .scale_index = CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE,
    .enabled    = CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED,
};

static void sequencer_refresh_track_note(uint8_t layer_idx, uint8_t track,
                                        bool preview);
static void sequencer_emit_step(uint8_t layer_idx, uint8_t track, uint8_t step);
static inline uint32_t seq_preview_tag(uint8_t layer, uint8_t track);
static inline uint32_t seq_preview_off_tag(uint8_t layer, uint8_t track);

static float sequencer_step_velocity(const seq_layer_t *layer,
                                     uint8_t track, uint8_t step)
{
    if (layer->type == SEQ_LAYER_DRUM) {
        return 1.0f;
    }

#if !CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS
    (void)track;
    (void)step;
    return 1.0f;
#else

    /* With the EG0 envelope now shaping onset/tail, we can use a wider dynamic
     * range without the notes sounding dull — the accent pattern provides the
     * groove that breaks up the old "machine-gun" monotony. Base level sits
     * mid-range so accents have room to push up and ghost notes can drop down.
     * Tracks are spread slightly so stacked voices don't all hit identically. */
    float velocity = 0.62f + (0.02f * (float)track);

    /* Metric accents: strong downbeat, lighter backbeat, weak off-beats. */
    if ((step % 4) == 0) {
        velocity += 0.30f; /* downbeat of each quarter-note */
    } else if ((step % 4) == 2) {
        velocity += 0.16f; /* backbeat emphasis */
    } else {
        velocity -= 0.04f; /* the in-between 8ths sit back as ghost notes */
    }

    /* Deterministic per-step jitter so repeated bars are not bit-identical
     * (light "humanization"). Cycles every 4 steps with a small +/- swing. */
    static const float jitter[4] = { 0.015f, -0.02f, 0.01f, -0.015f };
    velocity += jitter[step & 3];

    velocity = SEQ_CLAMP_F32(velocity, 0.45f, 1.0f);
    return velocity;
#endif
}

/* ── Scratch AMY event (module-level, never on any task stack) ───────── */
/*
 * amy_event is ~800 bytes. Placing even one instance on the stack of
 * app_main (default 3584 bytes) causes a stack overflow during init.
 * All emit helpers share this single static buffer; concurrent callers
 * are serialised by s_ev_mutex (all callers are FreeRTOS tasks, never ISRs).
 */
static amy_event         s_ev;
static SemaphoreHandle_t s_ev_mutex = NULL;

static inline void seq_ev_begin(void)
{
    xSemaphoreTake(s_ev_mutex, portMAX_DELAY);
    s_ev = amy_default_event();
}

static inline void seq_ev_send(void)
{
    amy_add_event(&s_ev);
    xSemaphoreGive(s_ev_mutex);
}

/* The melodic envelope is stored PER ROW (per track). All four rows of a layer
 * currently share one AMY synth slot, so AMY can only hold ONE envelope on that
 * synth at a time: whichever row is "active" defines the live envelope. This
 * accessor is the single point of truth for "which env applies to (layer,track,
 * step)". For per-step support later, add a step parameter and index a wider
 * env[][] array here — callers stay unchanged. */
static seq_env_t *seq_layer_env(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers) layer_idx = 0;
    if (track >= SEQ_TRACKS) track = 0;
    return &s_layers[layer_idx].env[track];
}

/* The row whose envelope is currently mirrored onto the shared AMY synth. The
 * UI sets this when the user selects a row to edit; defaults to row 0. */
static uint8_t s_active_env_track[MAX_LAYERS] = {0};

/* Push the given row's stored envelope to the layer's AMY synth. */
static void sequencer_configure_melodic_envelope_track(uint8_t layer_idx, uint8_t track)
{
#if CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED
    const seq_layer_t *layer = &s_layers[layer_idx];
    const seq_env_t   *env   = seq_layer_env(layer_idx, track);
    float sustain = (float)env->sustain_pct / 100.0f;

    seq_ev_begin();
    s_ev.synth = layer->synth_id;
    s_ev.bp_is_set[0] = 1;
    s_ev.eg_type[0] = env->eg_type;
    s_ev.eg0_times[0] = env->attack_ms;
    s_ev.eg0_values[0] = 1.0f;
    s_ev.eg0_times[1] = env->decay_ms;
    s_ev.eg0_values[1] = sustain;
    s_ev.eg0_times[2] = env->release_ms;
    s_ev.eg0_values[2] = 0.0f;
    seq_ev_send();
#else
    (void)layer_idx; (void)track;
#endif
}

static void sequencer_configure_melodic_envelope(uint8_t layer_idx)
{
    sequencer_configure_melodic_envelope_track(layer_idx, s_active_env_track[layer_idx]);
}

static uint8_t sequencer_clamp_layer_note(const seq_layer_t *layer, uint8_t note)
{
    if (layer->type == SEQ_LAYER_DRUM) {
        return SEQ_CLAMP_U8(note, SEQ_MIDI_NOTE_MIN, SEQ_MIDI_NOTE_MAX);
    } else {
        return SEQ_CLAMP_U8(note, SEQ_MEL_NOTE_MIN, SEQ_MEL_NOTE_MAX);
    }
}

static uint8_t sequencer_resolve_track_note(const seq_layer_t *layer,
                                            uint8_t source_note)
{
    if (layer->type != SEQ_LAYER_MELODIC || !s_quantizer.enabled) {
        return sequencer_clamp_layer_note(layer, source_note);
    }

    const musical_scale_t *scale = quantizer_get_scale(s_quantizer.scale_index);
    uint8_t snapped = quantizer_snap_midi_note(source_note, s_quantizer.root_note, scale);
    return sequencer_clamp_layer_note(layer, snapped);
}

static void sequencer_refresh_melodic_layers(bool preview)
{
    for (uint8_t layer_idx = 0; layer_idx < s_num_layers; layer_idx++) {
        seq_layer_t *layer = &s_layers[layer_idx];
        if (layer->type != SEQ_LAYER_MELODIC) {
            continue;
        }
        for (uint8_t track = 0; track < layer->num_tracks; track++) {
            sequencer_refresh_track_note(layer_idx, track, preview);
        }
    }
}

/* Re-resolve a track's note (clamp + optional scale quantization), update every
 * step on that track to the new note, and re-emit them. When `preview` is set
 * (interactive editing) also fire a short one-shot so the user hears the note
 * immediately, even if quantization left the resolved note unchanged. */
static void sequencer_refresh_track_note(uint8_t layer_idx, uint8_t track,
                                        bool preview)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks) return;

    uint8_t source_note = s_track_source_note[layer_idx][track];
    uint8_t resolved_note = sequencer_resolve_track_note(layer, source_note);

    /* No change: skip the grid rewrite, but still preview so scrolling within
     * one scale degree remains audible. */
    if (layer->track_base_note[track] == resolved_note) {
        if (preview) {
            /* Keep the preview path active even when the snapped note does not change. */
        } else {
            return;
        }
    }

    /* Apply the resolved note to the whole track (all steps play one pitch). */
    layer->track_base_note[track] = resolved_note;
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        layer->step_note[track][s] = resolved_note;
    }

    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_step(layer_idx, track, s);
    }

    if (!preview) {
        return;
    }

    /* One-shot preview: fires a few ticks from now using the same tag slot.
     * Rapid scrolling overwrites the slot so only the last change is heard. */
    uint32_t fire_tick = sequencer_ticks() + SEQ_PREVIEW_DELAY_TICKS;
    seq_ev_begin();
    s_ev.synth                     = layer->synth_id;
    s_ev.midi_note                 = resolved_note;
    s_ev.velocity                  = 1.0f;
    s_ev.sequence[SEQUENCE_TAG]    = seq_preview_tag(layer_idx, track);
    s_ev.sequence[SEQUENCE_TICK]   = fire_tick;
    s_ev.sequence[SEQUENCE_PERIOD] = 0; /* one-shot */
    seq_ev_send();

    seq_ev_begin();
    s_ev.synth                     = layer->synth_id;
    s_ev.midi_note                 = resolved_note;
    s_ev.velocity                  = 0.0f;
    s_ev.sequence[SEQUENCE_TAG]    = seq_preview_off_tag(layer_idx, track);
    s_ev.sequence[SEQUENCE_TICK]   = fire_tick + SEQ_GATE_MELODIC;
    s_ev.sequence[SEQUENCE_PERIOD] = 0; /* one-shot */
    seq_ev_send();

    ESP_LOGI(TAG, "layer %d track %d note -> %d (preview @ tick %lu)",
             layer_idx, track, resolved_note, (unsigned long)fire_tick);
}

/* ── Tag helpers ─────────────────────────────────────────────────────── */
/*
 * Tag layout (uint32_t — tag space is effectively unlimited):
 *
 *   ON  tag = layer * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
 *             + track * SEQ_MAX_STEPS + step
 *
 *   OFF tag = ON tag + (SEQ_TRACKS * SEQ_MAX_STEPS)
 *
 *   Preview = MAX_LAYERS * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
 *             + layer * SEQ_TRACKS + track
 *
 * Per layer: 4*32*2 = 256 slots; all 4 layers occupy tags 0..1023.
 * Preview tags start at 1024.
 */
static inline uint32_t seq_tag_on(uint8_t layer, uint8_t track, uint8_t step)
{
    return (uint32_t)layer * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
         + (uint32_t)track * SEQ_MAX_STEPS
         + step;
}

static inline uint32_t seq_tag_off(uint8_t layer, uint8_t track, uint8_t step)
{
    return seq_tag_on(layer, track, step)
         + (uint32_t)(SEQ_TRACKS * SEQ_MAX_STEPS);
}

static inline uint32_t seq_preview_tag(uint8_t layer, uint8_t track)
{
    return (uint32_t)MAX_LAYERS * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
         + (uint32_t)layer * SEQ_TRACKS
         + track;
}

/* OFF tag for the preview note — occupies the block immediately after ON tags. */
static inline uint32_t seq_preview_off_tag(uint8_t layer, uint8_t track)
{
    return seq_preview_tag(layer, track) + (uint32_t)(MAX_LAYERS * SEQ_TRACKS);
}

/* ── Low-level AMY helpers ───────────────────────────────────────────── */

static void sequencer_emit_clear_tag(uint32_t tag)
{
    seq_ev_begin();
    s_ev.sequence[SEQUENCE_TAG]    = tag;
    s_ev.sequence[SEQUENCE_TICK]   = 0;
    s_ev.sequence[SEQUENCE_PERIOD] = 0;
    seq_ev_send();
}

/* (Re)configure the AMY synth for layer_idx. */
static void sequencer_configure_synth(uint8_t layer_idx)
{
    seq_layer_t *layer = &s_layers[layer_idx];
    seq_ev_begin();
    s_ev.patch_number = layer->patch;
    if (layer->type == SEQ_LAYER_DRUM) {
        patches_store_patch(&s_ev, "w7f0");
    }
    s_ev.num_voices  = layer->num_voices;
    s_ev.synth       = layer->synth_id;
    s_ev.synth_flags = layer->synth_flags;
    seq_ev_send();

    if (layer->type == SEQ_LAYER_MELODIC) {
        sequencer_configure_melodic_envelope(layer_idx);
    }
}

/* Schedule (or cancel) one grid step as a pair of repeating AMY events: a
 * note-on at the step's position in the bar, and a note-off `gate` ticks later.
 * Both repeat every `bar_ticks` so the pattern loops automatically. AMY keys
 * each event by its tag, so re-emitting with the same tag updates in place. */
static void sequencer_emit_step(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    seq_layer_t *layer  = &s_layers[layer_idx];
    /* Total ticks in one loop of this layer's pattern. */
    uint32_t bar_ticks  = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
    /* How long the note is held: drums are short/percussive, melodic longer. */
    uint8_t  gate       = (layer->type == SEQ_LAYER_DRUM)
                          ? SEQ_GATE_DRUM : SEQ_GATE_MELODIC;
    /* Melodic groove: shorten the off-beat 8ths a touch so accented downbeats
     * feel longer/legato while the in-between notes are slightly detached. We
     * only ever shorten (never lengthen past SEQ_GATE_MELODIC) so the note-off
     * always lands before the next step's note-on and never cuts it off. */
    if (layer->type == SEQ_LAYER_MELODIC && (step % 2) == 1 && gate > 2) {
        gate -= 2;
    }
    uint32_t tag_on     = seq_tag_on(layer_idx, track, step);
    uint32_t tag_off    = seq_tag_off(layer_idx, track, step);
    /* +1 so tick 0 stays reserved (AMY treats tick 0 specially as "clear"). */
    uint32_t tick_on    = (uint32_t)(1 + step * SEQ_TICKS_PER_STEP);
    /* Note-off wraps within the bar if the gate spills past the loop end. */
    uint32_t tick_off   = (tick_on + gate) % bar_ticks;
    float note_velocity = sequencer_step_velocity(layer, track, step);
    if (tick_off == 0) tick_off = 1; /* avoid the reserved tick 0 */

    /* If stopped or this step is off, cancel any previously scheduled events. */
    if (!s_playing || !layer->grid[track][step]) {
        sequencer_emit_clear_tag(tag_on);
        sequencer_emit_clear_tag(tag_off);
        return;
    }

    seq_ev_begin();
    s_ev.synth                     = layer->synth_id;
    s_ev.midi_note                 = layer->step_note[track][step];
    s_ev.velocity                  = note_velocity;
    s_ev.sequence[SEQUENCE_TAG]    = tag_on;
    s_ev.sequence[SEQUENCE_TICK]   = tick_on;
    s_ev.sequence[SEQUENCE_PERIOD] = bar_ticks;
    seq_ev_send();

    seq_ev_begin();
    s_ev.synth                     = layer->synth_id;
    s_ev.midi_note                 = layer->step_note[track][step];
    s_ev.velocity                  = 0.0f;
    s_ev.sequence[SEQUENCE_TAG]    = tag_off;
    s_ev.sequence[SEQUENCE_TICK]   = tick_off;
    s_ev.sequence[SEQUENCE_PERIOD] = bar_ticks;
    seq_ev_send();
}

/* Re-emit all steps for a layer (used on play-resume). */
static void sequencer_resync_layer(uint8_t layer_idx)
{
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < layer->num_tracks; t++) {
        for (uint8_t s = 0; s < layer->num_steps; s++) {
            sequencer_emit_step(layer_idx, t, s);
        }
    }
}

/* Cancel all scheduled tags for a layer (used on pause). */
static void sequencer_clear_layer_tags(uint8_t layer_idx)
{
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < layer->num_tracks; t++) {
        for (uint8_t s = 0; s < layer->num_steps; s++) {
            sequencer_emit_clear_tag(seq_tag_on(layer_idx, t, s));
            sequencer_emit_clear_tag(seq_tag_off(layer_idx, t, s));
        }
    }
}

/* ── BPM helpers ─────────────────────────────────────────────────────── */

static uint16_t sequencer_clamp_bpm(uint16_t b)
{
    return SEQ_CLAMP_U16(b, SEQ_MIN_BPM, SEQ_MAX_BPM);
}

static void sequencer_push_tempo(uint16_t b)
{
    seq_ev_begin();
    s_ev.tempo = b;
    seq_ev_send();
}

/* ── Public API ──────────────────────────────────────────────────────── */

void sequencer_core_init(void)
{
    if (s_ev_mutex == NULL) {
        s_ev_mutex = xSemaphoreCreateMutex();
        configASSERT(s_ev_mutex != NULL);
    }
    s_num_layers = 0;
    memset(s_layers, 0, sizeof(s_layers));
    memset(s_cached_step, 0, sizeof(s_cached_step));
    memset(s_track_source_note, 0, sizeof(s_track_source_note));
    s_playing = true;
    s_bpm     = 120;
    s_melodic_patch = SEQ_MEL_PATCH;
    s_quantizer.enabled = CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED;
    s_quantizer.root_note = CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE;
    s_quantizer.scale_index = CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE;
    if (s_quantizer.scale_index >= quantizer_scale_count()) {
        s_quantizer.scale_index = 0;
    }
    sequencer_push_tempo(s_bpm);
    ESP_LOGI(TAG, "sequencer_core initialized");
}

uint8_t sequencer_core_add_layer(seq_layer_type_t type, uint8_t num_steps)
{
    if (s_num_layers >= MAX_LAYERS) {
        ESP_LOGW(TAG, "sequencer_core_add_layer: max layers (%d) reached", MAX_LAYERS);
        return 0xFF;
    }
    uint8_t idx = s_num_layers++;
    seq_layer_t *layer = &s_layers[idx];
    memset(layer, 0, sizeof(seq_layer_t));

    layer->type       = type;
    layer->num_steps  = (num_steps == SEQ_MAX_STEPS) ? SEQ_MAX_STEPS : SEQ_STEPS;
    layer->num_tracks = SEQ_TRACKS;
    layer->step_page  = 0;

    if (type == SEQ_LAYER_DRUM) {
        layer->synth_id    = SEQ_DRUM_SYNTH;
        layer->patch       = SEQ_DRUM_PATCH;
        layer->synth_flags = (uint32_t)(_SYNTH_FLAGS_MIDI_DRUMS
                                        | _SYNTH_FLAGS_IGNORE_NOTE_OFFS);
        layer->num_voices  = SEQ_DRUM_VOICES;
        static const uint8_t drum_notes[SEQ_TRACKS] = {42, 35, 38, 56};
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            s_track_source_note[idx][t] = drum_notes[t];
            layer->track_base_note[t] = drum_notes[t];
            for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = drum_notes[t];
            }
        }
    } else {
        /* Melodic: give each layer its own AMY synth slot, offset past the
         * fixed drum slot so layers never share a synth (and thus voices). */
        uint8_t sid = (uint8_t)(SEQ_DRUM_SYNTH + idx + 1);
        if (sid >= 63) sid = 62; /* stay below AMY max_synths */
        layer->synth_id    = sid;
        layer->patch       = s_melodic_patch;
        layer->synth_flags = 0;
        layer->num_voices  = SEQ_MEL_VOICES;
        /* Default: Cmaj7 voicing — C4 E4 G4 B4 */
        static const uint8_t mel_notes[SEQ_TRACKS] = {60, 64, 67, 71};
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            s_track_source_note[idx][t] = mel_notes[t];
            layer->track_base_note[t] = mel_notes[t];
            for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = mel_notes[t];
            }
            /* Seed each row's envelope from the compile-time defaults. */
            layer->env[t].attack_ms   = CONFIG_SEQ_MELODIC_ENV_ATTACK_MS;
            layer->env[t].decay_ms    = CONFIG_SEQ_MELODIC_ENV_DECAY_MS;
            layer->env[t].sustain_pct = CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT;
            layer->env[t].release_ms  = CONFIG_SEQ_MELODIC_ENV_RELEASE_MS;
            layer->env[t].eg_type     = CONFIG_SEQ_MELODIC_ENV_EG0_TYPE;
        }
        s_active_env_track[idx] = 0;
    }

    sequencer_configure_synth(idx);
    ESP_LOGI(TAG, "add_layer[%d]: type=%d synth=%d patch=%d steps=%d",
             idx, type, layer->synth_id, layer->patch, layer->num_steps);
    return idx;
}

void sequencer_core_set_melodic_patch(uint16_t patch_number)
{
    /* Runtime UI cycling is intentionally constrained to AMY built-in patch IDs.
     * 0..127: Juno, 128..255: DX7, 256: built-in piano. */
    patch_number = SEQ_CLAMP_U16(patch_number, 0, 256);
    if (s_melodic_patch == patch_number) {
        return;
    }

    s_melodic_patch = patch_number;
    for (uint8_t i = 0; i < s_num_layers; i++) {
        seq_layer_t *layer = &s_layers[i];
        if (layer->type != SEQ_LAYER_MELODIC) {
            continue;
        }
        layer->patch = s_melodic_patch;
        sequencer_configure_synth(i);
    }

    ESP_LOGI(TAG, "melodic patch -> %u", (unsigned)s_melodic_patch);
}

uint16_t sequencer_core_get_melodic_patch(void)
{
    return s_melodic_patch;
}

/* ── Per-row melodic envelope (runtime-editable) ─────────────────────────── */

bool sequencer_core_get_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                         seq_env_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    if (s_layers[layer_idx].type != SEQ_LAYER_MELODIC) return false;
    *out = *seq_layer_env(layer_idx, track);
    return true;
}

void sequencer_core_set_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                         const seq_env_t *env)
{
    if (!env || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return;

    seq_env_t *dst = seq_layer_env(layer_idx, track);
    dst->attack_ms   = SEQ_CLAMP_U32(env->attack_ms,  0, 60000);
    dst->decay_ms    = SEQ_CLAMP_U32(env->decay_ms,   0, 60000);
    dst->sustain_pct = SEQ_CLAMP_U8(env->sustain_pct, 0, 100);
    dst->release_ms  = SEQ_CLAMP_U32(env->release_ms, 0, 60000);
    dst->eg_type     = env->eg_type;

    /* This row now owns the shared synth's live envelope. */
    s_active_env_track[layer_idx] = track;
    sequencer_configure_melodic_envelope_track(layer_idx, track);
    ESP_LOGI(TAG, "env L%u row%u -> A%u D%u S%u%% R%u",
             layer_idx, track, (unsigned)dst->attack_ms, (unsigned)dst->decay_ms,
             (unsigned)dst->sustain_pct, (unsigned)dst->release_ms);
}

uint8_t sequencer_core_get_num_layers(void)
{
    return s_num_layers;
}

seq_layer_type_t sequencer_core_get_layer_type(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return SEQ_LAYER_DRUM;
    return s_layers[layer_idx].type;
}

void sequencer_core_set_step(uint8_t layer_idx, uint8_t track,
                              uint8_t step, bool state)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    if (layer->grid[track][step] == state) return;
    layer->grid[track][step] = state;
    sequencer_emit_step(layer_idx, track, step);
}

void sequencer_core_set_bpm(uint16_t new_bpm)
{
    s_bpm = sequencer_clamp_bpm(new_bpm);
    sequencer_push_tempo(s_bpm);
}

/* Derive the currently-playing step from AMY's free-running tick counter:
 * position within the bar divided by ticks-per-step. When paused we return the
 * frozen value captured at pause time so the UI playhead stops in place. */
uint8_t sequencer_core_get_current_step(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return 0;
    if (!s_playing) return s_cached_step[layer_idx];
    seq_layer_t *layer = &s_layers[layer_idx];
    uint32_t bar_ticks = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
    s_cached_step[layer_idx] =
        (uint8_t)((sequencer_ticks() % bar_ticks) / SEQ_TICKS_PER_STEP);
    return s_cached_step[layer_idx];
}

/* Start/stop playback. On start, every step is re-emitted so AMY repopulates
 * its schedule; on stop, each layer's playhead is captured (for a frozen UI)
 * and all scheduled events are cancelled so nothing keeps triggering. */
void sequencer_core_set_playing(bool p)
{
    if (s_playing == p) return;
    s_playing = p;
    if (s_playing) {
        for (uint8_t i = 0; i < s_num_layers; i++) {
            sequencer_resync_layer(i);
        }
    } else {
        /* Freeze display positions before clearing scheduled events. */
        for (uint8_t i = 0; i < s_num_layers; i++) {
            seq_layer_t *layer = &s_layers[i];
            uint32_t bar_ticks = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
            s_cached_step[i] =
                (uint8_t)((sequencer_ticks() % bar_ticks) / SEQ_TICKS_PER_STEP);
            sequencer_clear_layer_tags(i);
        }
    }
}

void sequencer_core_set_track_midi_note(uint8_t layer_idx, uint8_t track,
                                         uint8_t midi_note)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];

    midi_note = sequencer_clamp_layer_note(layer, midi_note);

    s_track_source_note[layer_idx][track] = midi_note;
    sequencer_refresh_track_note(layer_idx, track, true);
}

uint8_t sequencer_core_get_track_midi_note(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    return s_layers[layer_idx].track_base_note[track];
}

uint8_t sequencer_core_get_track_source_note(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    return s_track_source_note[layer_idx][track];
}

void sequencer_core_set_quantizer_enabled(bool enabled)
{
    if (s_quantizer.enabled == enabled) return;
    s_quantizer.enabled = enabled;
    sequencer_refresh_melodic_layers(false);
    ESP_LOGI(TAG, "quantizer %s", enabled ? "enabled" : "disabled");
}

void sequencer_core_set_quantizer_root_note(uint8_t root_note)
{
    s_quantizer.root_note = root_note;
    sequencer_refresh_melodic_layers(false);
    ESP_LOGI(TAG, "quantizer root -> %u", root_note);
}

void sequencer_core_set_quantizer_scale(uint8_t scale_index)
{
    s_quantizer.scale_index = (scale_index >= quantizer_scale_count()) ? 0 : scale_index;
    sequencer_refresh_melodic_layers(false);
    ESP_LOGI(TAG, "quantizer scale -> %u", s_quantizer.scale_index);
}

bool sequencer_core_get_quantizer_enabled(void)
{
    return s_quantizer.enabled;
}

uint8_t sequencer_core_get_quantizer_root_note(void)
{
    return s_quantizer.root_note;
}

uint8_t sequencer_core_get_quantizer_scale(void)
{
    return s_quantizer.scale_index;
}

