#include "sequencer_core/seq_core_internal.h"
#include "seq_clamp.h"

/* ════════════════════════════════════════════════════════════════════════
 *  Per-step probability / ratchet / conditional-trig engine
 * ════════════════════════════════════════════════════════════════════════
 *
 * A step is "plain" when step_prob==100 && step_ratchet==1 &&
 * step_every<=1 && !step_prev && no note transform && no chord sentinel: it
 * keeps sequencer_emit_step()'s always-on repeating AMY sequence tag
 * (seq_core_engine.c) at zero added cost. The predicate is
 * sequencer_core_step_is_decorated(). The plain path emits one repeating tag at
 * a fixed pitch and AMY has no per-repetition hook, so a step whose pitch or
 * firing varies per loop cannot use it.
 *
 * Any other combination makes the step "decorated". sequencer_emit_step()
 * leaves such a step's plain ON/OFF tag pair cleared and
 * sequencer_core_service_tick() - called once per AMY sequencer tick from the
 * sequencer hook in main.c - takes over: it detects a layer's playhead
 * crossing into a new step (O(1) compare per layer per tick, with the per-track
 * decoration check only on that edge, so steady state costs a few comparisons
 * rather than a scan of every step), evaluates the conditional trig and
 * probability roll, and one-shot schedules step_ratchet sub-hits on the
 * dedicated tag range (SEQ_RATCHET_TAG_BASE.., seq_core_config.h).
 */

/* ── State (per MAX_LAYERS[/xSEQ_TRACKS]) ────────────────────────────────
 * Runtime bookkeeping only - never persisted, never pattern data (that lives
 * in seq_layer_t). Deliberately reset wholesale by
 * sequencer_core_trig_reset_all() on any layer add/delete rather than shifted
 * in lockstep with compaction; rationale at the call sites in
 * seq_core_state.c. */
uint32_t s_layer_loop_count[MAX_LAYERS];
uint8_t  s_layer_last_step[MAX_LAYERS];
bool     s_track_last_played[MAX_LAYERS][SEQ_TRACKS];

/* Dedicated xorshift32 PRNG, kept separate from seq_core_editors.c's
 * s_lfo_rng_state: that one is mutated from synth_ui_task, this one only from
 * sequencer_core_service_tick() on the AMY sequencer-hook task. Sharing one
 * PRNG across two independently-scheduled tasks would be a real data race. */
static uint32_t s_trig_rng_state = 0xA53C9E17u;

static inline uint8_t trig_rand_pct(void)
{
    uint32_t x = s_trig_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_trig_rng_state = x;
    return (uint8_t)(x % 100u);
}

/* Semitone span shared by every non-NONE transform mode: RANDOM offsets by
 * +/-SPAN, RAMP walks 0..SPAN across successive layer loops then wraps. Fixed
 * rather than a per-step param, to keep the step-component subset minimal; one
 * octave reads musically once the result is re-snapped to the scale. */
#define SEQ_STEP_TRANSFORM_SPAN 12

/* Per-step pitch transform, factored as "which offset does this fire get" so
 * single notes and chord voicings share one PRNG/loop-counter draw per fire.
 * Returns true and writes the semitone offset when a transform is active, false
 * (offset 0) for NONE. Integer math only: no float, no lock, safe on the
 * service-tick task. */
static bool trig_transform_offset(uint8_t layer_idx, const seq_layer_t *layer,
                                  uint8_t track, uint8_t step, int *offset)
{
    switch ((seq_step_transform_t)layer->step_transform[track][step]) {
        case SEQ_STEP_TRANSFORM_RANDOM:
            *offset = (int)(trig_rand_pct() % (2u * SEQ_STEP_TRANSFORM_SPAN + 1u))
                      - SEQ_STEP_TRANSFORM_SPAN;
            return true;
        case SEQ_STEP_TRANSFORM_RAMP_UP:
            *offset = (int)(s_layer_loop_count[layer_idx]
                            % (SEQ_STEP_TRANSFORM_SPAN + 1u));
            return true;
        case SEQ_STEP_TRANSFORM_RAMP_DOWN:
            *offset = -(int)(s_layer_loop_count[layer_idx]
                             % (SEQ_STEP_TRANSFORM_SPAN + 1u));
            return true;
        case SEQ_STEP_TRANSFORM_NONE:
        default:
            *offset = 0;
            return false;
    }
}

