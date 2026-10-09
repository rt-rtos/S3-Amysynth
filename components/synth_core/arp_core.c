#include "arp_core.h"
#include "sequencer_core.h"
#include "custompatches/fm_voice.h"  /* s_fm_voice + fm_voice_push_live (FM_CUSTOM) */
#include "custompatches/additive_voice.h"  /* s_additive_voice + push_live (ADDITIVE_CUSTOM) */
#include "amy_helpers.h"   /* amy_helpers_event_begin/send */
#include "voice_config.h"  /* canonical LFO depth scalars + shared wave map */
#include "seq_core_config.h" /* SEQ_LFO_SW_MAX_HZ (software-stepper rate cap) */
#include "quantizer.h"
#include "seq_clamp.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "diag_heap.h"
#include <math.h>
#include <string.h>

#ifndef CONFIG_SEQ_ARP_DEFAULT_ENABLED
#define CONFIG_SEQ_ARP_DEFAULT_ENABLED 0
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_SCALE
#define CONFIG_SEQ_ARP_DEFAULT_SCALE 0   /* Chromatic */
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_ROOT_NOTE
#define CONFIG_SEQ_ARP_DEFAULT_ROOT_NOTE 40  /* E2 */
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_GATE_PCT
#define CONFIG_SEQ_ARP_DEFAULT_GATE_PCT 75
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_OCTAVES
#define CONFIG_SEQ_ARP_DEFAULT_OCTAVES 1
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_PATCH
#define CONFIG_SEQ_ARP_DEFAULT_PATCH 138  /* DX7 E.Piano, matches melodic default */
#endif

/* Octaves of EG1 filter-cutoff sweep once the arp filter is authored+enabled.
 * Fixed: the arp exposes EG1 *timing*, not its depth. */
#define ARP_FILTER_EG1_DEPTH_OCT 3.0f

static const char *TAG = "arp_core";

_Static_assert(NOTE_DIV_PPQ == AMY_SEQUENCER_PPQ, "note_div ticks are AMY sequencer ticks");

typedef struct {
    bool       enabled;
    bool       solo_muted;    /* silenced because something is soloed; kept
                                 apart from `enabled` so the user's own on/off
                                 survives a solo round-trip                 */
    arp_dir_t  dir;
    uint8_t    octaves;       /* 1..ARP_OCT_MAX */
    note_div_t rate;
    uint8_t    gate_pct;      /* 10..100 */
    int16_t    slots[ARP_MAX_SLOTS];  /* raw chromatic MIDI, -1 = empty */
    uint8_t    scale_index;
    uint8_t    root_note;
    arp_quant_mode_t quant_mode;  /* what arp_snap snaps to; CHORD at init */
    uint16_t     patch;          /* full melodic catalog, 0..SEQ_PATCH_FULL_MAX */
    voice_params_t vp;           /* shared voice params: env, EG1, filter, LFO,
                                    each with its deferred-authority flag, plus
                                    amp_trim scaled into note velocity at emit
                                    time. */
    uint16_t     portamento_ms;  /* glide time, 0=off. The memset default matches
                                    AMY's own reset value, so no explicit init. */
} arp_state_t;

static arp_state_t s_arp;

/* Refresh coalescing: setters mark dirty; arp_core_service() re-emits once per
 * UI frame. A re-emit clears + re-schedules up to ARP_MAX_STEPS events through
 * the shared AMY event mutex, so coalescing keeps an encoder spin from bursting
 * mutex traffic on Core 0. */
static volatile bool s_arp_dirty = false;

static inline void arp_mark_dirty(void) { s_arp_dirty = true; }

/* Max distinct arp steps we can schedule = slots * max octaves. Each step uses
 * two tags (on + on+1); we space step tags by 2. */
#define ARP_MAX_STEPS (ARP_MAX_SLOTS * ARP_OCT_MAX)

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Shared ascending bubble sort for collect helpers. */
static void arp_sort_asc(uint8_t *out, uint8_t n)
{
    for (uint8_t i = 0; i + 1 < n; i++)
        for (uint8_t j = 0; j + 1 < n - i; j++)
            if (out[j] > out[j + 1]) { uint8_t t = out[j]; out[j] = out[j+1]; out[j+1] = t; }
}

/* UP mode: REST is a transparent skip — all non-empty notes are collected.
 * Returns sorted ascending; caller uses sorted[note_idx] directly. */
static uint8_t arp_collect_up(uint8_t out[ARP_MAX_SLOTS])
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++) {
        if (s_arp.slots[i] == -1) break;          /* empty sentinel: stop */
        if (s_arp.slots[i] == ARP_REST) continue; /* REST in UP: skip */
        out[n++] = (uint8_t)s_arp.slots[i];
    }
    arp_sort_asc(out, n);
    return n;
}

/* DOWN mode: identical skip-REST logic to UP; caller reverses with
 * sorted[count-1-note_idx]. */
static uint8_t arp_collect_down(uint8_t out[ARP_MAX_SLOTS])
{
    return arp_collect_up(out);
}

