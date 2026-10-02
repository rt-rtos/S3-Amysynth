#include "sequencer_core/seq_core_internal.h"
#include "seq_clamp.h"
#include "custompatches/clip_player.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"

/* ── State definitions — owns step cache, source notes, bar baseline ── */
uint8_t  s_cached_step[MAX_LAYERS];
uint8_t  s_track_source_note[MAX_LAYERS][SEQ_TRACKS];
uint32_t s_bar_baseline = 0;
/* Last plain source note per track - chord-slot-delete fallback (see
 * seq_core_internal.h). */
uint8_t  s_track_prev_plain[MAX_LAYERS][SEQ_TRACKS];

/* ── Bar counter ─────────────────────────────────────────────────────────
 * sequencer_ticks() is monotonic (never resets on play/stop in normal use).
 * Capture a baseline at play-start; compute bars elapsed from the delta. */
uint32_t sequencer_bars_elapsed_ahead(uint32_t ahead_ticks)
{
    uint32_t t = sequencer_ticks() + ahead_ticks;
    if (t < s_bar_baseline) return 0;
    return (t - s_bar_baseline) / SEQ_TICKS_PER_BAR;
}

uint32_t sequencer_bars_elapsed(void)
{
    return sequencer_bars_elapsed_ahead(0);
}

uint32_t sequencer_core_next_bar_tick(void)
{
    if (!s_playing) return 0;
    uint32_t t = sequencer_ticks();
    return (t / SEQ_TICKS_PER_BAR + 1u) * SEQ_TICKS_PER_BAR;
}

_Atomic uint32_t s_freeze_tick = 0;
_Atomic bool     s_freeze_set  = false;

void sequencer_core_freeze_set(uint32_t end_tick)
{
    atomic_store_explicit(&s_freeze_tick, end_tick, memory_order_relaxed);
    atomic_store_explicit(&s_freeze_set, true, memory_order_release);
    sequencer_set_periodic_horizon(end_tick);
}

void sequencer_core_freeze_clear(void)
{
    sequencer_clear_periodic_horizon();
    atomic_store_explicit(&s_freeze_set, false, memory_order_release);
}

/* Pump task. The note-offs apply at the next block start, one block after
 * the horizon: notes sounding across the bar line release there, the same
 * cut a mute makes. Drone slots are included on purpose (unlike the
 * transport stop): the bounce's commit disables them right after. */
void sequencer_core_freeze_release_apply(void)
{
    for (uint8_t i = 0; i < s_num_layers; i++) {
        seq_layer_t *layer = &s_layers[i];
        for (uint8_t t = 0; t < layer->num_tracks; t++) {
            sequencer_kill_synth_voices(layer->synth_id[t]);
        }
    }
    sequencer_kill_synth_voices(SEQ_ARP_SYNTH);
    sequencer_kill_synth_voices(DRONE_SYNTH_MAIN);
    sequencer_kill_synth_voices(DRONE_SYNTH_SUB);
    sequencer_kill_synth_voices(DRONE_STD_SYNTH_MAIN);
    sequencer_kill_synth_voices(DRONE_STD_SYNTH_SUB);
}

void sequencer_core_mute_all_tracks(void)
{
    for (uint8_t i = 0; i < s_num_layers; i++) {
        uint8_t n = s_layers[i].num_tracks;
        for (uint8_t t = 0; t < n; t++) {
            sequencer_core_set_track_mute(i, t, true);
        }
    }
}

void sequencer_core_get_mute_masks(uint8_t out[MAX_LAYERS])
{
    for (uint8_t i = 0; i < MAX_LAYERS; i++) {
        uint8_t m = 0;
        if (i < s_num_layers) {
            for (uint8_t t = 0; t < s_layers[i].num_tracks && t < 8; t++) {
                if (s_layers[i].mute[t]) m |= (uint8_t)(1u << t);
            }
        }
        out[i] = m;
    }
}

void sequencer_core_set_mute_masks(const uint8_t in[MAX_LAYERS])
{
    for (uint8_t i = 0; i < s_num_layers && i < MAX_LAYERS; i++) {
        for (uint8_t t = 0; t < s_layers[i].num_tracks && t < 8; t++) {
            sequencer_core_set_track_mute(i, t, (in[i] & (1u << t)) != 0);
        }
    }
}

uint8_t sequencer_core_pattern_period_bars(void)
{
    uint32_t bars = 0;
    if (sequencer_core_progression_get_enabled()) {
        uint8_t n = sequencer_core_progression_get_count();
        for (uint8_t i = 0; i < n; i++) {
            uint8_t root, dur;
            chord_type_t ct;
            sequencer_core_progression_get_entry(i, &root, &ct, &dur);
            bars += dur;
        }
    }
    if (bars == 0) {
        for (uint8_t i = 0; i < s_num_layers; i++) {
            const seq_layer_t *layer = &s_layers[i];
            uint32_t step_bars = ((uint32_t)layer->num_steps + 15u) / 16u;
            if (step_bars == 0) step_bars = 1;
            for (uint8_t t = 0; t < layer->num_tracks; t++) {
                uint32_t rr = (layer->repeat_rate[t] >= SEQ_REPEAT_2)
                              ? (uint32_t)layer->repeat_rate[t] : 1u;
                if (step_bars * rr > bars) bars = step_bars * rr;
            }
        }
    }
    if (bars == 0) bars = 1;
    if (bars > 255) bars = 255;
    return (uint8_t)bars;
}

/* ── Tag helpers ─────────────────────────────────────────────────────── */
/*
 * Tag layout (uint32_t, so tag space is effectively unlimited):
 *
 *   ON  tag = layer * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
 *             + track * SEQ_MAX_STEPS + step
 *
 *   OFF tag = ON tag + (SEQ_TRACKS * SEQ_MAX_STEPS)
 *
 *   Preview = MAX_LAYERS * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
 *             + layer * SEQ_TRACKS + track
 *
 * The layers' ON/OFF tags come first, then the preview tags, which the
 * _Static_assert below keeps under the arp's tag space. Whole-space layout:
 * seq_core_config.h.
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

/* OFF tag for the preview note: the block immediately after the ON tags. */
static inline uint32_t seq_preview_off_tag(uint8_t layer, uint8_t track)
{
    return seq_preview_tag(layer, track) + (uint32_t)(MAX_LAYERS * SEQ_TRACKS);
}

