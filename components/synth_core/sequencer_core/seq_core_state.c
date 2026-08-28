#include "sequencer_core/seq_core_internal.h"
#include "voice_config.h"
#include "esp_heap_caps.h"
#include <assert.h>

/* ── State definitions — owns the core layer table and play state ─── */
#if CONFIG_SEQ_STATE_IN_PSRAM
/* Allocated once in sequencer_core_init(), PSRAM-first with internal
 * fallback. NULL only before init; the tick path never dereferences the
 * table while s_num_layers == 0. */
seq_layer_t *s_layers = NULL;
#else
static seq_layer_t s_layers_storage[MAX_LAYERS];
seq_layer_t *s_layers = s_layers_storage;
#endif
uint8_t     s_num_layers   = 0;
bool        s_playing      = true;
/* Guards delete_layer()'s ~4 KB s_layers compaction against the sequencer tick,
 * which reads s_layers[] live: the tick early-returns while this is set. */
volatile bool s_layers_mutating = false;

/* Single-applier enforcement for structural s_layers edits. Once boot init
 * registers the applier (synth_ui_task, which drains the deferred add/delete
 * requests), debug builds assert every structural edit runs there. Complements
 * s_layers_mutating: that guard fences the one concurrent READER, this pins all
 * WRITERS to one task, the same discipline s_prog_apply_pending uses. */
static TaskHandle_t s_layers_applier = NULL;

void sequencer_core_set_layers_applier(TaskHandle_t applier)
{
    s_layers_applier = applier;
}

/* Skipped until a handle is registered, so single-threaded boot init passes. */
static inline void seq_assert_layers_applier(void)
{
#if !defined(NDEBUG)
    configASSERT(s_layers_applier == NULL ||
                 xTaskGetCurrentTaskHandle() == s_layers_applier);
#endif
}
/* Default seed + display fallback, NOT an authoritative global; dual-writer
 * contract at the declaration in seq_core_internal.h. */
uint16_t    s_melodic_patch = SEQ_MEL_PATCH;
/* Running allocator for per-row melodic synth slots. Each melodic layer claims
 * a contiguous block of SEQ_TRACKS slots starting here; reset in core_init. */
uint8_t     s_next_melodic_synth = SEQ_MEL_SYNTH_BASE;

/* Default per-track SYNTH patches by role: raw patch numbers chosen for a
 * 4-on-floor kit. */
static const uint16_t SEQ_DRUM_DEFAULT_PATCH[SEQ_TRACKS] = {
    58,   /* kick  - Juno Drum Booms, thumpy at low pitch */
    245,  /* snare - DX7 B.DRM-SNAR                       */
    221,  /* hat   - DX7 BLOCK, tight tick at high pitch  */
    220,  /* perc  - DX7 COW BELL accent                  */
};