/* Snap a chromatic note per the quant mode. OWN: the arp's own scale/root.
 * CHORD: the progression chord the rows are playing, rooted in octave 4 as the
 * global path is; with no chord applied it falls through to GLOBAL. GLOBAL: the
 * global quantizer, where a disabled quantizer means no snapping at all. */
static uint8_t arp_snap(uint8_t chromatic)
{
    uint8_t root      = s_arp.root_note;
    uint8_t scale_idx = s_arp.scale_index;
    if (s_arp.quant_mode != ARP_QUANT_OWN) {
        uint8_t pc;
        if (s_arp.quant_mode == ARP_QUANT_CHORD &&
            sequencer_core_progression_arp_chord(&pc, &scale_idx)) {
            root = (uint8_t)(60u + pc);
        } else if (!sequencer_core_get_quantizer_enabled()) {
            return sequencer_core_clamp_melodic_note((int32_t)chromatic);
        } else {
            root      = sequencer_core_get_quantizer_root_note();
            scale_idx = sequencer_core_get_quantizer_scale();
        }
    }
    const musical_scale_t *scale = quantizer_get_scale(scale_idx);
    uint8_t snapped = quantizer_snap_midi_note(chromatic, root, scale);
    return sequencer_core_clamp_melodic_note((int32_t)snapped);
}

/* Clear every possible arp tag slot (removes all scheduled repeating events). */
void arp_core_clear_all(void)
{
    uint32_t base = sequencer_core_arp_tag_base();
    for (uint8_t i = 0; i < ARP_MAX_STEPS; i++) {
        sequencer_core_arp_clear_note(base + (uint32_t)i * 2u);
    }
}

/* All-notes-off for the arp synth. MUST follow every arp_core_clear_all() that
 * can run while notes sound: clearing also removes the scheduled note-OFF tags,
 * so a ringing note loses the only event that would ever silence it and hangs
 * at sustain forever. */
static void arp_kill_voices(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth    = sequencer_core_arp_synth();
    e->velocity = 0.0f;
    amy_helpers_event_send(e);
}

/* Push the arp filter plus its envelope routing matrix (seq_filter_t.eg_depth,
 * in its own event). Mirrors melodic_filter_apply(); the EG1 breakpoints ride
 * along whenever an EG1 rail is live (sequencer_core_push_envelope_eg1()). */
static void arp_apply_filter(const seq_filter_t *f)
{
    if (!f) return;
    amy_event *e = amy_helpers_event_begin();
    e->synth = sequencer_core_arp_synth();
    if (f->enabled) {
        e->filter_type = f->filter_type;
        e->filter_freq_coefs[COEF_CONST] = sequencer_core_filter_const_hz(f);
        e->resonance = f->resonance;
    } else {
        e->filter_type = FILTER_NONE;
    }
    /* KS string decay from the authored feedback field; 0 = never authored,
     * keep AMY's build-time 0.9 default. Pluck duty is written on every KS
     * push: its zero offset IS the 0.5 default, so no authored gate. */
    if (s_arp.patch == SEQ_PATCH_KS) {
        if (f->feedback > 0.0f) {
            e->feedback = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
        }
        e->duty_coefs[COEF_CONST] = 0.5f + f->ks_duty_ofs;
    }
    amy_helpers_event_send(e);

    sequencer_core_push_eg_depths(sequencer_core_arp_synth(),
                                  sequencer_core_patch_voice_osc(s_arp.patch), f,
                                  s_arp.vp.filter_authored);
    if (sequencer_core_is_wavetable_patch(s_arp.patch)) {
        uint8_t carrier, coupled;
        uint8_t mask = sequencer_core_lfo_native_layout(s_arp.patch, &carrier, &coupled)
                     ? coupled : 0x01u;
        sequencer_core_push_eg_scan(sequencer_core_arp_synth(), mask, f,
                                    s_arp.vp.filter_authored);
    }

    if (seq_filter_eg1_live(f)) {
        sequencer_core_push_envelope_eg1(sequencer_core_arp_synth(), 0,
                                         &s_arp.vp.env1);
    }
}

/* Recompute and push the LFO carrier frequency at the current BPM. Called by
 * sequencer_core_set_bpm() after s_bpm updates. Must NOT be called from the
 * render body - amy_queue_lock is held there; set_bpm() runs on the UI task. */
void arp_core_refresh_lfo_freq(void)
{
    if (!s_arp.vp.lfo_authored || !s_arp.vp.lfo.enabled)
        return;
    /* Only native-layout patches carry a BPM-synced carrier; the software
     * stepper tracks BPM per step on its own. */
    uint8_t carrier;
    if (!sequencer_core_lfo_native_layout(s_arp.patch, &carrier, NULL))
        return;

    amy_event *e = amy_helpers_event_begin();
    e->synth                  = sequencer_core_arp_synth();
    e->osc                    = carrier;
    e->freq_coefs[COEF_CONST] = lfo_rate_to_hz(s_arp.vp.lfo.rate,
                                                     sequencer_core_get_bpm());
    amy_helpers_event_send(e);

    /* Keep the wobble modulator (carrier+1) BPM-synced as well. */
    e = amy_helpers_event_begin();
    e->synth                  = sequencer_core_arp_synth();
    e->osc                    = (uint8_t)(carrier + 1u);
    e->freq_coefs[COEF_CONST] = lfo_rate_to_hz((note_div_t)s_arp.vp.lfo.wob_rate,
                                                     sequencer_core_get_bpm());
    amy_helpers_event_send(e);
}

