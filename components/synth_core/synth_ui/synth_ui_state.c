#include "synth_ui/synth_ui_internal.h"
#include "sequencer_core.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "synth_ui";

/* ── Sequencer UI state ─────────────────────────────────────────────────── */

#if CONFIG_SEQ_STATE_IN_PSRAM
synth_ui_state_t *seq_state_ptr = NULL;   /* allocated in synth_ui_state_alloc() */
#else
static synth_ui_state_t seq_state_storage;
synth_ui_state_t *seq_state_ptr = &seq_state_storage;
#endif

/* Allocate (PSRAM path) and apply the boot defaults that used to live in the
 * static initializer - everything not set here is correct at zero. Must run
 * before any seq_state access; synth_ui_init() calls it first. */
void synth_ui_state_alloc(void)
{
#if CONFIG_SEQ_STATE_IN_PSRAM
    if (seq_state_ptr == NULL) {
        seq_state_ptr = heap_caps_calloc(1, sizeof(synth_ui_state_t),
                                         MALLOC_CAP_SPIRAM);
        if (seq_state_ptr == NULL) {
            ESP_LOGW(TAG, "seq_state: PSRAM alloc failed, using internal heap");
            seq_state_ptr = heap_caps_calloc(1, sizeof(synth_ui_state_t),
                                             MALLOC_CAP_DEFAULT);
        }
        if (seq_state_ptr == NULL) {
            /* Core UI state with no degrade path: fail loudly at boot rather
             * than NULL-deref on the first field access. */
            ESP_LOGE(TAG, "seq_state: allocation failed (%u B)",
                     (unsigned)sizeof(synth_ui_state_t));
            abort();
        }
    }
#endif
    seq_state.current_pattern = 1;
    seq_state.playing         = true;
    seq_state.edit_mode       = true;
}

/* Mirror the UI's grid of active steps into the audio core so the core
 * schedules notes for every step the user has toggled on. Only "on" steps are
 * pushed; "off" steps are the core's default after a fresh layer add. */
void sync_layer_to_core(uint8_t li)
{
    seq_layer_t *layer = &seq_state.layers[li];
    for (int t = 0; t < layer->num_tracks; t++) {
        for (int s = 0; s < layer->num_steps; s++) {
            if (layer->grid[t][s]) {
                sequencer_core_set_step(li, t, s, true);
            }
        }
    }
}

/* Re-sync the UI mirror from the core after a project load has rewritten layer
 * topology/content out from under the UI. Also folds in the transport-stop
 * mirror update, since project_snapshot_load() already stopped the core
 * transport just before this runs. */
void synth_ui_reload_mirror_from_core(void)
{
    seq_state.num_layers = sequencer_core_get_num_layers();
    for (uint8_t i = 0; i < seq_state.num_layers; i++) {
        sequencer_core_export_layer(i, &seq_state.layers[i]);
    }
    seq_state.active_layer_idx = 0;
    seq_state.selected_track   = 0;
    seq_state.selected_step    = 0;
    seq_state.playing          = false;
    /* Match the boot default: edit_mode gates what a bare encoder turn/push
     * does, and post-load must behave exactly like post-boot. */
    seq_state.edit_mode        = true;
    s_force_redraw = true;
    ESP_LOGI(TAG, "UI mirror reloaded from core: %u layer(s)", seq_state.num_layers);
}

/* ── Note-name helper (local; mirrors display_seq.c's static one) ─────── */
void ui_note_name(uint8_t midi_note, char buf[4])
{
    /* Chord preset sentinel: the "note" is a chord slot, shown as CHn. */
    if (SEQ_NOTE_IS_CHORD(midi_note)) {
        snprintf(buf, 4, "CH%u", (unsigned)(SEQ_CHORD_INDEX(midi_note) + 1u));
        return;
    }
    static const char *const names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    int octave = (int)midi_note / 12 - 1;
    snprintf(buf, 4, "%s%d", names[midi_note % 12], octave);
}

/* ── Render-on-change: sequencer view signature ──────────────────────── */

/* Signature of the sequencer view (everything display_seq_draw_frame reads). */
uint32_t seq_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &seq_state.active_layer_idx, sizeof(seq_state.active_layer_idx));
    { uint16_t bpm_snap = sequencer_core_get_bpm(); h = fnv1a_bytes(h, &bpm_snap, sizeof(bpm_snap)); }
    h = fnv1a_bytes(h, &seq_state.playing, sizeof(seq_state.playing));
    h = fnv1a_bytes(h, &seq_state.current_step, sizeof(seq_state.current_step));
    h = fnv1a_bytes(h, &seq_state.edit_mode, sizeof(seq_state.edit_mode));
    h = fnv1a_bytes(h, &seq_state.selected_track, sizeof(seq_state.selected_track));
    h = fnv1a_bytes(h, &seq_state.selected_step, sizeof(seq_state.selected_step));
    h = fnv1a_bytes(h, &seq_state.drum_select_mode, sizeof(seq_state.drum_select_mode));
    h = fnv1a_bytes(h, &seq_state.patch_select_mode, sizeof(seq_state.patch_select_mode));
    /* Drum engine + PCM presets live in the core (sample_rec can change them
     * without going through the UI), so snapshot them into the mirror each
     * frame, like the bpm snapshot above, and hash the mirror. */
    seq_state.drum_pcm = (sequencer_core_get_drum_engine() == SEQ_DRUM_PCM);
    h = fnv1a_bytes(h, &seq_state.drum_pcm, sizeof(seq_state.drum_pcm));
    if (seq_state.num_layers > 0) {
        seq_layer_t *L = &seq_state.layers[seq_state.active_layer_idx];
        if (L->type == SEQ_LAYER_DRUM) {
            for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
                L->track_pcm_preset[t] =
                    sequencer_core_get_drum_pcm_preset(seq_state.active_layer_idx, t);
                L->track_pcm_mode[t] =
                    sequencer_core_get_drum_pcm_mode(seq_state.active_layer_idx, t);
            }
            h = fnv1a_bytes(h, L->track_pcm_preset, sizeof(L->track_pcm_preset));
            h = fnv1a_bytes(h, L->track_pcm_mode, sizeof(L->track_pcm_mode));
        }
        h = fnv1a_bytes(h, &L->type, sizeof(L->type));
        h = fnv1a_bytes(h, &L->patch, sizeof(L->patch));
        h = fnv1a_bytes(h, L->track_patch, sizeof(L->track_patch));
        h = fnv1a_bytes(h, &L->patch_scope, sizeof(L->patch_scope));
        h = fnv1a_bytes(h, &L->num_steps, sizeof(L->num_steps));
        h = fnv1a_bytes(h, &L->num_tracks, sizeof(L->num_tracks));
        h = fnv1a_bytes(h, L->track_base_note, sizeof(L->track_base_note));
        h = fnv1a_bytes(h, L->grid, sizeof(L->grid));
    }
    return h;
}

/* ── Public accessors ─────────────────────────────────────────────────── */

uint16_t seq_get_bpm(void)
{
    return sequencer_core_get_bpm();
}

uint8_t seq_get_active_layer_idx(void)
{
    return seq_state.active_layer_idx;
}