void sequencer_core_init(void)
{
    amy_helpers_init();
    sequencer_core_trig_pump_init();
    s_num_layers = 0;
    s_next_melodic_synth = SEQ_MEL_SYNTH_BASE;
#if CONFIG_SEQ_STATE_IN_PSRAM
    if (s_layers == NULL) {
        s_layers = heap_caps_malloc(MAX_LAYERS * sizeof(seq_layer_t),
                                    MALLOC_CAP_SPIRAM);
        if (s_layers == NULL) {
            ESP_LOGW(TAG, "s_layers: PSRAM alloc failed, using internal heap");
            s_layers = heap_caps_malloc(MAX_LAYERS * sizeof(seq_layer_t),
                                        MALLOC_CAP_DEFAULT);
        }
        if (s_layers == NULL) {
            /* Core state with no degrade path: fail loudly at boot rather
             * than NULL-deref on the first layer access. */
            ESP_LOGE(TAG, "s_layers: allocation failed (%u B)",
                     (unsigned)(MAX_LAYERS * sizeof(seq_layer_t)));
            abort();
        }
    }
#endif
    memset(s_layers, 0, MAX_LAYERS * sizeof(seq_layer_t));
    memset(s_cached_step, 0, MAX_LAYERS * sizeof(s_cached_step[0]));
    memset(s_track_source_note, 0, MAX_LAYERS * SEQ_TRACKS * sizeof(s_track_source_note[0][0]));
    memset(s_track_prev_plain, 0, MAX_LAYERS * SEQ_TRACKS * sizeof(s_track_prev_plain[0][0]));
    memset(s_voices_applied, 0, MAX_LAYERS * SEQ_TRACKS * sizeof(s_voices_applied[0][0]));
    s_playing = true;
    s_bpm     = SEQ_DEFAULT_BPM;
    s_melodic_patch = SEQ_MEL_PATCH;
    s_quantizer.enabled = CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED;
    s_quantizer.root_note = CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE;
    s_quantizer.scale_index = CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE;
    if (s_quantizer.scale_index >= quantizer_scale_count()) {
        s_quantizer.scale_index = 0;
    }
    memset(s_lfo_phase, 0, MAX_LAYERS * SEQ_TRACKS * sizeof(s_lfo_phase[0][0]));
    memset(s_lfo_hz,    0, MAX_LAYERS * SEQ_TRACKS * sizeof(s_lfo_hz[0][0]));
    memset(s_lfo_rnd,   0, MAX_LAYERS * SEQ_TRACKS * sizeof(s_lfo_rnd[0][0]));
    DIAG_HEAP_CHECK("core_init: before push_tempo");
    sequencer_push_tempo(s_bpm);
    DIAG_HEAP_CHECK("core_init: after push_tempo");
    ESP_LOGI(TAG, "sequencer_core initialized");
}