/* Push the current glide time to the arp synth. Leaving e->osc and e->velocity
 * unset makes patches_event_has_voices() fan this out to every voice's base
 * osc (amy.c/patches.c dispatch). */
static void arp_push_portamento(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth         = sequencer_core_arp_synth();
    e->portamento_ms = s_arp.portamento_ms;
    amy_helpers_event_send(e);
}

/* (Re)build the arp synth slot for the current patch and params, then
 * re-impose any authored ADSR / filter. Mirrors drone_rebuild().
 *
 * Scheduled events are cleared FIRST and re-emitted afterwards (arp_mark_dirty
 * at the end); why: see sequencer_reconfigure_layer_paused(). Cost: the
 * arp is quiet for at most one UI frame around a rebuild. */
static void arp_rebuild(void)
{
    arp_core_clear_all();
    arp_kill_voices();

    sequencer_core_arp_configure(s_arp.patch, sequencer_core_arp_voices(),
                                 s_arp.vp.filter_authored, s_arp.vp.filter.feedback,
                                 s_arp.vp.filter.ks_duty_ofs);
    /* Raw wave/wavetable patches have no built-in EG0; always push the
     * envelope so notes decay. Juno/DX7 strings and bass/FM presets carry
     * their own envelope as part of their character: only override when
     * the user authored one. */
    if (sequencer_core_is_wave_patch(s_arp.patch) || s_arp.vp.env_authored) {
        sequencer_core_push_envelope_osc(sequencer_core_arp_synth(),
                                     sequencer_core_patch_voice_osc(s_arp.patch), &s_arp.vp.env);
    }
    /* Native-layout patches take the native LFO here; the software stepper
     * skips them. */
    uint8_t carrier, coupled;
    if (sequencer_core_lfo_native_layout(s_arp.patch, &carrier, &coupled)) {
        bool lfo_on = s_arp.vp.lfo_authored && s_arp.vp.lfo.enabled;
        voice_apply_native_lfo_topo(sequencer_core_arp_synth(),
                                    lfo_on ? &s_arp.vp.lfo : NULL,
                                    sequencer_core_get_bpm(),
                                    carrier, coupled, coupled);
    }
    /* arp_apply_filter also pushes the routing matrix, and the EG1 breakpoints
     * whenever an EG1 rail is live. */
    if (s_arp.vp.filter_authored) {
        arp_apply_filter(&s_arp.vp.filter);
    }
    /* EG1: push only when authored. Otherwise an instrument that routes its
     * own bp1 keeps its patch-string values. */
    if (s_arp.vp.env1_authored) {
        sequencer_core_push_envelope_eg1(sequencer_core_arp_synth(), 0, &s_arp.vp.env1);
    }
    /* Distortion is per-osc state the reconfigure above cleared - reassert. */
    arp_reapply_dist();
    /* Any reconfigure above resets AMY's per-osc portamento_alpha to 0 -
     * reassert. */
    arp_push_portamento();

    /* Re-emit the schedule cleared at the top (coalesced onto the next
     * arp_core_service() frame; no-op while disabled). */
    arp_mark_dirty();
}

/* ── Public API ──────────────────────────────────────────────────────── */