/* The highest preview OFF tag, seq_preview_off_tag(MAX_LAYERS-1, SEQ_TRACKS-1),
 * must stay below the arp's tag space. */
_Static_assert(MAX_LAYERS * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
               + (MAX_LAYERS - 1) * SEQ_TRACKS + (SEQ_TRACKS - 1)
               + MAX_LAYERS * SEQ_TRACKS < SEQ_ARP_TAG_BASE,
               "sequencer preview tags overlap the arp tag space");

/* Chord edit-preview tags: extra-tone (1..SEQ_CHORD_MAX_NOTES-1) on/off pairs
 * for the one-shot preview path; tone 0 stays on the preview pair above.
 * Layout: seq_core_config.h chord tag space. */
static inline uint32_t seq_chord_preview_on_tag(uint8_t layer, uint8_t track,
                                                uint8_t tone /* 1.. */)
{
    return SEQ_CHORD_PREVIEW_TAG_BASE
         + (((uint32_t)layer * SEQ_TRACKS + track) * (SEQ_CHORD_MAX_NOTES - 1)
            + (uint32_t)(tone - 1)) * 2u;
}

static inline uint32_t seq_chord_preview_off_tag(uint8_t layer, uint8_t track,
                                                 uint8_t tone /* 1.. */)
{
    return seq_chord_preview_on_tag(layer, track, tone) + 1u;
}

/* Clear the extra-tone preview pairs from `first_tone` up. Run on every chord
 * preview - a shrink between two previews must not let a pending higher-tone
 * pair fire - and when a track's resolved note leaves the chord zone. */
static void seq_chord_preview_clear_from(uint8_t layer, uint8_t track,
                                         uint8_t first_tone)
{
    for (uint8_t tone = first_tone ? first_tone : 1; tone < SEQ_CHORD_MAX_NOTES; tone++) {
        sequencer_emit_clear_tag(seq_chord_preview_on_tag(layer, track, tone));
        sequencer_emit_clear_tag(seq_chord_preview_off_tag(layer, track, tone));
    }
}

/* ── Chord expansion (shared with seq_core_trig.c) ─────────────────────── */

/* Progression transpose for chord presets. Presets are authored as the "I"
 * voicing (progression entry 0); each advance moves them by the delta between
 * the live chord root and entry 0's root - a rigid transpose, never per-tone
 * re-quantization. With the progression off, chords play exactly as entered.
 * Computed at fire time, so an advance re-pitches with no re-emit plumbing. */
int sequencer_chord_transpose_root(const seq_layer_t *layer, uint8_t root)
{
    if (!s_prog.enabled || s_prog.count == 0) return 0;
    if (!layer->chord_mode) return 0;
    return (int)root - (int)s_prog.entries[0].root;
}

uint8_t seq_track_fire_notes_root(const seq_layer_t *layer, uint8_t stored_note,
                                  uint8_t root, uint8_t out[SEQ_CHORD_MAX_NOTES])
{
    if (!SEQ_NOTE_IS_CHORD(stored_note)) {
        out[0] = stored_note;
        return 1;
    }
    return seq_chords_resolve(SEQ_CHORD_INDEX(stored_note),
                              sequencer_chord_transpose_root(layer, root), out);
}

uint8_t seq_track_fire_notes(const seq_layer_t *layer, uint8_t stored_note,
                             uint8_t out[SEQ_CHORD_MAX_NOTES])
{
    return seq_track_fire_notes_root(layer, stored_note, layer->chord_root, out);
}

float sequencer_step_velocity(const seq_layer_t *layer,
                              uint8_t track, uint8_t step)
{
    /* Drums share the melodic accent+jitter curve rather than a flat 1.0. */
    (void)layer;

#if !CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS
    (void)track;
    (void)step;
    return 1.0f;
#else

    /* Base level sits mid-range so accents have room to push up and ghost notes
     * to drop down; tracks are spread slightly so stacked voices don't hit
     * identically. */
    float velocity = 0.62f + (0.02f * (float)track);

    /* Metric accents: strong downbeat, lighter backbeat, weak off-beats. */
    if ((step % 4) == 0) {
        velocity += 0.30f; /* downbeat of each quarter-note */
    } else if ((step % 4) == 2) {
        velocity += 0.16f; /* backbeat emphasis */
    } else {
        velocity -= 0.04f; /* in-between 8ths sit back as ghost notes */
    }

    /* Deterministic per-step jitter (light humanization) so repeated bars are
     * not bit-identical. Cycles every 4 steps. */
    static const float jitter[4] = { 0.015f, -0.02f, 0.01f, -0.015f };
    velocity += jitter[step & 3];

    velocity = SEQ_CLAMP_F32(velocity, 0.45f, 1.0f);

    /* Layer page Groove: blend the accent curve (100%) against flat 1.0 (0%).
     * After the clamp, so 0% is exactly 1.0. */
    velocity = 1.0f - ((float)layer->groove_pct * 0.01f) * (1.0f - velocity);
    return velocity;
#endif
}

/* Set once at init by the app layer; NULL until then, so the core stays usable
 * (and testable) with no arp/drone modules linked in at all. */
static seq_solo_change_cb_t s_solo_change_cb = NULL;

/* True when any track of any layer has solo engaged (solo is global:
 * sequencer_core.h). Scans only the rows each layer has (num_tracks): a row
 * dropped by a shrink keeps its stored flag, which must not gate the rows
 * still in use. */
bool sequencer_core_any_solo(void)
{
    for (uint8_t li = 0; li < s_num_layers; li++) {
        for (uint8_t t = 0; t < s_layers[li].num_tracks; t++) {
            if (s_layers[li].solo[t]) return true;
        }
    }
    return false;
}

/* Whether `track` will sound: solo overrides mute (even on the same track)
 * whenever anything anywhere is soloed, otherwise mute alone gates. Not static:
 * also used by seq_core_trig.c's decorated-step ratchet path. */