/* ── Ratchet tag formula ──────────────────────────────────────────────────
 * Statically assigned per (layer, track, ratchet-slot), never pooled across
 * tracks, so a slow ratchet group cannot overwrite another track's in-flight
 * schedule. Only one step per track is ever current, so SEQ_MAX_RATCHET slots
 * per track cover the worst case. Tag-space layout: seq_core_config.h. */
static inline uint32_t ratchet_slot_index(uint8_t layer, uint8_t track, uint8_t slot)
{
    return ((uint32_t)layer * SEQ_TRACKS + track) * SEQ_MAX_RATCHET + slot;
}

static inline uint32_t ratchet_on_tag(uint8_t layer, uint8_t track, uint8_t slot)
{
    return SEQ_RATCHET_TAG_BASE + ratchet_slot_index(layer, track, slot) * 2u;
}

static inline uint32_t ratchet_off_tag(uint8_t layer, uint8_t track, uint8_t slot)
{
    return ratchet_on_tag(layer, track, slot) + 1u;
}

/* ── Chord-tone tag formula ───────────────────────────────────────────────
 * Extra chord tones (1..SEQ_CHORD_MAX_NOTES-1; tone 0 rides the ratchet tags
 * above) get a statically assigned one-shot pair per (layer, track, slot,
 * tone), same never-pooled discipline as the ratchets. Layout in
 * seq_core_config.h (SEQ_CHORD_TAG_BASE..MAX). */
static inline uint32_t chord_on_tag(uint8_t layer, uint8_t track, uint8_t slot,
                                    uint8_t tone /* 1.. */)
{
    return SEQ_CHORD_TAG_BASE
         + (ratchet_slot_index(layer, track, slot) * (SEQ_CHORD_MAX_NOTES - 1)
            + (uint32_t)(tone - 1)) * 2u;
}

static inline uint32_t chord_off_tag(uint8_t layer, uint8_t track, uint8_t slot,
                                     uint8_t tone /* 1.. */)
{
    return chord_on_tag(layer, track, slot, tone) + 1u;
}

bool sequencer_core_step_is_decorated(const seq_layer_t *layer, uint8_t track, uint8_t step)
{
    if (track >= SEQ_TRACKS || step >= SEQ_MAX_STEPS) return false;
    /* A chord sentinel forces the decorated path too: the plain path emits ONE
     * periodic tag pair at a fixed pitch, while a chord fires up to
     * SEQ_CHORD_MAX_NOTES tones whose pitches (progression transpose) and count
     * (live table edits) resolve per fire. One-shots self-consume, so chords
     * add no resident periodic events and cannot leave a tag ringing. A
     * ROOT-follow melodic row is decorated for the same reason: its progression
     * transpose resolves per fire. */
    return layer->step_prob[track][step]      != 100
        || layer->step_ratchet[track][step]   != 1
        || layer->step_every[track][step]     > 1
        || layer->step_prev[track][step]      != 0
        || layer->step_transform[track][step] != (uint8_t)SEQ_STEP_TRANSFORM_NONE
        || SEQ_NOTE_IS_CHORD(layer->step_note[track][step])
        || (layer->type == SEQ_LAYER_MELODIC &&
            layer->follow[track] == SEQ_FOLLOW_ROOT);
}

void sequencer_core_trig_reset(uint8_t layer_idx)
{
    if (layer_idx >= MAX_LAYERS) return;
    s_layer_loop_count[layer_idx] = 0;
    s_layer_last_step[layer_idx]  = 0xFF;   /* sentinel: no step seen yet this run */
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        s_track_last_played[layer_idx][t] = false;
    }
}

void sequencer_core_trig_reset_all(void)
{
    for (uint8_t i = 0; i < MAX_LAYERS; i++) {
        sequencer_core_trig_reset(i);
    }
}

