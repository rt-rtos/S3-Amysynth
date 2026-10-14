/* Bulk layer snapshot support for project save/load.
 *
 * The per-field setters in sequencer_core.h each re-emit their scheduled steps
 * on every call, and three per-step arrays (step_nudge/step_velocity_adj/
 * step_ratchet_taper) have no public setters at all. A project loader must
 * overwrite a whole layer and settle the AMY side effects exactly once, so this
 * copies the struct wholesale and re-runs the same "configure synth + resync"
 * sequence the layer-management calls use.
 */

#include "sequencer_core.h"
#include "seq_core_internal.h"
#include "seq_core_config.h"
#include <string.h>

bool sequencer_core_export_layer(uint8_t layer_idx, seq_layer_t *out)
{
    if (layer_idx >= s_num_layers || !out) return false;
    *out = s_layers[layer_idx];   /* RAM-to-RAM copy is fine; only flash
                                     serialization must be field-by-field */
    /* track_pcm_preset/track_pcm_mode are stale on the core side (seq_model.h):
     * serialize core truth, or a saved project resets every drum kit to preset
     * 0 on load (import_layer replays these through the setters). */
    if (out->type == SEQ_LAYER_DRUM) {
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            out->track_pcm_preset[t] = sequencer_core_get_drum_pcm_preset(layer_idx, t);
            out->track_pcm_mode[t]   = sequencer_core_get_drum_pcm_mode(layer_idx, t);
        }
    }
    return true;
}

/* Rewrite a track's pitch lane from one note to another over the layer's
 * active steps, the range seq_apply_track_note writes; steps above num_steps
 * and steps holding a different note keep theirs. */
static void seq_relabel_track_note(seq_layer_t *layer, uint8_t t,
                                   uint8_t from, uint8_t to)
{
    if (from == to) return;
    layer->track_base_note[t] = to;
    uint8_t n = layer->num_steps;
    if (n > SEQ_MAX_STEPS) n = SEQ_MAX_STEPS;
    for (uint8_t s = 0; s < n; s++) {
        if (layer->step_note[t][s] == from) layer->step_note[t][s] = to;
    }
}

bool sequencer_core_export_layer_authored(uint8_t layer_idx, seq_layer_t *out)
{
    if (!sequencer_core_export_layer(layer_idx, out)) return false;
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        seq_relabel_track_note(out, t, out->track_base_note[t],
                               s_track_source_note[layer_idx][t]);
    }
    return true;
}

bool sequencer_core_import_layer(uint8_t layer_idx, const seq_layer_t *src)
{
    if (layer_idx >= s_num_layers || !src) return false;
    seq_layer_t *dst = &s_layers[layer_idx];
    if (dst->type != src->type) return false;   /* topology fixed by caller */

    /* Adopt the source's row count. Rows the copy drops stop scheduling and
     * sounding and release their synths first, as a shrink does. */
    uint8_t num_tracks = src->num_tracks;
    if (num_tracks < SEQ_TRACKS_DEFAULT) num_tracks = SEQ_TRACKS_DEFAULT;
    if (num_tracks > SEQ_TRACKS)         num_tracks = SEQ_TRACKS;
    for (uint8_t t = num_tracks; t < dst->num_tracks; t++) {
        sequencer_clear_track_tags(layer_idx, t);
        sequencer_core_trig_clear_track(layer_idx, t);
        sequencer_kill_synth_voices(dst->synth_id[t]);
        sequencer_release_synth(dst->synth_id[t]);
    }

    /* Preserve runtime-owned fields. */
    uint8_t synth_id[SEQ_TRACKS];
    memcpy(synth_id, dst->synth_id, sizeof synth_id);

    *dst = *src;
    memcpy(dst->synth_id, synth_id, sizeof synth_id);
    dst->num_tracks = num_tracks;

    /* The loaded pitch lane is the source (export_layer_authored), then
     * resolved here so the resync below emits the resolved pitches. Keeping
     * the pre-load sources instead would snap every track back to stale
     * pitches on the first re-resolve and strip a loaded chord sentinel.
     * Chord sentinels double as their own source; plain notes also seed the
     * chord-delete fallback. */
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        uint8_t n = dst->track_base_note[t];
        s_track_source_note[layer_idx][t] = n;
        if (!SEQ_NOTE_IS_CHORD(n)) {
            s_track_prev_plain[layer_idx][t] = n;
        }
    }
    uint8_t rows[SEQ_TRACKS];
    seq_resolve_layer_rows(layer_idx, dst->chord_root, dst->chord_type, rows);
    for (uint8_t t = 0; t < num_tracks; t++) {
        seq_relabel_track_note(dst, t, dst->track_base_note[t], rows[t]);
    }

    /* Re-push the full sound config. The public patch setters dedup against the
     * layer's stored patch number, which the struct copy above already
     * overwrote, so they would silently no-op; the configure path instead reads
     * patch/num_voices/synth_flags from the copied struct and pushes the
     * authored env/env1/filter/LFO itself. */
    sequencer_configure_synth(layer_idx);
    if (dst->type == SEQ_LAYER_DRUM) {
        /* Restore each track's PCM mode then preset: mode first so the
         * preset's live-reload (when PCM is active) applies both in one
         * pass; otherwise both just store for the next engine toggle. Rows
         * above num_tracks are stored only. */
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            sequencer_core_set_drum_pcm_mode(layer_idx, t, dst->track_pcm_mode[t]);
            sequencer_core_set_drum_pcm_preset(layer_idx, t, dst->track_pcm_preset[t]);
        }
    }

    sequencer_core_trig_reset_all();
    sequencer_resync_layer(layer_idx);
    return true;
}