void arp_core_init(void)
{
    memset(&s_arp, 0, sizeof(s_arp));
    voice_params_init_defaults(&s_arp.vp);   /* unauthored, amp trim at unity */
    /* Seed the bipolar EG1->cutoff sweep so an enabled arp filter has its
     * plucky-sweep character; the graph editor's EG1 page edits it
     * (-8..+8 oct, 0 = no sweep). */
    s_arp.vp.filter.eg_depth[1][SEQ_EGT_CUTOFF] = ARP_FILTER_EG1_DEPTH_OCT;
    s_arp.enabled     = CONFIG_SEQ_ARP_DEFAULT_ENABLED;
    s_arp.dir         = ARP_UP;
    s_arp.octaves     = CONFIG_SEQ_ARP_DEFAULT_OCTAVES;
    s_arp.rate        = NOTE_DIV_1_16;
    s_arp.gate_pct    = CONFIG_SEQ_ARP_DEFAULT_GATE_PCT;
    s_arp.scale_index = CONFIG_SEQ_ARP_DEFAULT_SCALE;
    s_arp.root_note   = CONFIG_SEQ_ARP_DEFAULT_ROOT_NOTE;
    s_arp.patch       = CONFIG_SEQ_ARP_DEFAULT_PATCH;
    s_arp.quant_mode  = ARP_QUANT_CHORD;
    /* Default ADSR mirrors the melodic compile-time defaults; unauthored until
     * the user commits in the graph editor (the patch's own env wins). */
    s_arp.vp.env.attack_ms   = 4;    /* tiny curve, prevents a digital click */
    s_arp.vp.env.decay_ms    = 250;
    s_arp.vp.env.sustain_pct = 30;   /* low sustain keeps the sequence energetic */
    s_arp.vp.env.release_ms  = 200;
    s_arp.vp.env.eg_type     = 0;    /* ENVELOPE_NORMAL */
    /* EG1 is deliberately slower than the amp env: the classic "plucky amp
     * decay, slower filter tail" shape. */
    s_arp.vp.env1.attack_ms   = 15;
    s_arp.vp.env1.decay_ms    = 400;
    s_arp.vp.env1.sustain_pct = 20;
    s_arp.vp.env1.release_ms  = 300;
    s_arp.vp.env1.eg_type     = 0;   /* ENVELOPE_NORMAL */
    /* Default filter: bypass (unauthored; patch's filter wins until commit). */
    s_arp.vp.filter.filter_type = 0;   /* SEQ_FILTER_NONE */
    s_arp.vp.filter.cutoff_hz   = 800.0f;
    s_arp.vp.filter.resonance   = 1.0f;
    /* Default LFO: disabled (bypass until the user commits). */
    s_arp.vp.lfo.wave   = LFO_WAVE_SINE;
    s_arp.vp.lfo.rate   = NOTE_DIV_1_1;
    s_arp.vp.lfo.depth  = 50;
    s_arp.vp.lfo.targets = LFO_TGT_BIT(LFO_TARGET_FILTER);
    s_arp.octaves = SEQ_CLAMP_U8(s_arp.octaves, 1, ARP_OCT_MAX);
    if (s_arp.scale_index >= quantizer_scale_count()) s_arp.scale_index = 0;
    for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++) s_arp.slots[i] = -1;

    DIAG_HEAP_CHECK("arp_init: before arp_configure");
    arp_rebuild();
    DIAG_HEAP_CHECK("arp_init: after arp_configure");
    /* If the arp boots enabled, let the first service tick emit rather than
     * emitting inline during init. */
    arp_mark_dirty();
    ESP_LOGI(TAG, "arp_core initialized (synth %u)", sequencer_core_arp_synth());
}

void arp_core_refresh(void)
{
    arp_core_clear_all();
    /* The clear above deleted the scheduled note-offs of anything still
     * sounding - silence them now or they hang forever. On disable this is the
     * only note-off they will ever get. */
    arp_kill_voices();

    if (!s_arp.enabled || s_arp.solo_muted) {
        return;
    }

    /* ── SLOT mode: play slots in stored order, rests preserved ── */
    if (s_arp.dir == ARP_SLOT) {
        uint8_t active = 0;
        for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++)
            if (s_arp.slots[i] != -1) active++;
        if (active == 0) return;

        uint32_t rate    = note_div_ticks(s_arp.rate);
        uint8_t  steps   = (uint8_t)(active * s_arp.octaves);
        uint32_t period  = (uint32_t)steps * rate;
        uint32_t gate    = (rate * s_arp.gate_pct) / 100u;
        if (gate < 1) gate = 1;

        uint32_t tag_base = sequencer_core_arp_tag_base();
        uint8_t  step_i   = 0;

        /* Per-target amp trim; clamp so velocity stays <=1 (AMY cap). */
        float arp_vel = 0.9f * s_arp.vp.amp_trim;
        if (arp_vel > 1.0f) arp_vel = 1.0f;

        for (uint8_t oct = 0; oct < s_arp.octaves; oct++) {
            for (uint8_t si = 0; si < ARP_MAX_SLOTS; si++) {
                int16_t v = s_arp.slots[si];
                if (v == -1) continue;   /* unused: not part of the cycle */

                uint32_t tick_on = 1u + (uint32_t)step_i * rate;

                if (v != ARP_REST) {
                    int32_t chromatic = (int32_t)v + (int32_t)oct * 12;
                    uint8_t play_note = arp_snap(sequencer_core_clamp_melodic_note(chromatic));
                    sequencer_core_arp_emit_note(tag_base + (uint32_t)step_i * 2u,
                                                 play_note, arp_vel, tick_on, gate, period);
                }
                /* REST: tag stays cleared (arp_clear_all already ran); step_i still advances. */
                step_i++;
            }
        }
        return;
    }

    /* ── UP / DOWN: sorted pitch order ── */
    uint8_t sorted[ARP_MAX_SLOTS];
    uint8_t count = (s_arp.dir == ARP_DOWN)
                    ? arp_collect_down(sorted)
                    : arp_collect_up(sorted);
    if (count == 0) {
        return;  /* nothing to play */
    }

    uint32_t rate    = note_div_ticks(s_arp.rate);
    uint8_t  steps   = (uint8_t)(count * s_arp.octaves);
    uint32_t period  = (uint32_t)steps * rate;
    uint32_t gate    = (rate * s_arp.gate_pct) / 100u;
    if (gate < 1) gate = 1;

    uint32_t tag_base = sequencer_core_arp_tag_base();

    /* Apply per-target amp trim for UP/DOWN modes. */
    float arp_vel = 0.9f * s_arp.vp.amp_trim;
    if (arp_vel > 1.0f) arp_vel = 1.0f;

    for (uint8_t i = 0; i < steps; i++) {
        /* index within the sorted set, advancing by octave every `count`. */
        uint8_t note_idx = i % count;
        uint8_t octave   = i / count;
        /* Direction: UP walks sorted ascending; DOWN walks it in reverse. */
        uint8_t pick = (s_arp.dir == ARP_DOWN)
                       ? sorted[count - 1 - note_idx]
                       : sorted[note_idx];
        int32_t chromatic = (int32_t)pick + (int32_t)octave * 12;
        uint8_t play_note = arp_snap(sequencer_core_clamp_melodic_note(chromatic));

        uint32_t tick_on = 1u + (uint32_t)i * rate;
        sequencer_core_arp_emit_note(tag_base + (uint32_t)i * 2u,
                                     play_note, arp_vel, tick_on, gate, period);
    }
}