bool sequencer_track_audible(const seq_layer_t *layer, uint8_t track)
{
    if (sequencer_core_any_solo()) {
        return layer->solo[track];
    }
    return !layer->mute[track];
}

uint8_t sequencer_clamp_layer_note(const seq_layer_t *layer, uint8_t note)
{
    if (layer->type == SEQ_LAYER_DRUM) {
        return SEQ_CLAMP_U8(note, SEQ_MIDI_NOTE_MIN, SEQ_MIDI_NOTE_MAX);
    } else {
        return SEQ_CLAMP_U8(note, SEQ_MEL_NOTE_MIN, SEQ_MEL_NOTE_MAX);
    }
}

uint8_t sequencer_resolve_track_note(const seq_layer_t *layer,
                                     uint8_t source_note)
{
    return sequencer_resolve_track_note_chord(layer, source_note,
                                              layer->chord_root,
                                              layer->chord_type);
}

uint8_t sequencer_resolve_track_note_chord(const seq_layer_t *layer,
                                           uint8_t source_note, uint8_t root,
                                           chord_type_t chord_type)
{
    if (layer->type != SEQ_LAYER_MELODIC) {
        return sequencer_clamp_layer_note(layer, source_note);
    }

    /* Chord preset sentinel passes through untouched: the clamp would smash it
     * to SEQ_MEL_NOTE_MAX. Expansion happens at fire time
     * (seq_track_fire_notes); quantize/chord snap never apply to chord tones. */
    if (SEQ_NOTE_IS_CHORD(source_note)) {
        return source_note;
    }

    /* Chord mode overrides the global scale quantizer for this layer. */
    if (layer->chord_mode) {
        uint8_t snapped = quantizer_snap_to_chord(source_note, root, chord_type);
        return sequencer_clamp_layer_note(layer, snapped);
    }

    if (!s_quantizer.enabled) {
        return sequencer_clamp_layer_note(layer, source_note);
    }

    const musical_scale_t *scale = quantizer_get_scale(s_quantizer.scale_index);
    uint8_t snapped = quantizer_snap_midi_note(source_note, s_quantizer.root_note, scale);
    return sequencer_clamp_layer_note(layer, snapped);
}

_Static_assert(SEQ_TRACKS <= QUANTIZER_VOICE_MAX,
               "a layer's rows must fit one quantizer_voice_chord call");

void seq_resolve_layer_rows(uint8_t layer_idx, uint8_t root,
                            chord_type_t type, uint8_t out[SEQ_TRACKS])
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    const uint8_t *src = s_track_source_note[layer_idx];
    uint8_t n = layer->num_tracks;
    if (n > SEQ_TRACKS) n = SEQ_TRACKS;

    if (layer->type != SEQ_LAYER_MELODIC || !layer->chord_mode) {
        for (uint8_t t = 0; t < n; t++) {
            out[t] = sequencer_resolve_track_note(layer, src[t]);
        }
        return;
    }

    /* Chord mode: the plain rows are voiced together, so they share the
     * chord's tones instead of each snapping to its own nearest one. Chord
     * preset rows pass through and take no part. */
    uint8_t refs[SEQ_TRACKS] = {0};
    uint8_t voiced[SEQ_TRACKS];
    uint8_t rows[SEQ_TRACKS];
    uint8_t nplain = 0;
    for (uint8_t t = 0; t < n; t++) {
        if (SEQ_NOTE_IS_CHORD(src[t])) {
            out[t] = src[t];
        } else {
            rows[nplain] = t;
            refs[nplain++] = src[t];
        }
    }
    quantizer_voice_chord(refs, nplain, root, type, voiced);
    for (uint8_t i = 0; i < nplain; i++) {
        out[rows[i]] = sequencer_clamp_layer_note(layer, voiced[i]);
    }
}

/* ── Low-level AMY helpers ───────────────────────────────────────────── */

void sequencer_emit_clear_tag(uint32_t tag)
{
    amy_event *e = amy_helpers_event_begin();
    e->ticks[TICKS_TAG]    = tag;
    e->ticks[TICKS_TICK]   = 0;
    e->ticks[TICKS_PERIOD] = 0;
    amy_helpers_event_send(e);
}

uint32_t seq_step_hold_ticks(const seq_layer_t *layer, uint8_t track, uint8_t step)
{
    uint32_t period = seq_track_period(layer, track);
    uint32_t on     = seq_step_tick_on(layer, track, step, period);
    uint32_t dist   = period;
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        if (s == step || !layer->grid[track][s]) continue;
        uint32_t d = (seq_step_tick_on(layer, track, s, period) + period - on) % period;
        if (d == 0) d = period;
        if (d < dist) dist = d;
    }
    uint32_t hold = seq_step_gate(layer, step);
    if (hold > dist - 1) hold = dist - 1;
    if (hold < 1) hold = 1;
    return hold;
}

/* Schedule (or cancel) one grid step as a pair of repeating AMY events: note-on
 * at the step's position in the bar, note-off seq_step_hold_ticks later, both
 * repeating every `period`. AMY keys events by tag, so re-emitting with the
 * same tag updates in place. */
