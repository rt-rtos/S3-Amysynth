#include "sequencer_core.h"
#include "amy.h"
#include "sequencer.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "seq_core";

extern uint32_t sequencer_ticks(void);

/* ── Timing ──────────────────────────────────────────────────────────── */
#define SEQ_TICKS_PER_STEP    (AMY_SEQUENCER_PPQ / 4)
#define SEQ_GATE_DRUM         (SEQ_TICKS_PER_STEP / 3)
#define SEQ_GATE_MELODIC      ((SEQ_TICKS_PER_STEP * 2) / 3)
#define SEQ_MIN_BPM           40
#define SEQ_MAX_BPM           300

/* ── Drum synth slot ─────────────────────────────────────────────────── */
#define SEQ_DRUM_SYNTH        10
#define SEQ_DRUM_PATCH        1025
#define SEQ_DRUM_VOICES       6
#define SEQ_MIDI_NOTE_MIN     27
#define SEQ_MIDI_NOTE_MAX     87

/* ── Melodic synth defaults ──────────────────────────────────────────── */
#define SEQ_MEL_PATCH         128   /* DX7 preset 0 — "E Piano 1" */
#define SEQ_MEL_VOICES        4
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
}

/* Emit (or cancel) the ON+OFF repeating events for one step of one layer. */
static void sequencer_emit_step(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    seq_layer_t *layer  = &s_layers[layer_idx];
    uint32_t bar_ticks  = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
    uint8_t  gate       = (layer->type == SEQ_LAYER_DRUM)
                          ? SEQ_GATE_DRUM : SEQ_GATE_MELODIC;
    uint32_t tag_on     = seq_tag_on(layer_idx, track, step);
    uint32_t tag_off    = seq_tag_off(layer_idx, track, step);
    uint32_t tick_on    = (uint32_t)(1 + step * SEQ_TICKS_PER_STEP);
    uint32_t tick_off   = (tick_on + gate) % bar_ticks;
    if (tick_off == 0) tick_off = 1;

    if (!s_playing || !layer->grid[track][step]) {
        sequencer_emit_clear_tag(tag_on);
        sequencer_emit_clear_tag(tag_off);
        return;
    }

    seq_ev_begin();
    s_ev.synth                     = layer->synth_id;
    s_ev.midi_note                 = layer->step_note[track][step];
    s_ev.velocity                  = 1.0f;
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
    if (b < SEQ_MIN_BPM) return SEQ_MIN_BPM;
    if (b > SEQ_MAX_BPM) return SEQ_MAX_BPM;
    return b;
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
    s_playing = true;
    s_bpm     = 120;
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
            layer->track_base_note[t] = drum_notes[t];
            for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = drum_notes[t];
            }
        }
    } else {
        /* Melodic: assign an unused AMY synth slot */
        uint8_t sid = (uint8_t)(SEQ_DRUM_SYNTH + idx + 1);
        if (sid >= 63) sid = 62; /* stay below AMY max_synths */
        layer->synth_id    = sid;
        layer->patch       = SEQ_MEL_PATCH;
        layer->synth_flags = 0;
        layer->num_voices  = SEQ_MEL_VOICES;
        /* Default: Cmaj7 voicing — C4 E4 G4 B4 */
        static const uint8_t mel_notes[SEQ_TRACKS] = {60, 64, 67, 71};
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            layer->track_base_note[t] = mel_notes[t];
            for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = mel_notes[t];
            }
        }
    }

    sequencer_configure_synth(idx);
    ESP_LOGI(TAG, "add_layer[%d]: type=%d synth=%d patch=%d steps=%d",
             idx, type, layer->synth_id, layer->patch, layer->num_steps);
    return idx;
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

    /* Clamp to the note range valid for this layer type. */
    if (layer->type == SEQ_LAYER_DRUM) {
        if (midi_note < SEQ_MIDI_NOTE_MIN) midi_note = SEQ_MIDI_NOTE_MIN;
        if (midi_note > SEQ_MIDI_NOTE_MAX) midi_note = SEQ_MIDI_NOTE_MAX;
    } else {
        if (midi_note < SEQ_MEL_NOTE_MIN) midi_note = SEQ_MEL_NOTE_MIN;
        if (midi_note > SEQ_MEL_NOTE_MAX) midi_note = SEQ_MEL_NOTE_MAX;
    }
    if (layer->track_base_note[track] == midi_note) return;

    layer->track_base_note[track] = midi_note;
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        layer->step_note[track][s] = midi_note;
    }

    /* Reschedule all active steps for this track. */
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_step(layer_idx, track, s);
    }

    /* One-shot preview: fires a few ticks from now using the same tag slot.
     * Rapid scrolling overwrites the slot so only the last change is heard. */
    uint32_t fire_tick = sequencer_ticks() + SEQ_PREVIEW_DELAY_TICKS;
    seq_ev_begin();
    s_ev.synth                     = layer->synth_id;
    s_ev.midi_note                 = midi_note;
    s_ev.velocity                  = 1.0f;
    s_ev.sequence[SEQUENCE_TAG]    = seq_preview_tag(layer_idx, track);
    s_ev.sequence[SEQUENCE_TICK]   = fire_tick;
    s_ev.sequence[SEQUENCE_PERIOD] = 0; /* one-shot */
    seq_ev_send();

    /* Note-off one-shot: always schedule even for drum layers (harmless there
     * due to _SYNTH_FLAGS_IGNORE_NOTE_OFFS); essential for melodic so the note
     * doesn't sustain indefinitely.  Use SEQ_GATE_MELODIC ticks of hold time. */
    seq_ev_begin();
    s_ev.synth                     = layer->synth_id;
    s_ev.midi_note                 = midi_note;
    s_ev.velocity                  = 0.0f;
    s_ev.sequence[SEQUENCE_TAG]    = seq_preview_off_tag(layer_idx, track);
    s_ev.sequence[SEQUENCE_TICK]   = fire_tick + SEQ_GATE_MELODIC;
    s_ev.sequence[SEQUENCE_PERIOD] = 0; /* one-shot */
    seq_ev_send();

    ESP_LOGI(TAG, "layer %d track %d note -> %d (preview @ tick %lu)",
             layer_idx, track, midi_note, (unsigned long)fire_tick);
}

uint8_t sequencer_core_get_track_midi_note(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    return s_layers[layer_idx].track_base_note[track];
}


#define SEQ_TRACKS 4
#define SEQ_STEPS 16
#define SEQ_TICKS_PER_STEP (AMY_SEQUENCER_PPQ / 4)
#define SEQ_BAR_TICKS (SEQ_STEPS * SEQ_TICKS_PER_STEP)
#define SEQ_GATE_TICKS (SEQ_TICKS_PER_STEP / 3)
#define SEQ_MIN_BPM 40
#define SEQ_MAX_BPM 300
#define SEQ_DRUM_SYNTH 10
#define SEQ_DRUM_PATCH 1025
#define SEQ_DRUM_VOICES 6
#define SEQ_MIDI_NOTE_MIN 27    // lowest valid GM percussion note
#define SEQ_MIDI_NOTE_MAX 87    // highest valid GM percussion note
// Preview tags live above the on/off tag space (4 tracks * 16 steps * 2 = 128)
#define SEQ_PREVIEW_TAG_BASE (SEQ_TRACKS * SEQ_STEPS * 2)
#define SEQ_PREVIEW_DELAY_TICKS 4  // fire ~42 ms ahead at 120 BPM