void arp_set_enabled(bool enabled)
{
    if (s_arp.enabled == enabled) return;
    s_arp.enabled = enabled;
    arp_mark_dirty();
    ESP_LOGI(TAG, "arp %s", enabled ? "enabled" : "disabled");
}

void arp_set_solo_muted(bool muted)
{
    if (s_arp.solo_muted == muted) return;
    s_arp.solo_muted = muted;
    /* Refresh immediately rather than marking dirty: solo is toggled from the
     * UI task, the same task that services the arp, so there is no reason to
     * let up to one frame of arp notes through after the user hits solo. */
    arp_core_refresh();
}

void arp_set_direction(arp_dir_t dir)
{
    if (s_arp.dir == dir) return;
    s_arp.dir = dir;
    arp_mark_dirty();
}

void arp_set_octaves(uint8_t octaves)
{
    octaves = SEQ_CLAMP_U8(octaves, 1, ARP_OCT_MAX);
    if (s_arp.octaves == octaves) return;
    s_arp.octaves = octaves;
    arp_mark_dirty();
}

void arp_set_rate(note_div_t rate)
{
    if (rate >= NOTE_DIV_COUNT) return;
    if (s_arp.rate == rate) return;
    s_arp.rate = rate;
    arp_mark_dirty();
}

void arp_set_gate_pct(uint8_t gate_pct)
{
    gate_pct = SEQ_CLAMP_U8(gate_pct, 10, 100);
    if (s_arp.gate_pct == gate_pct) return;
    s_arp.gate_pct = gate_pct;
    arp_mark_dirty();
}

void arp_set_scale(uint8_t scale_index)
{
    if (scale_index >= quantizer_scale_count()) scale_index = 0;
    if (s_arp.scale_index == scale_index) return;
    s_arp.scale_index = scale_index;
    arp_mark_dirty();
}

void arp_set_root_note(uint8_t root_note)
{
    root_note = SEQ_CLAMP_U8(root_note, 0, 127);
    if (s_arp.root_note == root_note) return;
    s_arp.root_note = root_note;
    arp_mark_dirty();
}

void arp_set_quant_mode(arp_quant_mode_t mode)
{
    if ((unsigned)mode >= ARP_QUANT_COUNT) mode = ARP_QUANT_CHORD;
    if (s_arp.quant_mode == mode) return;
    s_arp.quant_mode = mode;
    arp_mark_dirty();
}

void arp_set_patch(uint16_t patch_number)
{
    /* Same ceiling as melodic; kind dispatch happens in
     * sequencer_core_arp_configure(). */
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_FULL_MAX);
    if (s_arp.patch == patch_number) return;
    s_arp.patch = patch_number;
    arp_rebuild();
    /* Patch reconfig does not change scheduling; no re-emit needed. */
}

void arp_core_fm_voice_changed(uint8_t what)
{
#if CONFIG_SYNTH_CUSTOM_FM
    if (s_arp.patch == SEQ_PATCH_FM_CUSTOM) {
        fm_voice_push(sequencer_core_arp_synth(), &s_fm_voice, what);
    }
#else
    (void)what;
#endif
}

void arp_core_additive_voice_changed(void)
{
#if CONFIG_SYNTH_ADDITIVE
    if (s_arp.patch == SEQ_PATCH_ADDITIVE_CUSTOM) {
        additive_voice_push_live(sequencer_core_arp_synth(), &s_additive_voice);
    }
#endif
}

void arp_get_envelope(seq_env_t *out)
{
    if (out) *out = s_arp.vp.env;
}