void sequencer_emit_step(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    seq_layer_t *layer  = &s_layers[layer_idx];
    /* The note-off must wrap against the row's full period (repeat rate
     * included) to land in the correct half of the extended window. */
    uint32_t period     = seq_track_period(layer, track);
    uint32_t hold       = seq_step_hold_ticks(layer, track, step);
    uint32_t tag_on     = seq_tag_on(layer_idx, track, step);
    uint32_t tag_off    = seq_tag_off(layer_idx, track, step);
    /* Swing and nudge apply to tick_on only - tick_off derives from it below. */
    uint32_t tick_on    = seq_step_tick_on(layer, track, step, period);
    /* Wrap within the full period, not bar_ticks, so a repeat_rate=2 note whose
     * gate spills past bar_ticks still fires correctly. */
    uint32_t tick_off   = (tick_on + hold) % period;
    float note_velocity = sequencer_step_velocity(layer, track, step);
    /* Per-track amplitude trim (default 1.0, set by graph editor amp mode). */
    note_velocity *= layer->vp[track].amp_trim;
    /* Per-step velocity offset in signed percentage points. */
    note_velocity += (float)layer->step_velocity_adj[track][step] * 0.01f;
    note_velocity = SEQ_CLAMP_F32(note_velocity, 0.0f, 1.0f);
    /* Avoid the reserved tick 0; stepping back keeps the off before a next
     * step that fires at tick 1. */
    if (tick_off == 0) tick_off = (hold > 1) ? period - 1 : 1;

    /* Stopped, step off, track inaudible, or decorated (seq_core_trig.c):
     * cancel the plain periodic tag pair instead of emitting. */
    if (!s_playing || !layer->grid[track][step] ||
        !sequencer_track_audible(layer, track) ||
        sequencer_core_step_is_decorated(layer, track, step)) {
        sequencer_emit_clear_tag(tag_on);
        sequencer_emit_clear_tag(tag_off);
        return;
    }

    /* Drum and melodic layers alike have one synth slot per track. */
    uint8_t synth = layer->synth_id[track];

    /* Per-step pitch offset, chromatic on top of the resolved step pitch, not
     * re-quantized. The plain path never carries a chord sentinel (chords force
     * the decorated path), so plain arithmetic is safe. On and off use the same
     * value (note-off matching: seq_apply_track_note). */
    uint8_t note = layer->step_note[track][step];
    int8_t pofs = layer->step_pitch_ofs[track][step];
    if (pofs != 0)
        note = (uint8_t)SEQ_CLAMP_INT((int)note + (int)pofs, 0, 127);

    amy_helpers_note_send(synth, note, note_velocity,
                        tag_on, tick_on, period);
    /* PCM drums get no scheduled note-off: pcm_note_off() is a hard phase-jump
     * to the sample end, so the gate would truncate even the natural tail. The
     * hit rings to the sample's own end under EG0 (sustain 0 - the decay the
     * user authors is what shapes the audible length), and non-looping PCM
     * self-terminates, so nothing is left held. Suppressing only the emit
     * keeps every velocity-0 kill path (stop/mute/solo) fully effective -
     * unlike SYNTH_FLAGS_IGNORE_NOTE_OFFS, which swallows those too. SYNTH
     * mode keeps its note-offs: patch releases shape those tails on purpose. */
    if (layer->type == SEQ_LAYER_DRUM && s_drum_engine == SEQ_DRUM_PCM) {
        sequencer_emit_clear_tag(tag_off);
    } else {
        amy_helpers_note_send(synth, note, 0.0f,
                            tag_off, tick_off, period);
    }
}

void sequencer_emit_track(uint8_t layer_idx, uint8_t track)
{
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_step(layer_idx, track, s);
    }
}

/* Re-emit all steps for a layer (used on play-resume). */
void sequencer_resync_layer(uint8_t layer_idx)
{
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < layer->num_tracks; t++) {
        for (uint8_t s = 0; s < layer->num_steps; s++) {
            sequencer_emit_step(layer_idx, t, s);
        }
    }
}

/* Cancel all scheduled tags for a layer (used on pause). */
void sequencer_clear_layer_tags(uint8_t layer_idx)
{
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < layer->num_tracks; t++) {
        for (uint8_t s = 0; s < layer->num_steps; s++) {
            sequencer_emit_clear_tag(seq_tag_on(layer_idx, t, s));
            sequencer_emit_clear_tag(seq_tag_off(layer_idx, t, s));
        }
    }
}

void sequencer_clear_track_tags(uint8_t layer_idx, uint8_t track)
{
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_clear_tag(seq_tag_on(layer_idx, track, s));
        sequencer_emit_clear_tag(seq_tag_off(layer_idx, track, s));
    }
    sequencer_emit_clear_tag(seq_preview_tag(layer_idx, track));
    sequencer_emit_clear_tag(seq_preview_off_tag(layer_idx, track));
    seq_chord_preview_clear_from(layer_idx, track, 1);
}

/* Apply a track's resolved note: write it to every step and re-emit. With
 * `preview` set, also fire a short one-shot so the note is audible even when
 * quantization left it unchanged. */