void sequencer_core_trig_clear_all(uint8_t layer_idx)
{
    if (layer_idx >= MAX_LAYERS) return;
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        for (uint8_t k = 0; k < SEQ_MAX_RATCHET; k++) {
            sequencer_emit_clear_tag(ratchet_on_tag(layer_idx, t, k));
            sequencer_emit_clear_tag(ratchet_off_tag(layer_idx, t, k));
            for (uint8_t tone = 1; tone < SEQ_CHORD_MAX_NOTES; tone++) {
                sequencer_emit_clear_tag(chord_on_tag(layer_idx, t, k, tone));
                sequencer_emit_clear_tag(chord_off_tag(layer_idx, t, k, tone));
            }
        }
    }
}

void sequencer_core_trig_clear_track_chord(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= MAX_LAYERS || track >= SEQ_TRACKS) return;
    for (uint8_t k = 0; k < SEQ_MAX_RATCHET; k++) {
        for (uint8_t tone = 1; tone < SEQ_CHORD_MAX_NOTES; tone++) {
            sequencer_emit_clear_tag(chord_on_tag(layer_idx, track, k, tone));
            sequencer_emit_clear_tag(chord_off_tag(layer_idx, track, k, tone));
        }
    }
}

void sequencer_core_trig_clear_track(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= MAX_LAYERS || track >= SEQ_TRACKS) return;
    for (uint8_t k = 0; k < SEQ_MAX_RATCHET; k++) {
        sequencer_emit_clear_tag(ratchet_on_tag(layer_idx, track, k));
        sequencer_emit_clear_tag(ratchet_off_tag(layer_idx, track, k));
    }
    sequencer_core_trig_clear_track_chord(layer_idx, track);
}

/* ── Conditional trig evaluation ──────────────────────────────────────────
 * EVERY and PREV are independent conditions; both must hold. every==0 (an
 * uninitialised or hand-edited value) is treated as 1 (neutral). */
static bool trig_eval_condition(uint8_t layer_idx, const seq_layer_t *layer,
                                uint8_t track, uint8_t step)
{
    uint8_t n = layer->step_every[track][step];
    if (n > 1 && (s_layer_loop_count[layer_idx] % n) != 0) return false;
    if (layer->step_prev[track][step] && !s_track_last_played[layer_idx][track])
        return false;
    return true;
}

static bool trig_roll_probability(uint8_t prob_pct)
{
    if (prob_pct >= 100) return true;
    if (prob_pct == 0)   return false;
    return trig_rand_pct() < prob_pct;
}

/* One-shot schedule step_ratchet sub-hits, evenly spaced across the step's
 * slot. n==1 shares sequencer_emit_step()'s note-hold (seq_step_hold_ticks) so
 * a merely probabilistic or conditional step feels identical to a plain one
 * when it fires; n>1 subdivides the slot.
 *
 * Not static: pump task only (seq_trig_pump.c); never call it from the render
 * task. */
