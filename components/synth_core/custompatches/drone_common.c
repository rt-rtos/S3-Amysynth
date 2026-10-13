/* drone_common.c - note emitter, chord voicing and envelope store shared by
 * the stutter and standard drones. Contract: drone_common.h. */

#include "custompatches/drone_common.h"
#include "quantizer.h"      /* quantizer_chord_intervals/_note_count */
#include "seq_clamp.h"
#include "voice_config.h"   /* VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_RELEASE_MIN_MS */
#include "sequencer_core.h" /* sequencer_core_clamp_patch_voices, SEQ_PATCH_* */
#include "seq_core_config.h"            /* SEQ_TRACK_OSC_BUDGET */
#include "custompatches/fm_voice.h"     /* FM_NUM_OPS */
#include "custompatches/additive_voice.h" /* ADD_MAX_PARTIALS */
#include "amy.h"
#include "amy_helpers.h"

#include <string.h>

uint8_t drone_voices_for(uint16_t patch)
{
    uint16_t v = sequencer_core_clamp_patch_voices(patch, DRONE_CHORD_MAX_NOTES);
    uint16_t opv = 0;
    if (patch >= SEQ_PATCH_FM_BASE && patch <= SEQ_PATCH_FM_MAX)
        opv = FM_NUM_OPS + 1u;
    else if (patch >= SEQ_PATCH_ADDITIVE_BASE && patch <= SEQ_PATCH_ADDITIVE_MAX)
        opv = ADD_MAX_PARTIALS + 1u;
    if (opv != 0u) {
        uint16_t max = SEQ_TRACK_OSC_BUDGET / opv;
        if (max < 1u) max = 1u;
        if (v > max) v = max;
    }
    return (uint8_t)((v > 0u) ? v : 1u);
}

void drone_notes_init(drone_notes_t *n)
{
    memset(n, 0, sizeof(*n));
    n->sounding_sub = -1;
}

void drone_note(drone_notes_t *n, uint8_t synth, bool on, float midi_note,
                uint32_t land_ms)
{
    uint32_t t = AMY_TIME_GEQ(land_ms, n->last_sched_ms) ? land_ms : n->last_sched_ms;
    amy_event *e = amy_helpers_event_begin();
    e->synth     = synth;
    e->midi_note = midi_note;
    e->velocity  = on ? 1.0f : 0.0f;
    e->time      = t;
    amy_helpers_event_send(e);
    n->last_sched_ms = t;
}

uint8_t drone_chord_tones(uint8_t root, chord_type_t chord, uint8_t voices,
                          uint8_t out[DRONE_CHORD_MAX_NOTES])
{
    const int8_t *formula = quantizer_chord_intervals(chord);
    uint8_t chord_n = quantizer_chord_note_count(chord);
    if (chord_n > DRONE_CHORD_MAX_NOTES) chord_n = DRONE_CHORD_MAX_NOTES;
    if (voices < 1u) voices = 1u;
    if (chord_n < 1u) {
        out[0] = root;
        return 1;
    }

    /* Tone indices in keep order: all of them in chord order when they fit,
     * else root, fifth, top, then the rest. */
    uint8_t order[DRONE_CHORD_MAX_NOTES];
    uint8_t on = 0;
    if (voices >= chord_n) {
        for (uint8_t i = 0; i < chord_n; i++) order[on++] = i;
    } else {
        bool taken[DRONE_CHORD_MAX_NOTES] = { false };
        order[on++] = 0;
        taken[0] = true;
        for (uint8_t i = 1; i < chord_n; i++) {
            if (formula[i] == 7) { order[on++] = i; taken[i] = true; break; }
        }
        uint8_t top = (uint8_t)(chord_n - 1u);
        if (!taken[top]) { order[on++] = top; taken[top] = true; }
        for (uint8_t i = 1; i < chord_n; i++) {
            if (!taken[i]) order[on++] = i;
        }
        on = voices;
    }

    for (uint8_t i = 0; i < on; i++) {
        out[i] = (uint8_t)SEQ_CLAMP_INT((int)root + (int)formula[order[i]], 0, 127);
    }
    return on;
}

static bool drone_note_in(const uint8_t *set, uint8_t n, uint8_t note)
{
    for (uint8_t i = 0; i < n; i++) {
        if (set[i] == note) return true;
    }
    return false;
}

void drone_apply_enabled(drone_notes_t *n, const drone_voicing_t *v,
                         uint32_t land_ms)
{
    uint8_t tgt[DRONE_CHORD_MAX_NOTES];
    uint8_t tn = 0;
    int16_t tsub = -1;
    if (v->sounding) {
        tn = drone_chord_tones(v->root, v->chord, v->voices, tgt);
        if (v->sub) {
            tsub = (int16_t)SEQ_CLAMP_INT((int)v->root + (int)v->sub_interval,
                                          DRONE_SUB_MIDI_MIN, DRONE_SUB_MIDI_MAX);
        }
    }

    for (uint8_t i = 0; i < n->sounding_n; i++) {
        if (!drone_note_in(tgt, tn, n->sounding[i])) {
            drone_note(n, v->main_synth, false, (float)n->sounding[i], land_ms);
        }
    }
    if (n->sounding_sub >= 0 && n->sounding_sub != tsub) {
        drone_note(n, v->sub_synth, false, (float)n->sounding_sub, land_ms);
    }
    for (uint8_t i = 0; i < tn; i++) {
        if (!drone_note_in(n->sounding, n->sounding_n, tgt[i])) {
            drone_note(n, v->main_synth, true, (float)tgt[i], land_ms);
        }
    }
    if (tsub >= 0 && n->sounding_sub != tsub) {
        drone_note(n, v->sub_synth, true, (float)tsub, land_ms);
    }

    memcpy(n->sounding, tgt, tn);
    n->sounding_n   = tn;
    n->sounding_sub = tsub;
}

void drone_release_all(drone_notes_t *n, uint8_t main_synth, uint8_t sub_synth,
                       uint32_t land_ms)
{
    for (uint8_t i = 0; i < n->sounding_n; i++) {
        drone_note(n, main_synth, false, (float)n->sounding[i], land_ms);
    }
    if (n->sounding_sub >= 0) {
        drone_note(n, sub_synth, false, (float)n->sounding_sub, land_ms);
    }
    n->sounding_n   = 0;
    n->sounding_sub = -1;
}

/* ── Envelope store ── */

static void drone_env_floor(seq_env_t *env)
{
    if (env->attack_ms < VOICE_ENV_ATTACK_MIN_MS)   env->attack_ms  = VOICE_ENV_ATTACK_MIN_MS;
    if (env->release_ms < VOICE_ENV_RELEASE_MIN_MS) env->release_ms = VOICE_ENV_RELEASE_MIN_MS;
}

void drone_env_get(const voice_params_t *vp, bool eg1, seq_env_t *out)
{
    if (out) *out = eg1 ? vp->env1 : vp->env;
}

bool drone_env_set(voice_params_t *vp, bool eg1, const seq_env_t *env)
{
    if (!env) return false;
    seq_env_t *dst = eg1 ? &vp->env1 : &vp->env;
    *dst = *env;
    drone_env_floor(dst);
    if (eg1) vp->env1_authored = true;
    else     vp->env_authored  = true;
    return true;
}

void drone_env_floor_vp(voice_params_t *vp)
{
    drone_env_floor(&vp->env);
    drone_env_floor(&vp->env1);
}