static void seq_apply_track_note(uint8_t layer_idx, uint8_t track,
                                 uint8_t resolved_note, bool preview)
{
    seq_layer_t *layer = &s_layers[layer_idx];

    /* No change: skip the grid rewrite, but still preview so scrolling within
     * one scale degree remains audible. */
    if (layer->track_base_note[track] == resolved_note) {
        if (preview) {
            /* fall through to the preview below */
        } else {
            return;
        }
    } else if (SEQ_NOTE_IS_CHORD(layer->track_base_note[track])) {
        /* Leaving or switching a chord: kill this track's pending chord-tone
         * one-shots and preview pairs so no tone from the old voicing survives
         * the transition (clear -> rebuild, every time). */
        sequencer_core_trig_clear_track_chord(layer_idx, track);
        seq_chord_preview_clear_from(layer_idx, track, 1);
    }

    /* All steps on a track play one pitch. */
    layer->track_base_note[track] = resolved_note;
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        layer->step_note[track][s] = resolved_note;
    }

    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_step(layer_idx, track, s);
    }

    /* Kill anything this track is CURRENTLY sounding at the old pitch. Since
     * AMY v1.2.121 note-offs match by note number (instrument.c
     * _instrument_voice_for_note): the re-emits above and the preview
     * reschedule below rewrite the pending off tags with the NEW pitch, so a
     * note whose on already fired would never receive a matching off and ring
     * indefinitely (nearly every preview during a pitch scroll, since preview
     * spacing < gate). A velocity-0 event with NO midi_note is AMY's
     * synth-scoped all-notes-off (patches.c) - matched by voice, silent no-op
     * on an idle instrument. Melodic-only: the 1-voice drum-PCM instruments
     * take the single-voice path where a no-note event means note 0, and PCM
     * drums schedule no offs anyway. */
    if (layer->type == SEQ_LAYER_MELODIC) {
        amy_event *kill = amy_helpers_event_begin();
        kill->synth    = layer->synth_id[track];
        kill->velocity = 0.0f;
        amy_helpers_event_send(kill);
    }

    if (!preview) {
        return;
    }

    /* One-shot preview a few ticks out, reusing the same tag slot so rapid
     * scrolling only sounds the last change. Velocity matches a real downbeat
     * step (groove velocity x amp trim) so the level is honest. A chord
     * sentinel previews the full voicing: tone 0 on the plain preview pair,
     * extra tones on the chord pairs, pairs past the tone count cleared so a
     * shrink cannot leave a stale higher tone pending. */
    uint8_t tones[SEQ_CHORD_MAX_NOTES];
    uint8_t ntones = seq_track_fire_notes(layer, resolved_note, tones);
    if (ntones == 0) return;   /* undefined chord slot: nothing to audition */

    float preview_vel = sequencer_step_velocity(layer, track, 0)
                        * layer->vp[track].amp_trim;
    if (preview_vel > 1.0f) preview_vel = 1.0f;
    uint32_t fire_tick = sequencer_ticks() + SEQ_PREVIEW_DELAY_TICKS;
    uint32_t gate      = seq_step_gate(layer, 0);
    if (gate > SEQ_TICKS_PER_STEP) gate = SEQ_TICKS_PER_STEP;   /* one-step audition */
    uint32_t off_tick  = fire_tick + gate;
    /* PCM drums get no note-off, same rule as sequencer_emit_step(): a preview
     * while tuning a drum's pitch should sound like the real hit. */
    bool send_offs = !(layer->type == SEQ_LAYER_DRUM &&
                       s_drum_engine == SEQ_DRUM_PCM);
    amy_helpers_note_send(layer->synth_id[track], tones[0], preview_vel,
                        seq_preview_tag(layer_idx, track), fire_tick, 0);
    if (send_offs)
        amy_helpers_note_send(layer->synth_id[track], tones[0], 0.0f,
                            seq_preview_off_tag(layer_idx, track), off_tick, 0);
    for (uint8_t i = 1; i < ntones; i++) {
        amy_helpers_note_send(layer->synth_id[track], tones[i], preview_vel,
                            seq_chord_preview_on_tag(layer_idx, track, i),
                            fire_tick, 0);
        if (send_offs)
            amy_helpers_note_send(layer->synth_id[track], tones[i], 0.0f,
                                seq_chord_preview_off_tag(layer_idx, track, i),
                                off_tick, 0);
    }
    if (SEQ_NOTE_IS_CHORD(resolved_note)) {
        seq_chord_preview_clear_from(layer_idx, track, ntones);
    }

    ESP_LOGI(TAG, "L%d T%d note -> %d (preview @ tick %lu)",
             layer_idx + 1, track + 1, resolved_note, (unsigned long)fire_tick);
}

/* Re-resolve a track's note (clamp + optional quantization or chord voicing)
 * and apply it. In a chord-mode melodic layer the rows are voiced together, so
 * one row's source change can move the others: they take the new voicing too,
 * without a preview (the preview stays on the requested row). */
static void sequencer_refresh_track_note(uint8_t layer_idx, uint8_t track,
                                        bool preview)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks) return;

    uint8_t rows[SEQ_TRACKS];
    seq_resolve_layer_rows(layer_idx, layer->chord_root, layer->chord_type, rows);
    seq_apply_track_note(layer_idx, track, rows[track], preview);

    if (layer->type == SEQ_LAYER_MELODIC && layer->chord_mode) {
        for (uint8_t t = 0; t < layer->num_tracks; t++) {
            if (t != track) seq_apply_track_note(layer_idx, t, rows[t], false);
        }
    }
}

void sequencer_refresh_melodic_layers(bool preview)
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

void sequencer_core_set_step(uint8_t layer_idx, uint8_t track,
                              uint8_t step, bool state)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    if (layer->grid[track][step] == state) return;
    layer->grid[track][step] = state;
    sequencer_emit_track(layer_idx, track);
    /* Above one step the removed step's note may still be sounding, and its
     * off tag is gone. */
    if (!state && layer->gate_pct > 100u)
        sequencer_kill_synth_voices(layer->synth_id[track]);
}

bool sequencer_core_set_layer_steps(uint8_t layer_idx, uint8_t num_steps)
{
    if (layer_idx >= s_num_layers) return false;
    if (num_steps != SEQ_STEPS && num_steps != SEQ_MAX_STEPS) return false;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->num_steps == num_steps) return true;

    if (num_steps == SEQ_MAX_STEPS) {
        for (uint8_t t = 0; t < layer->num_tracks; t++) {
            seq_layer_copy_first_half(layer, t);
        }
    } else {
        for (uint8_t t = 0; t < layer->num_tracks; t++) {
            bool sounding = false;
            for (uint8_t s = SEQ_STEPS; s < SEQ_MAX_STEPS; s++) {
                sounding |= layer->grid[t][s];
                sequencer_emit_clear_tag(seq_tag_on(layer_idx, t, s));
                sequencer_emit_clear_tag(seq_tag_off(layer_idx, t, s));
            }
            /* A note held by an upper-half step loses its off tag with it. */
            if (sounding) sequencer_kill_synth_voices(layer->synth_id[t]);
        }
    }
    layer->num_steps = num_steps;
    /* The loop period is part of every step's schedule: re-emit them all. */
    sequencer_resync_layer(layer_idx);
    return true;
}

void sequencer_core_clear_track_pattern(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks) return;
    /* The full width, not num_steps: a later 16 -> 32 resize must not
     * uncover stale decorations. Neutral values as add_layer sets them. */
    for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
        layer->grid[track][s]               = false;
        layer->step_pitch_ofs[track][s]     = 0;
        layer->step_prob[track][s]          = 100;
        layer->step_ratchet[track][s]       = 1;
        layer->step_every[track][s]         = 1;
        layer->step_prev[track][s]          = 0;
        layer->step_transform[track][s]     = SEQ_STEP_TRANSFORM_NONE;
        layer->step_quant_bypass[track][s]  = 0;
        layer->step_nudge[track][s]         = 0;
        layer->step_velocity_adj[track][s]  = 0;
        layer->step_ratchet_taper[track][s] = 0;
    }
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_step(layer_idx, track, s);
    }
    sequencer_kill_synth_voices(layer->synth_id[track]);
}