void trig_schedule_ratchets(uint8_t layer_idx, const seq_layer_t *layer,
                            uint8_t track, uint8_t step, uint32_t grid_tick)
{
    uint8_t n = layer->step_ratchet[track][step];
    n = SEQ_CLAMP_U8(n, 1, SEQ_MAX_RATCHET);

    uint16_t sub_ticks;
    uint32_t gate;
    if (n == 1) {
        sub_ticks = SEQ_TICKS_PER_STEP;
        gate = seq_step_hold_ticks(layer, track, step);
    } else {
        sub_ticks = SEQ_TICKS_PER_STEP / n;
        if (sub_ticks < 2) sub_ticks = 2;
        gate = (uint16_t)(sub_ticks - 1);
    }

    float velocity = sequencer_step_velocity(layer, track, step) * layer->vp[track].amp_trim;
    /* Per-step velocity offset in signed percentage points, applied here as
     * well as on the plain path so decorated steps match. */
    velocity += (float)layer->step_velocity_adj[track][step] * 0.01f;
    velocity = SEQ_CLAMP_F32(velocity, 0.0f, 1.0f);

    /* Same tick law as sequencer_emit_step(): grid boundary + 1, swung and
     * nudged. The lookahead in sequencer_core_service_tick() guarantees this
     * base is still ahead of the sequencer clock for the earliest nudge. */
    int32_t base = (int32_t)grid_tick + 1
                 + (int32_t)sequencer_step_swing_offset(layer, step)
                 + (int32_t)layer->step_nudge[track][step];

    /* This runs about a step ahead of the fire, which can be before the
     * progression applies the chord of the bar the step fires in (the apply
     * leads the bar line by only 65 ms). A published change due by the fire
     * tick resolves the pitch against that chord instead of the layer's. */
    uint8_t chord_root = layer->chord_root;
    chord_type_t chord_type = layer->chord_type;
    bool pending = layer->type == SEQ_LAYER_MELODIC && layer->chord_mode &&
                   sequencer_core_progression_chord_at((uint32_t)base,
                                                       &chord_root, &chord_type);

    /* Single per-fire pitch source for both ratchet sub-hits and the n==1 path.
     * A chord sentinel expands here with the progression transpose applied, and
     * a per-step transform draws ONE offset per fire so every sub-hit and every
     * chord tone shares it. Single notes offset then re-snap unless bypassed;
     * chord tones take the offset chromatically with a bounds clamp, never
     * per-tone re-quantization - the intervals are the feature. */
    uint8_t stored = layer->step_note[track][step];
    if (pending && !SEQ_NOTE_IS_CHORD(stored)) {
        uint8_t rows[SEQ_TRACKS];
        seq_resolve_layer_rows(layer_idx, chord_root, chord_type, rows);
        stored = rows[track];
    }
    uint8_t tones[SEQ_CHORD_MAX_NOTES];
    uint8_t ntones = seq_track_fire_notes_root(layer, track, stored, chord_root, tones);
    if (ntones == 0) return;   /* undefined chord slot: fire nothing */

    int toff;
    if (trig_transform_offset(layer_idx, layer, track, step, &toff)) {
        if (SEQ_NOTE_IS_CHORD(stored)) {
            for (uint8_t i = 0; i < ntones; i++) {
                tones[i] = sequencer_core_clamp_melodic_note((int32_t)tones[i] + toff);
            }
        } else {
            /* Re-snap by follow mode: ROOT already sits on its transposed scale
             * line, so it clamps only; OFF snaps to the scale, not the chord. */
            int tn = SEQ_CLAMP_INT((int)tones[0] + toff, 0, 127);
            uint8_t follow = layer->type == SEQ_LAYER_MELODIC
                           ? layer->follow[track] : (uint8_t)SEQ_FOLLOW_CHORD;
            if (layer->step_quant_bypass[track][step] || follow == SEQ_FOLLOW_ROOT) {
                tones[0] = sequencer_clamp_layer_note(layer, (uint8_t)tn);
            } else if (follow == SEQ_FOLLOW_OFF) {
                tones[0] = seq_resolve_scale_note(layer, (uint8_t)tn);
            } else {
                tones[0] = sequencer_resolve_track_note_chord(layer, (uint8_t)tn,
                                                              chord_root, chord_type);
            }
        }
    }
    /* Per-step pitch offset, applied last so the transform's re-snap above
     * cannot quantize it away: chromatic semitones on single notes and chord
     * tones alike (block transpose preserves the chord's intervals). */
    int8_t pofs = layer->step_pitch_ofs[track][step];
    if (pofs != 0) {
        for (uint8_t i = 0; i < ntones; i++)
            tones[i] = (uint8_t)SEQ_CLAMP_INT((int)tones[i] + (int)pofs, 0, 127);
    }
    uint8_t synth = layer->synth_id[track];

    /* PCM drums get no scheduled note-off - same rule and rationale as the
     * plain path in sequencer_emit_step(): the off is a hard cut for PCM, and
     * the sample self-terminates under EG0. */
    bool send_offs = !(layer->type == SEQ_LAYER_DRUM &&
                       s_drum_engine == SEQ_DRUM_PCM);

    /* Ratchet velocity taper: sub-hit k scales by (1 - taper*k%). 0 = flat,
     * positive decays toward the tail, negative ramps up; k==0 is always
     * full velocity. */
    int8_t taper = layer->step_ratchet_taper[track][step];
    /* One wavetable frame for every sub-hit and chord tone of the step. */
    float duty = seq_track_frame_duty(layer, track, step);
    for (uint8_t k = 0; k < n; k++) {
        float scale = 1.0f - (float)taper * 0.01f * (float)k;
        scale = SEQ_CLAMP_F32(scale, 0.0f, 1.0f);
        float v = velocity * scale;
        uint32_t tick_on  = (uint32_t)base + (uint32_t)k * sub_ticks;
        uint32_t tick_off = tick_on + gate;
        amy_helpers_note_send_duty(synth, (float)tones[0], v,
                                   ratchet_on_tag(layer_idx, track, k), tick_on, 0, duty);
        if (send_offs)
            amy_helpers_note_send(synth, (float)tones[0], 0.0f,
                                ratchet_off_tag(layer_idx, track, k), tick_off, 0);
        for (uint8_t i = 1; i < ntones; i++) {
            amy_helpers_note_send_duty(synth, (float)tones[i], v,
                                       chord_on_tag(layer_idx, track, k, i), tick_on, 0, duty);
            if (send_offs)
                amy_helpers_note_send(synth, (float)tones[i], 0.0f,
                                    chord_off_tag(layer_idx, track, k, i), tick_off, 0);
        }
    }
}