void arp_set_envelope(const seq_env_t *env)
{
    if (!env) return;
    s_arp.vp.env = *env;
    s_arp.vp.env.attack_ms  = SEQ_CLAMP_U32(s_arp.vp.env.attack_ms,
                                            VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    s_arp.vp.env.release_ms = SEQ_CLAMP_U32(s_arp.vp.env.release_ms,
                                            VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    s_arp.vp.env_authored = true;
    sequencer_core_push_envelope_osc(sequencer_core_arp_synth(),
                                     sequencer_core_patch_voice_osc(s_arp.patch), &s_arp.vp.env);
    ESP_LOGI(TAG, "arp env -> A%u D%u S%u%% R%u",
             (unsigned)s_arp.vp.env.attack_ms, (unsigned)s_arp.vp.env.decay_ms,
             (unsigned)s_arp.vp.env.sustain_pct, (unsigned)s_arp.vp.env.release_ms);
}

void arp_get_envelope2(seq_env_t *out)
{
    if (out) *out = s_arp.vp.env1;
}

void arp_set_envelope2(const seq_env_t *env)
{
    if (!env) return;
    s_arp.vp.env1 = *env;
    s_arp.vp.env1.attack_ms  = SEQ_CLAMP_U32(s_arp.vp.env1.attack_ms,
                                             VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    s_arp.vp.env1.release_ms = SEQ_CLAMP_U32(s_arp.vp.env1.release_ms,
                                             VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    s_arp.vp.env1_authored = true;
    sequencer_core_push_envelope_eg1(sequencer_core_arp_synth(), 0, &s_arp.vp.env1);
    ESP_LOGI(TAG, "arp env1 -> A%u D%u S%u%% R%u",
             (unsigned)s_arp.vp.env1.attack_ms, (unsigned)s_arp.vp.env1.decay_ms,
             (unsigned)s_arp.vp.env1.sustain_pct, (unsigned)s_arp.vp.env1.release_ms);
}

void arp_get_filter(seq_filter_t *out)
{
    if (out) *out = s_arp.vp.filter;
}

/* ── Editor live-preview (AMY only; arp store + authored flags untouched) ── */

void arp_preview_envelope(const seq_env_t *env)
{
    if (!env) return;
    sequencer_core_push_envelope_osc(sequencer_core_arp_synth(),
                                     sequencer_core_patch_voice_osc(s_arp.patch), env);
}

void arp_preview_envelope2(const seq_env_t *env)
{
    if (!env) return;
    sequencer_core_push_envelope_eg1(sequencer_core_arp_synth(), 0, env);
}

void arp_preview_filter(const seq_filter_t *f)
{
    arp_apply_filter(f);
}

void arp_set_filter(const seq_filter_t *f)
{
    if (!f) return;
    s_arp.vp.filter = *f;
    s_arp.vp.filter_authored = true;
    arp_apply_filter(&s_arp.vp.filter);
    ESP_LOGI(TAG, "arp filter -> type%u %.0fHz Q%.2f en=%d",
             s_arp.vp.filter.filter_type, (double)s_arp.vp.filter.cutoff_hz,
             (double)s_arp.vp.filter.resonance, s_arp.vp.filter.enabled);
}

void arp_get_dist(seq_dist_t *out)
{
    if (out) *out = s_arp.vp.dist;
}

void arp_preview_dist(const seq_dist_t *d)
{
    voice_apply_dist(sequencer_core_arp_synth(), d);
}

void arp_set_dist(const seq_dist_t *d)
{
    if (!d) return;
    s_arp.vp.dist = *d;
    voice_dist_clamp(&s_arp.vp.dist);
    s_arp.vp.dist_authored = true;
    voice_apply_dist(sequencer_core_arp_synth(), &s_arp.vp.dist);
    ESP_LOGI(TAG, "arp dist -> type%u drv%u bit%u rte%u mix%u",
             s_arp.vp.dist.type, s_arp.vp.dist.drive, s_arp.vp.dist.bits,
             s_arp.vp.dist.rate, s_arp.vp.dist.mix);
}

/* Re-assert after any voice rebuild (patch change): AMY drops the stage with
 * the osc pool, and the arp's rebuild path is not the wave-cfg one. */
void arp_reapply_dist(void)
{
    if (s_arp.vp.dist_authored)
        voice_apply_dist(sequencer_core_arp_synth(), &s_arp.vp.dist);
}

void arp_get_lfo(seq_lfo_t *out)
{
    if (out) *out = s_arp.vp.lfo;
}

void arp_set_lfo(const seq_lfo_t *lfo)
{
    if (!lfo) return;
    s_arp.vp.lfo = *lfo;
    s_arp.vp.lfo_authored = true;
    /* Native carrier patches take the edit immediately via the shared applier
     * (mirrors the melodic editor path); software-stepper patches read
     * s_arp.vp.lfo live each frame, nothing to push. */
    uint8_t carrier, coupled;
    if (sequencer_core_lfo_native_layout(s_arp.patch, &carrier, &coupled)) {
        voice_apply_native_lfo_topo(sequencer_core_arp_synth(),
                                    lfo->enabled ? &s_arp.vp.lfo : NULL,
                                    sequencer_core_get_bpm(),
                                    carrier, coupled, coupled);
    }
    ESP_LOGI(TAG, "arp LFO -> en=%d wave=%u rate=%u depth=%u tgt=0x%02x",
             lfo->enabled, (unsigned)lfo->wave, (unsigned)lfo->rate,
             (unsigned)lfo->depth, (unsigned)lfo->targets);
}

void arp_set_slot(uint8_t idx, int16_t chromatic_note)
{
    if (idx >= ARP_MAX_SLOTS) return;
    if (chromatic_note >= 0) {
        chromatic_note = (int16_t)sequencer_core_clamp_melodic_note(chromatic_note);
    } else if (chromatic_note == ARP_REST) {
        /* leave as -2: deliberate silent step */
    } else {
        chromatic_note = -1;
    }
    if (s_arp.slots[idx] == chromatic_note) return;
    s_arp.slots[idx] = chromatic_note;
    arp_mark_dirty();
}

/* Public dirty-mark: lets the transport request a coalesced re-emit on resume,
 * since emission is gated on the sequencer playing and nothing re-arms the
 * schedule while paused. */
void arp_core_mark_dirty(void)
{
    arp_mark_dirty();
}

/* ── Software LFO fallback ───────────────────────────────────────────────
 * Native-layout patches get the AMY-native LFO (arp_rebuild); every other
 * patch runs the 20 Hz software stepper (canonical impl:
 * sequencer_core_lfo_service; split: ENGINE-SEMANTICS.md, "LFO: native carrier
 * vs software stepper"). */
static float   s_swlfo_phase   = 0.0f;
static float   s_swlfo_rnd    = 0.0f;
static bool    s_swlfo_active = false;
static uint8_t s_swlfo_targets = 0;   /* targets driven while active - the set
                                       * to restore, even if edited since */

/* seq_core_editors.c internals shared with this stepper. Mirrored prototypes:
 * seq_core_internal.h cannot be included here (it defines a TU-local TAG). */
float lfo_next_rand(void);
void  lfo_push_amp(uint8_t synth_id, uint16_t patch, float amp);
void  lfo_push_target_neutral(uint8_t synth_id, uint16_t patch,
                              lfo_target_t target);

/* lfo_rate_to_hz capped to the stepper's usable band - mirrors seq_lfo_sw_hz
 * (seq_core_internal.h); needs >= 4 stepper samples per LFO cycle. */
static inline float arp_swlfo_hz(note_div_t rate, uint16_t bpm)
{
    float hz = lfo_rate_to_hz(rate, bpm);
    return (hz > SEQ_LFO_SW_MAX_HZ) ? SEQ_LFO_SW_MAX_HZ : hz;
}

static float arp_swlfo_eval(lfo_wave_t wave, float ph)
{
    switch (wave) {
        case LFO_WAVE_SINE:     return sinf(2.0f * 3.14159265f * ph);
        case LFO_WAVE_TRIANGLE: return (ph < 0.5f) ? (4.0f * ph - 1.0f)
                                                   : (3.0f - 4.0f * ph);
        case LFO_WAVE_SAW_UP:   return 2.0f * ph - 1.0f;
        case LFO_WAVE_SAW_DOWN: return 1.0f - 2.0f * ph;
        case LFO_WAVE_SQUARE:   return (ph < 0.5f) ? 1.0f : -1.0f;
        case LFO_WAVE_RANDOM:   return s_swlfo_rnd;
        default:                return 0.0f;
    }
}

static void arp_swlfo_service(void)
{
    const seq_lfo_t *lfo = &s_arp.vp.lfo;
    /* Native-layout patches are served by arp_rebuild; stepping them here
     * would double-modulate (mirrors sequencer_core_lfo_service). */
    bool native = sequencer_core_lfo_native_layout(s_arp.patch, NULL, NULL);
    bool want = s_arp.enabled && lfo->enabled && lfo->targets != 0 && !native;

    if (!want) {
        if (s_swlfo_active) {
            /* Restore every rail the stepper was driving (mirrors
             * lfo_restore_target_neutrals in seq_core_editors.c). */
            s_swlfo_active = false;
            uint8_t syn = sequencer_core_arp_synth();
            for (int t = 0; t < LFO_TARGET_COUNT; t++) {
                if (!(s_swlfo_targets & LFO_TGT_BIT(t))) continue;
                if (t == LFO_TARGET_FILTER)
                    sequencer_core_push_filter(syn, &s_arp.vp.filter,
                                               s_arp.patch == SEQ_PATCH_KS);
                else if (t == LFO_TARGET_DIST_DRIVE || t == LFO_TARGET_DIST_MIX) {
                    /* One restore event covers both dist bits. */
                    if (t == LFO_TARGET_DIST_DRIVE ||
                        !(s_swlfo_targets & LFO_TGT_BIT(LFO_TARGET_DIST_DRIVE)))
                        voice_apply_dist(syn, &s_arp.vp.dist);
                }
                else
                    lfo_push_target_neutral(syn, s_arp.patch, (lfo_target_t)t);
            }
        }
        return;
    }

    if (!s_swlfo_active) {
        s_swlfo_active = true;
        s_swlfo_phase  = 0.0f;
    }
    /* want requires !native, so the stepper only ever runs on PATCH-mode
     * tracks - record the full target set for the deactivation restore. */
    s_swlfo_targets = lfo->targets;

    float ph = s_swlfo_phase +
               arp_swlfo_hz(lfo->rate, sequencer_core_get_bpm()) * 0.05f;
    if (ph >= 1.0f) {
        ph -= 1.0f;
        if (lfo->wave == LFO_WAVE_RANDOM) s_swlfo_rnd = lfo_next_rand();
    }
    s_swlfo_phase = ph;

    float val = arp_swlfo_eval(lfo->wave, ph);
    float d   = (float)lfo->depth / 100.0f;

    if (!native) {
    amy_event *e = amy_helpers_event_begin();
    e->synth = sequencer_core_arp_synth();
    if (LFO_HAS_TGT(lfo, LFO_TARGET_FILTER)) {
        float base = (s_arp.vp.filter.enabled && s_arp.vp.filter.cutoff_hz > 0.0f)
                     ? sequencer_core_filter_const_hz(&s_arp.vp.filter) : 1000.0f;
        e->filter_freq_coefs[COEF_CONST] =
            base * powf(2.0f, voice_lfo_filter_octaves(lfo) * val);
    }
    if (LFO_HAS_TGT(lfo, LFO_TARGET_PAN))
        e->pan_coefs[COEF_CONST] = 0.5f + d * 0.5f * val;
    /* SCAN needs a wavetable voice - WAVE mode / native only. */
    amy_helpers_event_send(e);
    if (LFO_HAS_TGT(lfo, LFO_TARGET_AMP))
        lfo_push_amp(sequencer_core_arp_synth(), s_arp.patch,
                     1.0f - d * (0.5f - 0.5f * val));
    } /* !native */

    /* DIST on a PATCH-mode arp: step around the committed dist block
     * (voice_push_dist_lfo). */
    if (!native && (lfo->targets & LFO_TGT_DIST_MASK))
        voice_push_dist_lfo(sequencer_core_arp_synth(), &s_arp.vp.dist, lfo, val);

    if (!native && LFO_HAS_TGT(lfo, LFO_TARGET_PITCH)) {
        /* Absolute Hz anchored on SEQ_LFO_PITCH_BASE_HZ, sent to osc 0 only
         * (ENGINE-SEMANTICS.md, software stepper). */
        amy_event *pe = amy_helpers_event_begin();
        pe->synth = sequencer_core_arp_synth();
        pe->osc   = 0;
        pe->freq_coefs[COEF_CONST] = SEQ_LFO_PITCH_BASE_HZ * powf(2.0f, d * VOICE_LFO_DEPTH_PITCH * val);
        amy_helpers_event_send(pe);
    }
}

void arp_core_service(void)
{
    /* The software LFO runs every frame regardless of the dirty flag - it is a
     * modulator, not a re-emit. */
    arp_swlfo_service();
    if (!s_arp_dirty) return;
    s_arp_dirty = false;
    arp_core_refresh();
}

/* ── Getters ─────────────────────────────────────────────────────────── */

bool         arp_get_enabled(void)    { return s_arp.enabled; }
arp_dir_t    arp_get_direction(void)  { return s_arp.dir; }
uint8_t      arp_get_octaves(void)    { return s_arp.octaves; }
note_div_t   arp_get_rate(void)       { return s_arp.rate; }
uint8_t      arp_get_gate_pct(void)   { return s_arp.gate_pct; }
uint8_t      arp_get_scale(void)      { return s_arp.scale_index; }
uint8_t      arp_get_root_note(void)  { return s_arp.root_note; }
arp_quant_mode_t arp_get_quant_mode(void) { return s_arp.quant_mode; }
uint16_t     arp_get_patch(void)      { return s_arp.patch; }

int16_t arp_get_slot(uint8_t idx)
{
    if (idx >= ARP_MAX_SLOTS) return -1;
    return s_arp.slots[idx];
}

int16_t arp_get_slot_snapped(uint8_t idx)
{
    if (idx >= ARP_MAX_SLOTS || s_arp.slots[idx] < 0) return -1;
    return (int16_t)arp_snap((uint8_t)s_arp.slots[idx]);
}

/* ── Per-target amplitude trim (graph editor amp mode) ──────────────────── */

void arp_set_amp_scale(float v)
{
    v = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
    if (fabsf(s_arp.vp.amp_trim - v) < 0.001f) return;
    s_arp.vp.amp_trim = v;
    arp_mark_dirty();   /* coalesced re-emit on next arp_core_service() */
}

float arp_get_amp_scale(void) { return s_arp.vp.amp_trim; }

/* ── Portamento / glide ──────────────────────────────────────────────────── */

void arp_set_portamento_ms(uint16_t ms)
{
    ms = SEQ_CLAMP_U16(ms, 0, ARP_PORTAMENTO_MAX_MS);
    if (s_arp.portamento_ms == ms) return;
    s_arp.portamento_ms = ms;
    arp_push_portamento();   /* not a scheduling change: push directly, no re-emit */
}

uint16_t arp_get_portamento_ms(void) { return s_arp.portamento_ms; }