/* Derive the playing step from AMY's free-running tick counter. When paused,
 * return the value captured at pause time so the UI playhead stops in place. */
uint8_t sequencer_core_get_current_step(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return 0;
    if (!s_playing) return s_cached_step[layer_idx];
    seq_layer_t *layer = &s_layers[layer_idx];
    s_cached_step[layer_idx] = seq_playhead_step(layer, sequencer_ticks());
    return s_cached_step[layer_idx];
}

/* Start/stop playback. Start re-emits every step so AMY repopulates its
 * schedule; stop captures each layer's playhead (frozen UI) and cancels all
 * scheduled events so nothing keeps triggering. */
#if CONFIG_SEQ_OOM_RESYNC
/* Self-heal after an AMY OOM burst. While internal heap is exhausted (e.g. an
 * oversized patch load), scheduled events and tag cancels are dropped
 * silently - only the FIRST failure prints (AMY's print-once policy), so
 * tracks stay mute until something happens to re-emit them. Poll the OOM
 * counter from the UI task; when a burst STOPS growing (one quiet service
 * tick, 50 ms), re-emit everything once. Waiting for quiet avoids resyncing
 * into the middle of the pressure that caused the drops. */
void sequencer_core_oom_service(void)
{
    static uint32_t s_last_oom = 0;
    static bool     s_resync_owed = false;
    uint32_t count = amy_get_oom_count();
    if (count != s_last_oom) {
        s_last_oom = count;
        s_resync_owed = true;   /* still growing - wait for a quiet tick */
        return;
    }
    if (!s_resync_owed) return;
    s_resync_owed = false;
    ESP_LOGW(TAG, "AMY oom count %lu settled - resyncing all layers",
             (unsigned long)count);
    for (uint8_t i = 0; i < s_num_layers; i++) {
        sequencer_resync_layer(i);
    }
    arp_core_mark_dirty();
}
#endif

void sequencer_core_set_playing(bool p)
{
    if (s_playing == p) return;
    s_playing = p;
    if (s_playing) {
        /* Anchor the bar counter to the NEXT absolute bar boundary, not the
         * play-press tick: AMY fires periodic events on tick % period, so
         * pattern loops are phase-locked to the absolute tick grid, and
         * rounding up makes progression bar lines coincide with step 0. The
         * partial pre-boundary stretch counts as bar 0 (bars_elapsed clamps),
         * so the first chord keeps its full duration. */
        uint32_t t = sequencer_ticks();
        s_bar_baseline = ((t + SEQ_TICKS_PER_BAR - 1) / SEQ_TICKS_PER_BAR)
                         * SEQ_TICKS_PER_BAR;
        s_prog.entry_start_bar = 0;
        s_prog.current = 0;
        /* The progression restarts from entry 0, but the layers/arp may still
         * hold the chord that was live at stop time - request a re-apply so
         * what is shown active is what sounds. Drained by the progression
         * service on synth_ui_task (single-applier). Immediate, because a
         * correction must bypass the BAR launch-quantize hold. */
        if (s_prog.enabled) {
            s_prog_apply_immediate = true;
            s_prog_apply_pending = true;
        }
        for (uint8_t i = 0; i < s_num_layers; i++) {
            sequencer_core_trig_reset(i);
            sequencer_resync_layer(i);
        }
        /* The pause path cleared the arp schedule and emission is
         * s_playing-gated, so nothing re-armed it while stopped - request a
         * coalesced re-emit (drained by arp_core_service() on the UI task). */
        arp_core_mark_dirty();
        clip_player_on_transport(true);
    } else {
        /* Clear arp scheduled events FIRST so repeating arp tags don't keep
         * firing while the sequencer is paused. */
        arp_core_clear_all();

        /* Freeze display positions before clearing scheduled events. */
        for (uint8_t i = 0; i < s_num_layers; i++) {
            seq_layer_t *layer = &s_layers[i];
            s_cached_step[i] = seq_playhead_step(layer, sequencer_ticks());
            sequencer_clear_layer_tags(i);
            /* Decorated steps' one-shot ratchet tags live in a different tag
             * space and survive sequencer_clear_layer_tags(), so clear them
             * explicitly or a pending sub-hit fires after stop. */
            sequencer_core_trig_clear_all(i);
        }

        /* Silence only the sequencer's own synth slots (per-layer melodic/drum
         * plus the arp). The drone slots are deliberately spared: they manage
         * their own lifecycle and never auto-resume, so a global notes-off
         * would silence the drone permanently. */
        for (uint8_t i = 0; i < s_num_layers; i++) {
            seq_layer_t *layer = &s_layers[i];
            for (uint8_t t = 0; t < layer->num_tracks; t++) {
                sequencer_kill_synth_voices(layer->synth_id[t]);
            }
        }
        sequencer_kill_synth_voices(SEQ_ARP_SYNTH);
        clip_player_on_transport(false);
    }
}

void sequencer_core_set_track_midi_note(uint8_t layer_idx, uint8_t track,
                                         uint8_t midi_note)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];

    /* A defined chord sentinel is a valid melodic assignment and must not be
     * range-clamped (that would smash it to SEQ_MEL_NOTE_MAX). Everything else
     * takes the normal clamp. */
    bool chord_ok = layer->type == SEQ_LAYER_MELODIC &&
                    SEQ_NOTE_IS_CHORD(midi_note) &&
                    seq_chords_is_defined(SEQ_CHORD_INDEX(midi_note));
    if (!chord_ok) {
        midi_note = sequencer_clamp_layer_note(layer, midi_note);
        /* Fallback if a chord slot this track later references is deleted. */
        s_track_prev_plain[layer_idx][track] = midi_note;
    }

    s_track_source_note[layer_idx][track] = midi_note;
    sequencer_refresh_track_note(layer_idx, track, true);

    /* Chord assignment can widen or release this track's voice need beyond the
     * layer's configured count. Reconfigure through the paused
     * clear -> configure -> resync path, only when the need actually changed. */
    if (layer->type == SEQ_LAYER_MELODIC && sequencer_layer_voices_stale(layer_idx)) {
        sequencer_reconfigure_layer_paused(layer_idx);
    }
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