/* ── Per-tick entry point, called from main.c's AMY sequencer hook ──────── */
void sequencer_core_service_tick(void)
{
    if (!s_playing) return;
    /* A structural s_layers edit (delete_layer's compaction) is in flight on
     * another task: skip this tick rather than read a half-shifted table. The
     * engine tolerates skipped ticks - trig_reset_all runs after the edit - so
     * a dropped tick is inaudible. */
    if (s_layers_mutating) return;
    /* Decorated steps are evaluated on a clock run SEQ_TRIG_LOOKAHEAD_TICKS
     * ahead of the sequencer, so a step can be scheduled at its absolute grid
     * tick plus swing plus a negative nudge and still land in the future.
     * Everything below - edge detection, loop count, conditions, repeat-rate
     * window, mute/solo - is consistent on this one advanced clock. */
    uint32_t eval_ticks = sequencer_ticks() + SEQ_TRIG_LOOKAHEAD_TICKS;

    for (uint8_t li = 0; li < s_num_layers; li++) {
        seq_layer_t *layer = &s_layers[li];
        /* bar_ticks is local to the repeat-rate window math below; the step
         * derivation goes through the shared helper. */
        uint32_t bar_ticks = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
        if (bar_ticks == 0) continue;
        uint8_t cur_step = seq_playhead_step(layer, eval_ticks);
        if (cur_step == s_layer_last_step[li]) continue;  /* still mid-step: O(1) exit */

        if (cur_step == 0 && s_layer_last_step[li] != 0xFF) {
            s_layer_loop_count[li]++;
        }
        s_layer_last_step[li] = cur_step;
        /* Absolute tick of this step's grid boundary: the fire law needs the
         * boundary, not the (slightly later) detection tick. */
        uint32_t grid_tick = eval_ticks - ((eval_ticks % bar_ticks) % SEQ_TICKS_PER_STEP);

        for (uint8_t tr = 0; tr < layer->num_tracks; tr++) {
            bool on        = layer->grid[tr][cur_step];
            bool decorated = sequencer_core_step_is_decorated(layer, tr, cur_step);
            bool fire;

            if (!on) {
                fire = false;
            } else if (!decorated) {
                fire = true;  /* AMY's own periodic tag already fires this one */
            } else {
                fire = trig_eval_condition(li, layer, tr, cur_step)
                    && trig_roll_probability(layer->step_prob[tr][cur_step]);
                /* Per-track repeat_rate: the plain emit path repeats its
                 * note-on every bar_ticks*rr, so a plain step only sounds on
                 * the 0th bar of each rr-bar window. Gate the decorated
                 * one-shot the same way so SEQ_REPEAT_N behaves identically for
                 * both. Like mute/solo below, this silences output only -
                 * condition/probability state keeps evaluating every bar. */
                uint32_t rr = (layer->repeat_rate[tr] >= SEQ_REPEAT_2)
                              ? (uint32_t)layer->repeat_rate[tr] : 1u;
                bool rr_bar = ((eval_ticks / bar_ticks) % rr) == 0;
                /* Loop-bounce freeze: past the horizon nothing new fires,
                 * matching the periodic entries AMY holds back. Judged at the
                 * k==0 fire tick (swung and nudged), not the detection tick.
                 * Evaluation continues so PREV/condition state stays coherent
                 * for the unfreeze. */
                int32_t fire_tick = (int32_t)grid_tick + 1
                                  + (int32_t)sequencer_step_swing_offset(layer, cur_step)
                                  + (int32_t)layer->step_nudge[tr][cur_step];
                bool frozen = seq_freeze_blocks((uint32_t)fire_tick);
                /* Mute/solo silences output only: fire and s_track_last_played
                 * keep evaluating, so a track resumes its rhythmic position
                 * seamlessly on unmute instead of freezing. */
                if (fire && rr_bar && !frozen && sequencer_track_audible(layer, tr)) {
                    /* Render task cannot send: hand the job to the pump's
                     * urgent source (seq_trig_pump.c). */
                    sequencer_core_trig_enqueue(li, tr, cur_step, grid_tick);
                }
            }
            s_track_last_played[li][tr] = fire;
        }
    }
}