uint8_t sequencer_core_add_layer(seq_layer_type_t type, uint8_t num_steps)
{
    seq_assert_layers_applier();
    if (s_num_layers >= MAX_LAYERS) {
        ESP_LOGW(TAG, "sequencer_core_add_layer: max layers (%d) reached", MAX_LAYERS);
        return 0xFF;
    }
    /* Claim the slot but do NOT expose it via s_num_layers yet: the tick
     * iterates 0..s_num_layers-1 and would see a half-initialised layer.
     * s_num_layers++ waits until sequencer_configure_synth() completes. */
    uint8_t idx = s_num_layers;
    seq_layer_t *layer = &s_layers[idx];
    memset(layer, 0, sizeof(seq_layer_t));

    /* Per-row voice params: unauthored with amp_trim at unity. The single
     * defaults source, so no field here needs post-memset repair. */
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        voice_params_init_defaults(&layer->vp[t]);
    }
    voice_params_init_defaults(&layer->vp_layer);
    /* vp_src[] is TRACK (0) from the memset: every row reads its own block. */

    layer->type       = type;
    layer->num_steps  = (num_steps == SEQ_MAX_STEPS) ? SEQ_MAX_STEPS : SEQ_STEPS;
    layer->num_tracks = SEQ_TRACKS;
    layer->step_page  = 0;
    /* NoteFX defaults. Required after the memset: a 0% gate would silence
     * every note and a 0% groove would flatten dynamics. */
    layer->gate_pct       = SEQ_MELODIC_GATE_DEFAULT_PCT;
    layer->portamento_ms  = 0;
    layer->groove_pct     = 100;   /* full accent curve */
    /* Required after the memset too: 0 is a real FM algorithm. */
    layer->fm_algo_override = SEQ_FM_ALGO_NONE;

    if (type == SEQ_LAYER_DRUM) {
        /* Per-track drum layer: each track gets a fixed synth slot from the
         * reserved block and its own patch from the curated list, with
         * note-offs honored (synth_flags = 0) so the patch's release shapes
         * the tail. */
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            layer->synth_id[t]   = (uint8_t)(SEQ_DRUM_SYNTH_BASE + t);
            layer->track_patch[t] = SEQ_DRUM_DEFAULT_PATCH[t];
        }
        layer->patch       = layer->track_patch[0];  /* display fallback */
        layer->synth_flags = 0;
        layer->num_voices  = SEQ_DRUM_VOICES;
        /* Role-based pitches: pitch IS timbre for these tuned patches, and also
         * tunes the samples in PCM mode (render_pcm shifts by midi_note).
         * Seeded from the boot bank's ear-tuned notes[], the same source the
         * bank selector re-seeds from. */
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            uint8_t n = sequencer_drum_default_note(t);
            s_track_source_note[idx][t] = n;
            s_track_prev_plain[idx][t]  = n;
            layer->track_base_note[t] = n;
            for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = n;
            }
        }
    } else {
        /* Melodic: claim a contiguous block of SEQ_TRACKS slots from the running
         * allocator, one synth per row, so identical pitches on different rows
         * land in distinct instruments instead of collapsing into one voice.
         * Past AMY's synth ceiling, reuse the last valid block - shared synths
         * degrade the sound but never corrupt state. */
        uint8_t base = s_next_melodic_synth;
        if (base + SEQ_TRACKS - 1 > SEQ_MAX_SYNTH) {
            base = (uint8_t)(SEQ_MAX_SYNTH - (SEQ_TRACKS - 1));
            ESP_LOGW(TAG, "add_layer[L%d]: melodic synth ceiling reached, "
                          "reusing slots %u..%u", idx + 1, base, base + SEQ_TRACKS - 1);
        } else {
            s_next_melodic_synth = (uint8_t)(base + SEQ_TRACKS);
        }
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            layer->synth_id[t] = (uint8_t)(base + t);
        }
        layer->patch       = s_melodic_patch;
        layer->synth_flags = 0;
        layer->num_voices  = SEQ_MEL_VOICES;
        /* Default: Cmaj7 voicing - C4 E4 G4 B4 */
        static const uint8_t mel_notes[SEQ_TRACKS] = {60, 64, 67, 71};
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            s_track_source_note[idx][t] = mel_notes[t];
            s_track_prev_plain[idx][t]  = mel_notes[t];
            layer->track_base_note[t] = mel_notes[t];
            for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = mel_notes[t];
            }
            layer->vp[t].env  = seq_default_melodic_env();
            layer->vp[t].env1 = seq_default_melodic_env1();
        }
        layer->vp_layer.env  = seq_default_melodic_env();
        layer->vp_layer.env1 = seq_default_melodic_env1();
    }

    /* step_prob (0% silences every step), step_ratchet (0 sub-hits fire
     * nothing) and step_every (canonical neutral is 1, though the trig engine
     * treats 0 the same defensively) need explicit non-zero defaults. The
     * prev and transform arrays are correct at their zeroed no-op. */
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
            layer->step_prob[t][s]    = 100;
            layer->step_ratchet[t][s] = 1;
            layer->step_every[t][s]   = 1;
        }
    }

    DIAG_HEAP_CHECK("add_layer: before configure_synth");
    sequencer_configure_synth(idx);
    DIAG_HEAP_CHECK("add_layer: after configure_synth");
    /* Fully initialised: now expose it to the tick path. */
    s_num_layers++;
    /* Check the permanent-drum-layer invariant here once rather than trusting
     * it throughout: slot 0 is the drum layer and every later add is above it. */
    assert(s_layers[SEQ_DRUM_LAYER_IDX].type == SEQ_LAYER_DRUM);
    /* Runtime trig bookkeeping is indexed by layer SLOT, not identity, so a
     * wholesale reset is simpler than shifting it in lockstep and costs only
     * FILL/PREV continuity - never the persisted per-step data in
     * seq_layer_t. */
    sequencer_core_trig_reset_all();
    ESP_LOGI(TAG, "add_layer[L%d]: type=%d synth0=%d patch=%d steps=%d",
             idx + 1, type, layer->synth_id[0], layer->patch, layer->num_steps);
    return idx;
}