/* Chord table edit sweep (called by seq_chords_set/clear on the UI task).
 * Voicing edits need no re-emit - fires read the table live - but pending
 * one-shots from the old voicing are cleared, a now-undefined slot drops
 * referencing tracks back to their last plain note, and a changed tone count
 * reconfigures the layer's voice budget. */
void sequencer_core_chord_slot_changed(uint8_t idx)
{
    if (idx >= SEQ_CHORD_SLOTS) return;
    uint8_t sentinel = SEQ_CHORD_NOTE(idx);
    bool defined = seq_chords_is_defined(idx);

    for (uint8_t li = 0; li < s_num_layers; li++) {
        seq_layer_t *layer = &s_layers[li];
        if (layer->type != SEQ_LAYER_MELODIC) continue;
        bool touched = false;
        for (uint8_t t = 0; t < layer->num_tracks; t++) {
            if (s_track_source_note[li][t] != sentinel) continue;
            touched = true;
            sequencer_core_trig_clear_track_chord(li, t);
            seq_chord_preview_clear_from(li, t, 1);
            if (!defined) {
                /* Never leave a track silently referencing an empty slot. */
                uint8_t fb = s_track_prev_plain[li][t];
                if (fb == 0) fb = 60;   /* C4 when no plain note was ever set */
                s_track_source_note[li][t] =
                    sequencer_clamp_layer_note(layer, fb);
            }
            sequencer_refresh_track_note(li, t, false);
        }
        if (touched && sequencer_layer_voices_stale(li)) {
            sequencer_reconfigure_layer_paused(li);
        }
    }
}

/* Chord-editor audition: one-shot the (possibly unsaved) voicing through a
 * melodic track's synth via the preview tags. Tones play exactly as authored -
 * no progression transpose, the editor edits the "I" voicing. Caveat: a voicing
 * wider than the track's current voice count voice-steals until assignment
 * widens it; timbre, the point of the audition, is still accurate. */
void sequencer_core_audition_chord(uint8_t layer_idx, uint8_t track,
                                   const seq_chord_t *chord)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS || chord == NULL) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return;

    uint8_t tones[SEQ_CHORD_MAX_NOTES];
    uint8_t ntones = 0;
    for (uint8_t i = 0; i < SEQ_CHORD_MAX_NOTES; i++) {
        if (chord->notes[i] == 0) continue;
        tones[ntones++] = sequencer_core_clamp_melodic_note(chord->notes[i]);
    }
    seq_chord_preview_clear_from(layer_idx, track, ntones ? ntones : 1);
    if (ntones == 0) return;

    float preview_vel = sequencer_step_velocity(layer, track, 0)
                        * layer->vp[track].amp_trim;
    if (preview_vel > 1.0f) preview_vel = 1.0f;
    uint32_t fire_tick = sequencer_ticks() + SEQ_PREVIEW_DELAY_TICKS;
    uint32_t gate      = seq_step_gate(layer, 0);
    if (gate > SEQ_TICKS_PER_STEP) gate = SEQ_TICKS_PER_STEP;   /* one-step audition */
    uint32_t off_tick  = fire_tick + gate;
    amy_helpers_note_send(layer->synth_id[track], tones[0], preview_vel,
                        seq_preview_tag(layer_idx, track), fire_tick, 0);
    amy_helpers_note_send(layer->synth_id[track], tones[0], 0.0f,
                        seq_preview_off_tag(layer_idx, track), off_tick, 0);
    for (uint8_t i = 1; i < ntones; i++) {
        amy_helpers_note_send(layer->synth_id[track], tones[i], preview_vel,
                            seq_chord_preview_on_tag(layer_idx, track, i),
                            fire_tick, 0);
        amy_helpers_note_send(layer->synth_id[track], tones[i], 0.0f,
                            seq_chord_preview_off_tag(layer_idx, track, i),
                            off_tick, 0);
    }
}

void sequencer_core_arp_emit_note(uint32_t tag_base, uint8_t midi_note,
                                  float velocity, uint32_t tick_on,
                                  uint32_t gate_ticks, uint32_t period)
{
    /* Slaved to the transport like sequencer_emit_step(): nothing may re-arm
     * the arp's repeating schedule while paused, or an arp refresh during a
     * pause (param edit, progression apply, project load) leaves the arp
     * playing alone. sequencer_core_set_playing() marks the arp dirty on
     * resume; periodic events are phase-locked to the absolute tick grid, so
     * the rebuild lands back in sync. */
    if (!s_playing) return;

    /* Never let an out-of-range tag reach AMY: sequences[] is sized to
     * max_sequencer_tags and add_event has a `tag > max` off-by-one, so a stray
     * tag smashes the heap. Cap to the reserved arp window. */
    if (tag_base + 1 > SEQ_ARP_TAG_MAX) {
        ESP_LOGE(TAG, "arp tag %u out of range (max %u) - dropped",
                 (unsigned)tag_base, (unsigned)SEQ_ARP_TAG_MAX);
        return;
    }

    uint32_t tick_off = (period > 0) ? ((tick_on + gate_ticks) % period)
                                     : (tick_on + gate_ticks);
    if (tick_off == 0) tick_off = 1; /* tick 0 is reserved (clear) */
    if (tick_on  == 0) tick_on  = 1;

    amy_helpers_note_send(SEQ_ARP_SYNTH, midi_note, velocity,
                        tag_base, tick_on, period);
    amy_helpers_note_send(SEQ_ARP_SYNTH, midi_note, 0.0f,
                        tag_base + 1, tick_off, period);
}

void sequencer_core_arp_clear_note(uint32_t tag_base)
{
    sequencer_emit_clear_tag(tag_base);
    sequencer_emit_clear_tag(tag_base + 1);
}