/* ── Public setters/getters ──────────────────────────────────────────────
 * Every setter re-emits the step, which re-resolves plain vs decorated; the
 * trig service reads the step fields live, so nothing else propagates. */
void sequencer_core_set_step_prob(uint8_t layer_idx, uint8_t track, uint8_t step,
                                  uint8_t prob_pct)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    layer->step_prob[track][step] = SEQ_CLAMP_U8(prob_pct, 0, 100);
    sequencer_emit_step(layer_idx, track, step);
}

uint8_t sequencer_core_get_step_prob(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    if (layer_idx >= s_num_layers) return 100;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return 100;
    return layer->step_prob[track][step];
}

void sequencer_core_set_step_ratchet(uint8_t layer_idx, uint8_t track, uint8_t step,
                                     uint8_t count)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    layer->step_ratchet[track][step] = SEQ_CLAMP_U8(count, 1, SEQ_MAX_RATCHET);
    sequencer_emit_step(layer_idx, track, step);
}

uint8_t sequencer_core_get_step_ratchet(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    if (layer_idx >= s_num_layers) return 1;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return 1;
    return layer->step_ratchet[track][step];
}

void sequencer_core_set_step_pitch_ofs(uint8_t layer_idx, uint8_t track, uint8_t step,
                                       int8_t ofs)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    ofs = (int8_t)SEQ_CLAMP_INT(ofs, -SEQ_STEP_PITCH_OFS_MAX, SEQ_STEP_PITCH_OFS_MAX);
    if (layer->step_pitch_ofs[track][step] == ofs) return;
    layer->step_pitch_ofs[track][step] = ofs;
    sequencer_emit_step(layer_idx, track, step);
    /* The re-emit rewrote the off tag at the NEW pitch, which a note already
     * sounding at the old pitch would never match (note-off matching:
     * seq_apply_track_note) - kill, same as the track pitch scroll. Drum tracks
     * skip note matching and PCM drums schedule no offs. */
    if (layer->type == SEQ_LAYER_MELODIC) {
        amy_event *kill = amy_helpers_event_begin();
        kill->synth    = layer->synth_id[track];
        kill->velocity = 0.0f;
        amy_helpers_event_send(kill);
    }
}

int8_t sequencer_core_get_step_pitch_ofs(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    if (layer_idx >= s_num_layers) return 0;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return 0;
    return layer->step_pitch_ofs[track][step];
}

/* Velocity offset, nudge and ratchet taper: the same shape as pitch_ofs minus
 * the kill event - none of them changes the emitted pitch, so a sounding note's
 * pending off tag still matches after the re-emit. */
void sequencer_core_set_step_velocity_adj(uint8_t layer_idx, uint8_t track, uint8_t step,
                                          int8_t pts)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    pts = (int8_t)SEQ_CLAMP_INT(pts, -SEQ_STEP_VEL_ADJ_MAX, SEQ_STEP_VEL_ADJ_MAX);
    if (layer->step_velocity_adj[track][step] == pts) return;
    layer->step_velocity_adj[track][step] = pts;
    sequencer_emit_step(layer_idx, track, step);
}

int8_t sequencer_core_get_step_velocity_adj(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    if (layer_idx >= s_num_layers) return 0;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return 0;
    return layer->step_velocity_adj[track][step];
}