bool sequencer_core_delete_layer(uint8_t layer_idx)
{
    seq_assert_layers_applier();
    if (s_num_layers <= 1) return false;                  /* must keep at least 1 */
    if (layer_idx == SEQ_DRUM_LAYER_IDX) return false;    /* drum layer is permanent */
    if (layer_idx >= s_num_layers) return false;
    if (s_layers[layer_idx].type == SEQ_LAYER_DRUM) return false;

    /* Clear ALL layers' tags before shifting: indices above layer_idx go stale
     * on compaction and would fire as ghost notes. Ratchet one-shot tags live
     * in a separate tag space and need the same treatment. */
    for (uint8_t i = 0; i < s_num_layers; i++) {
        sequencer_clear_layer_tags(i);
        sequencer_core_trig_clear_all(i);
    }

    /* Release AMY oscillator slots for the deleted layer. */
    const seq_layer_t *dead = &s_layers[layer_idx];
    for (uint8_t t = 0; t < dead->num_tracks; t++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth      = dead->synth_id[t];
        e->num_voices = 0;
        amy_helpers_event_send(e);
    }

    /* Raise the mutation guard so the sequencer tick early-returns instead of
     * reading s_layers[] mid-memmove. Skipped ticks are inaudible - trig state
     * is reset wholesale below. */
    s_layers_mutating = true;
    /* Compact all parallel arrays by shifting survivors down by one slot. */
    uint8_t tail = (uint8_t)(s_num_layers - layer_idx - 1);
    if (tail > 0) {
        memmove(&s_layers[layer_idx],
                &s_layers[layer_idx + 1],
                tail * sizeof(s_layers[0]));
        memmove(&s_cached_step[layer_idx],
                &s_cached_step[layer_idx + 1],
                tail * sizeof(s_cached_step[0]));
        memmove(&s_track_source_note[layer_idx],
                &s_track_source_note[layer_idx + 1],
                tail * sizeof(s_track_source_note[0]));
        memmove(&s_track_prev_plain[layer_idx],
                &s_track_prev_plain[layer_idx + 1],
                tail * sizeof(s_track_prev_plain[0]));
        memmove(&s_voices_applied[layer_idx],
                &s_voices_applied[layer_idx + 1],
                tail * sizeof(s_voices_applied[0]));
        memmove(&s_lfo_phase[layer_idx],
                &s_lfo_phase[layer_idx + 1],
                tail * sizeof(s_lfo_phase[0]));
        memmove(&s_lfo_hz[layer_idx],
                &s_lfo_hz[layer_idx + 1],
                tail * sizeof(s_lfo_hz[0]));
        memmove(&s_lfo_rnd[layer_idx],
                &s_lfo_rnd[layer_idx + 1],
                tail * sizeof(s_lfo_rnd[0]));
    }
    s_num_layers--;
    /* Only the permanent drum layer left: every melodic slot is free again
     * (AMY released each deleted layer's voices above), so rewind the bump
     * allocator. Without this, project loads - which delete down to the drum
     * layer and re-add - creep the counter 4 slots per melodic layer per load
     * until the arena ceiling's shared-slot degrade kicks in. */
    if (s_num_layers == 1) s_next_melodic_synth = SEQ_MEL_SYNTH_BASE;
    /* Table is fully compacted and the count updated; drop the guard. */
    s_layers_mutating = false;
    sequencer_core_trig_reset_all();  /* see rationale in sequencer_core_add_layer() */

    /* Resync all surviving layers so their note tags re-register correctly. */
    if (s_playing) {
        for (uint8_t i = 0; i < s_num_layers; i++) {
            sequencer_resync_layer(i);
        }
    }

    ESP_LOGI(TAG, "delete_layer[L%u]: %u layers remain", layer_idx + 1u, s_num_layers);
    return true;
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