void sequencer_core_set_track_repeat_rate(uint8_t layer_idx, uint8_t track,
                                          seq_repeat_rate_t rate)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    layer->repeat_rate[track] = (uint8_t)rate;
    /* Re-emit all steps on this track so AMY picks up the new period. */
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_step(layer_idx, track, s);
    }
}

seq_repeat_rate_t sequencer_core_get_track_repeat_rate(uint8_t layer_idx,
                                                        uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return SEQ_REPEAT_1;
    uint8_t rr = s_layers[layer_idx].repeat_rate[track];
    switch (rr) {
        case 2: return SEQ_REPEAT_2;
        case 4: return SEQ_REPEAT_4;
        case 8: return SEQ_REPEAT_8;
        default: return SEQ_REPEAT_1;
    }
}

/* ── Per-layer swing ─────────────────────────────────────────────────────
 * Whole-layer feel control, not per-track: every odd step across all tracks
 * shifts by the same fraction, so the layer grooves as a unit. Re-emit the
 * whole layer so AMY reschedules each step at its swung tick. */
void sequencer_core_set_layer_swing(uint8_t layer_idx, uint8_t swing_pct)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    uint8_t clamped = (uint8_t)SEQ_CLAMP_U8((int)swing_pct, 0, SEQ_SWING_MAX);
    if (layer->swing_pct == clamped) return;
    layer->swing_pct = clamped;
    sequencer_resync_layer(layer_idx);
}

uint8_t sequencer_core_get_layer_swing(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return 0;
    return s_layers[layer_idx].swing_pct;
}

/* ── Per-layer note FX: gate length + glide (portamento) ───────────────────
 * Per-layer scalars from the Layer menu page. Gate applies at emit time
 * (seq_step_hold_ticks), so a change must re-emit the layer's steps for the new
 * note-off ticks to take effect, exactly like swing; it applies to drum and
 * melodic layers alike. Glide is an AMY per-osc setting pushed straight to the
 * row synths, melodic only (drums don't glide). */
void sequencer_core_set_layer_gate_pct(uint8_t layer_idx, uint16_t gate_pct)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    uint16_t clamped = (gate_pct == SEQ_GATE_HOLD)
                       ? (uint16_t)SEQ_GATE_HOLD
                       : SEQ_CLAMP_U16((int)gate_pct, SEQ_GATE_PCT_MIN,
                                       SEQ_GATE_PCT_MAX);
    if (layer->gate_pct == clamped) return;
    uint16_t prev = layer->gate_pct;
    layer->gate_pct = clamped;
    sequencer_resync_layer(layer_idx);   /* re-emit: gate changes note-off ticks */
    /* Shortening from above one step (sequencer_core.h gate contract). */
    if (prev > 100u && clamped < prev) {
        for (uint8_t t = 0; t < layer->num_tracks; t++) {
            sequencer_kill_synth_voices(layer->synth_id[t]);
        }
    }
}

uint16_t sequencer_core_get_layer_gate_pct(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return 0;
    return s_layers[layer_idx].gate_pct;
}

void sequencer_core_set_melodic_portamento_ms(uint8_t layer_idx, uint16_t ms)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return;
    uint16_t clamped = (uint16_t)SEQ_CLAMP_U16((int)ms, 0, SEQ_MELODIC_PORTAMENTO_MAX_MS);
    if (layer->portamento_ms == clamped) return;
    layer->portamento_ms = clamped;
    sequencer_core_push_melodic_portamento(layer_idx);
}

uint16_t sequencer_core_get_melodic_portamento_ms(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return 0;
    return s_layers[layer_idx].portamento_ms;
}

void sequencer_core_set_melodic_groove_pct(uint8_t layer_idx, uint8_t groove_pct)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return;
    uint8_t clamped = (uint8_t)SEQ_CLAMP_U8((int)groove_pct, 0, 100);
    if (layer->groove_pct == clamped) return;
    layer->groove_pct = clamped;
    sequencer_resync_layer(layer_idx);   /* re-emit: velocity is baked at emit */
}

uint8_t sequencer_core_get_melodic_groove_pct(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return 0;
    return s_layers[layer_idx].groove_pct;
}

void sequencer_core_set_track_mute(uint8_t layer_idx, uint8_t track, bool mute)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->mute[track] == mute) return;
    layer->mute[track] = mute;
    /* Mute only ever changes this one track's own audibility. */
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_step(layer_idx, track, s);
    }
    if (!sequencer_track_audible(layer, track)) {
        sequencer_kill_synth_voices(layer->synth_id[track]);
    }
}

bool sequencer_core_get_track_mute(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    return s_layers[layer_idx].mute[track];
}

/* Re-emit every layer and hard-kill whatever just went inaudible: a note
 * already sounding on a now-silenced row would otherwise ring on to its
 * scheduled note-off. Ducking the arp and drones is the hook's job
 * (sequencer_core.h). */
static void sequencer_apply_solo_change(void)
{
    for (uint8_t li = 0; li < s_num_layers; li++) {
        seq_layer_t *L = &s_layers[li];
        sequencer_resync_layer(li);
        for (uint8_t t = 0; t < L->num_tracks; t++) {
            if (!sequencer_track_audible(L, t)) {
                sequencer_kill_synth_voices(L->synth_id[t]);
            }
        }
    }
    if (s_solo_change_cb) s_solo_change_cb(sequencer_core_any_solo());
}

void sequencer_core_set_track_solo(uint8_t layer_idx, uint8_t track, bool solo)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->solo[track] == solo) return;
    layer->solo[track] = solo;
    sequencer_apply_solo_change();
}

void sequencer_core_clear_all_solos(void)
{
    bool any = false;
    for (uint8_t li = 0; li < s_num_layers; li++) {
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            if (s_layers[li].solo[t]) { s_layers[li].solo[t] = false; any = true; }
        }
    }
    if (any) sequencer_apply_solo_change();
}

void sequencer_core_notify_solo_changed(void)
{
    sequencer_apply_solo_change();
}

void sequencer_core_set_solo_change_cb(seq_solo_change_cb_t cb)
{
    s_solo_change_cb = cb;
}

bool sequencer_core_get_track_solo(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    return s_layers[layer_idx].solo[track];
}