void sequencer_core_set_step_nudge(uint8_t layer_idx, uint8_t track, uint8_t step,
                                   int8_t ticks)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    ticks = (int8_t)SEQ_CLAMP_INT(ticks, -SEQ_STEP_NUDGE_MAX, SEQ_STEP_NUDGE_MAX);
    if (layer->step_nudge[track][step] == ticks) return;
    layer->step_nudge[track][step] = ticks;
    sequencer_emit_track(layer_idx, track);
}

int8_t sequencer_core_get_step_nudge(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    if (layer_idx >= s_num_layers) return 0;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return 0;
    return layer->step_nudge[track][step];
}

void sequencer_core_set_step_frame(uint8_t layer_idx, uint8_t track, uint8_t step,
                                   uint8_t frame)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    frame = SEQ_CLAMP_U8(frame, 0, 64);
    if (layer->step_frame[track][step] == frame) return;
    layer->step_frame[track][step] = frame;
    sequencer_emit_track(layer_idx, track);
}

uint8_t sequencer_core_get_step_frame(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    if (layer_idx >= s_num_layers) return 0;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return 0;
    return layer->step_frame[track][step];
}

void sequencer_core_set_step_ratchet_taper(uint8_t layer_idx, uint8_t track, uint8_t step,
                                           int8_t pct)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    pct = (int8_t)SEQ_CLAMP_INT(pct, -SEQ_STEP_TAPER_MAX, SEQ_STEP_TAPER_MAX);
    if (layer->step_ratchet_taper[track][step] == pct) return;
    layer->step_ratchet_taper[track][step] = pct;
    sequencer_emit_step(layer_idx, track, step);
}

int8_t sequencer_core_get_step_ratchet_taper(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    if (layer_idx >= s_num_layers) return 0;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return 0;
    return layer->step_ratchet_taper[track][step];
}

void sequencer_core_set_step_every(uint8_t layer_idx, uint8_t track, uint8_t step,
                                   uint8_t n)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    layer->step_every[track][step] = SEQ_CLAMP_U8(n, 1, SEQ_STEP_EVERY_MAX);
    sequencer_emit_step(layer_idx, track, step);
}

uint8_t sequencer_core_get_step_every(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    if (layer_idx >= s_num_layers) return 1;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return 1;
    uint8_t n = layer->step_every[track][step];
    return n < 1 ? 1 : n;
}

void sequencer_core_set_step_prev(uint8_t layer_idx, uint8_t track, uint8_t step,
                                  bool on)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    layer->step_prev[track][step] = on ? 1u : 0u;
    sequencer_emit_step(layer_idx, track, step);
}

bool sequencer_core_get_step_prev(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    if (layer_idx >= s_num_layers) return false;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return false;
    return layer->step_prev[track][step] != 0;
}

/* ── Per-step note transform + quantize bypass ────────────────────────────
 * A non-NONE transform makes the step decorated. The quantize bypass rides on
 * the transform and only alters output while one is active: under NONE the base
 * note is already snapped and uniform per track. */
void sequencer_core_set_step_transform(uint8_t layer_idx, uint8_t track, uint8_t step,
                                       seq_step_transform_t mode, bool quant_bypass)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    if ((uint8_t)mode >= SEQ_STEP_TRANSFORM_COUNT) mode = SEQ_STEP_TRANSFORM_NONE;
    layer->step_transform[track][step]    = (uint8_t)mode;
    layer->step_quant_bypass[track][step] = quant_bypass ? 1u : 0u;
    sequencer_emit_step(layer_idx, track, step);
}

void sequencer_core_get_step_transform(uint8_t layer_idx, uint8_t track, uint8_t step,
                                       seq_step_transform_t *mode, bool *quant_bypass)
{
    seq_step_transform_t m = SEQ_STEP_TRANSFORM_NONE;
    bool q = false;
    if (layer_idx < s_num_layers) {
        const seq_layer_t *layer = &s_layers[layer_idx];
        if (track < layer->num_tracks && step < layer->num_steps) {
            m = (seq_step_transform_t)layer->step_transform[track][step];
            q = layer->step_quant_bypass[track][step] != 0;
        }
    }
    if (mode)         *mode         = m;
    if (quant_bypass) *quant_bypass = q;
}
