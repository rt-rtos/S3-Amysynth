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

    /* Rebuild the per-track source notes from the loaded resolved notes.
     * s_track_source_note is not persisted, and keeping the pre-load values
     * makes the first re-resolve after load snap every track back to stale
     * pitches - and strips a loaded chord sentinel entirely. The resolved note
     * re-resolves to itself (idempotent snap), so it is a faithful source.
     * Chord sentinels double as their own source; plain notes also seed the
     * chord-delete fallback. */
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        uint8_t n = dst->track_base_note[t];
        s_track_source_note[layer_idx][t] = n;
        if (!SEQ_NOTE_IS_CHORD(n)) {
            s_track_prev_plain[layer_idx][t] = n;
        }
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
