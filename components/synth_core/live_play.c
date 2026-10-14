#include "sdkconfig.h"
#if CONFIG_SYNTH_WIRELESS

#include "live_play.h"
#include "amy_helpers.h"
#include "sequencer_core.h"
#include "seq_core_config.h"  /* SEQ_LFO_SW_MAX_HZ, SEQ_MEL_PATCH */
#include "voice_config.h"   /* voice_params_init_defaults, env bounds, LFO apply */
#include "lfo_bank.h"       /* shared LFO synths (lfo_bank_bind) */
#include "seq_clamp.h"
#include "esp_log.h"
#include <math.h>           /* sinf/powf - software LFO stepper (UI task) */

static const char *TAG = "live_play";

/* LIVE_SYNTH sits between the drum block and the melodic base in the static
 * slot pool (synth_slots.h). 4 voices matches the arp: chords under one hand,
 * with osc headroom even for 7-osc FM voices. */
#include "synth_slots.h"
#define LIVE_VOICES  4

uint8_t live_play_synth_slot(void)
{
    return LIVE_SYNTH;
}

static uint16_t s_patch = SEQ_MEL_PATCH;
static bool     s_ready = false;

/* Browse-group toggle (Wireless page "Source" row), the arp source model's
 * twin: WAVE walks the raw-wave/bass/wavetable block, PATCH everything else.
 * Pure browse state with per-group last-patch memory - configuration stays
 * patch-number driven through the kind dispatch. */
static bool s_wave_mode =
    (SEQ_MEL_PATCH >= SEQ_PATCH_WAVE_BASE && SEQ_MEL_PATCH <= SEQ_PATCH_WAVETABLE_MAX) ||
    (SEQ_MEL_PATCH >= SEQ_PATCH_WAVETABLE_APP_BASE && SEQ_MEL_PATCH <= SEQ_PATCH_WAVETABLE_APP_MAX);
static uint16_t s_patch_mem_wave  = SEQ_PATCH_WAVE_BASE;  /* SINE */
static uint16_t s_patch_mem_patch = 138;                  /* DX7 E.PIANO 1 */

/* Glide time (ms) between note pitches, AMY-native portamento. Same range as
 * the arp / melodic layer Glide (1 ms per encoder detent). */
static uint16_t s_glide_ms = 0;

/* Unison spec (count 0 = never set: live_play_get_unison's defaults).
 * Session-only, like the rest of the voice's state. */
static voice_unison_t s_unison;

/* Runtime-editable voice params, the same block every other engine embeds.
 * Defaults land on first ensure_ready so amp_trim starts at unity, not 0. */
static voice_params_t s_vp;
static bool           s_vp_inited = false;

/* Out-of-the-box editor values, voiced for held keys rather than a running
 * sequence: high sustain, controlled release. Everything stays unauthored
 * until the user commits, except the EG0 shape that live_apply_authored()
 * force-pushes for wave patches, which carry no envelope of their own (the
 * melodic force_wave rule). */
static void live_seed_defaults(void)
{
    voice_params_init_defaults(&s_vp);
    s_vp.env.attack_ms    = 4;    /* tiny ramp kills the note-on click */
    s_vp.env.decay_ms     = 250;
    s_vp.env.sustain_pct  = 75;   /* held keys keep their body */
    s_vp.env.release_ms   = 250;
    s_vp.env.eg_type      = 0;    /* ENVELOPE_NORMAL */
    /* EG1 (filter sweep once a filter is authored): the arp's slower-tail pair. */
    s_vp.env1.attack_ms   = 15;
    s_vp.env1.decay_ms    = 400;
    s_vp.env1.sustain_pct = 20;
    s_vp.env1.release_ms  = 300;
    s_vp.env1.eg_type     = 0;
    /* Filter/LFO: bypass until committed, but seeded so the editors open on
     * something musical instead of zeros. */
    s_vp.filter.filter_type = 0;      /* SEQ_FILTER_NONE */
    s_vp.filter.cutoff_hz   = 800.0f;
    s_vp.filter.resonance   = 1.0f;
    s_vp.lfo.wave    = LFO_WAVE_SINE;
    s_vp.lfo.rate    = NOTE_DIV_1_1;
    s_vp.lfo.depth   = 50;
    s_vp.lfo.targets = LFO_TGT_BIT(LFO_TARGET_FILTER);
}

/* The editors can open before ensure_ready() installs the defaults (a zeroed
 * block is silent: voice_params_init_defaults()). Every accessor goes through
 * this. UI task only, so no guard needed; live_note() deliberately skips it,
 * running on the transport task where ensure_ready() always precedes any
 * note. */
static voice_params_t *live_vp(void)
{
    if (!s_vp_inited) {
        live_seed_defaults();
        s_vp_inited = true;
    }
    return &s_vp;
}

/* Held-note bitmap so all_notes_off releases exactly what is sounding. Written
 * from the transport task, drained from synth_ui / NimBLE host on
 * stop/disconnect; per-bit races are harmless - a spurious note-off to an idle
 * voice is a no-op in AMY. */
static volatile uint32_t s_held[4];

static void live_note(uint8_t note, float velocity)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth     = LIVE_SYNTH;
    e->midi_note = (float)note;
    /* Amp trim rides the note velocity (the graph editor's amp mode). Note-offs
     * pass velocity 0 and must stay 0. */
    e->velocity  = (velocity > 0.0f) ? velocity * s_vp.amp_trim : 0.0f;
    amy_helpers_event_send(e);
}

/* Effective unison copies the slot is built with: the stored count through
 * the shared clamp (a never-set count of 0 gives 1). */
static uint8_t live_unison_copies(void)
{
    return sequencer_core_unison_copies_for(s_patch, s_unison.count, LIVE_VOICES);
}

/* The stored spec carrying the effective count and the global backend. */
static voice_unison_t live_unison_eff(void)
{
    voice_unison_t eff;
    live_play_get_unison(&eff);
    eff.count = live_unison_copies();
    return eff;
}

/* Osc layout of one live voice as built (sequencer_core_voice_layout); false
 * for patches that are not wave-topology. */
static bool live_voice_layout(seq_voice_layout_t *vl)
{
    return sequencer_core_voice_layout(s_patch, live_unison_copies(), vl);
}

/* One dist block where the voice carries the stage: each head under unison,
 * else the base osc (mirrors melodic_dist_apply). */
static void live_push_dist(const seq_dist_t *d)
{
    seq_voice_layout_t vl;
    uint8_t heads = live_voice_layout(&vl) ? vl.heads_mask : 0u;
    if (!heads) {
        voice_apply_dist(LIVE_SYNTH, d);
        return;
    }
    for (uint8_t o = 0; (uint8_t)(heads >> o) != 0u; o++)
        if (heads & (uint8_t)(1u << o))
            voice_apply_dist_osc(LIVE_SYNTH, o, d);
}

/* Push the stored filter plus its envelope routing matrix (eg_depth, in its
 * own event, all eight slots once the filter block is authored). Mirrors
 * arp_apply_filter(), including its KS branch for the raw KS patch. EG1
 * breakpoints ride along whenever an EG1 rail is live
 * (sequencer_core_push_envelope_eg1()). */
static void live_apply_filter(const seq_filter_t *f)
{
    if (!f) return;
    amy_event *e = amy_helpers_event_begin();
    e->synth = LIVE_SYNTH;
    if (f->enabled) {
        e->filter_type = f->filter_type;
        e->filter_freq_coefs[COEF_CONST] = sequencer_core_filter_const_hz(f);
        e->resonance = f->resonance;
    } else {
        e->filter_type = FILTER_NONE;
    }
    /* KS string decay from the authored feedback field; 0 = never authored,
     * keep what the voice configure sent (VOICE_KS_FEEDBACK_DEFAULT,
     * voice_config.h). Pluck duty is written on every KS push: its zero
     * offset IS the 0.5 default, so no authored gate. */
    if (s_patch == SEQ_PATCH_KS) {
        if (f->feedback > 0.0f) {
            e->feedback = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
        }
        e->duty_coefs[COEF_CONST] = 0.5f + f->ks_duty_ofs;
    }
    amy_helpers_event_send(e);

    sequencer_core_push_eg_depths(LIVE_SYNTH, sequencer_core_patch_voice_osc(s_patch), f, s_vp.filter_authored);
    if (sequencer_core_is_wavetable_patch(s_patch)) {
        seq_voice_layout_t vl;
        uint8_t mask = live_voice_layout(&vl) ? vl.pitch_mask : 0x01u;
        sequencer_core_push_eg_scan(LIVE_SYNTH, mask, f, s_vp.filter_authored);
    }

    if (seq_filter_eg1_live(f)) {
        sequencer_core_push_envelope_eg1(LIVE_SYNTH, 0, &s_vp.env1);
    }
}

/* Push the glide time straight to the live synth (fans out to every voice's
 * base osc, same dispatch as the arp's portamento push). */
static void live_push_glide(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth         = LIVE_SYNTH;
    e->portamento_ms = s_glide_ms;
    amy_helpers_event_send(e);
}

#if CONFIG_SEQ_LFO_SHARED
/* The shared LFO synth LIVE_SYNTH holds (lfo_bank.h); LFO_BANK_NONE while the
 * software stepper serves it, or nothing does. s_live_lfo2_ref is LFO2's (mod
 * slot 0), which has no stepper. */
static uint8_t s_live_lfo_ref = LFO_BANK_NONE;
static uint8_t s_live_lfo2_ref = LFO_BANK_NONE;

/* The oscs the voice's shared LFO synths reach: the wave-topology coupled
 * oscs (SCAN rides the pitch mask, as on a row) or a patch string's
 * lfo_targets map. False for an unmapped patch. */
static bool live_lfo_target_map(lfo_target_map_t *map)
{
    seq_voice_layout_t vl;
    if (live_voice_layout(&vl)) {
        *map = (lfo_target_map_t){ .voice_mask = vl.voice_mask,
                                   .pitch_mask = vl.pitch_mask,
                                   .scan_mask  = vl.pitch_mask };
        return true;
    }
    return lfo_targets_of_patch(s_patch, map);
}
#endif

/* With CONFIG_SEQ_LFO_SHARED: a shared LFO synth over the wave-topology
 * coupled oscs or a patch string's lfo_targets map; when the bank is full,
 * the LFO is off or the patch has no map, the software stepper below serves
 * it. Without it: apply or park the native LFO where
 * sequencer_core_lfo_native_layout() says so; other patches get the software
 * stepper. Parking a never-authored voice is a coupled-osc-only clear - the
 * carrier pair stays unmaterialized (voice_config.h). */
static void live_apply_lfo(void)
{
    bool on = s_vp.lfo_authored && s_vp.lfo.enabled;
    uint16_t bpm = sequencer_core_get_bpm();
#if CONFIG_SEQ_LFO_SHARED
    lfo_target_map_t map;
    if (live_lfo_target_map(&map))
        (void)lfo_bank_bind(LIVE_SYNTH, VOICE_LFO_SLOT_LFO1, &map,
                            on ? &s_vp.lfo : NULL, bpm, &s_live_lfo_ref);
    else
        lfo_bank_unbind(&s_live_lfo_ref);   /* the patch re-apply unwired it */
#else
    seq_voice_layout_t vl;
    if (!sequencer_core_lfo_native_layout(s_patch, NULL, NULL) || !live_voice_layout(&vl))
        return;
    voice_apply_native_lfo_topo(LIVE_SYNTH, on ? &s_vp.lfo : NULL,
                                bpm, vl.carrier, vl.pitch_mask, vl.voice_mask);
#endif
}

/* Send `lfo` (NULL = unwire) to LIVE_SYNTH's mod slot 0 where the patch has a
 * target map; drop LFO2's reference otherwise. */
static void live_wire_lfo2(const seq_lfo_t *lfo)
{
#if CONFIG_SEQ_LFO_SHARED
    lfo_target_map_t map;
    if (live_lfo_target_map(&map))
        (void)lfo_bank_bind(LIVE_SYNTH, VOICE_LFO_SLOT_LFO2, &map, lfo,
                            sequencer_core_get_bpm(), &s_live_lfo2_ref);
    else
        lfo_bank_unbind(&s_live_lfo2_ref);
#else
    (void)lfo;
#endif
}

/* LFO2 by the slot-0 rule (ENGINE-SEMANTICS.md, "LFO: native carrier vs
 * software stepper"): open, wired or unwired with the stored LFO2; collapsed,
 * no send, so a Juno/DX7 patch keeps its own LFO. */
static void live_apply_lfo2(void)
{
    if (s_vp.lfo2_authored) {
        live_wire_lfo2(&s_vp.lfo2);
        return;
    }
#if CONFIG_SEQ_LFO_SHARED
    lfo_bank_unbind(&s_live_lfo2_ref);
#endif
}

/* Re-push whatever the user has authored. Called after every slot configure,
 * since a patch load rebuilds the voice and drops our overrides. */
static void live_apply_authored(void)
{
    bool wave = sequencer_core_is_wave_patch(s_patch);
    /* Wave patches carry no envelope of their own, so force the default shape
     * even unauthored; patch strings keep theirs until the user commits. */
    if (s_vp.env_authored || wave)
        sequencer_core_push_envelope_osc(LIVE_SYNTH, sequencer_core_patch_voice_osc(s_patch), &s_vp.env);
    if (s_vp.env1_authored)   sequencer_core_push_envelope_eg1(LIVE_SYNTH, 0, &s_vp.env1);
    if (s_vp.filter_authored) live_apply_filter(&s_vp.filter);
    if (s_vp.dist_authored)   live_push_dist(&s_vp.dist);
    live_apply_lfo();   /* internal layout guard: patch strings bank or stepper */
    live_apply_lfo2();  /* after LFO1, as at every rebuild site */
    /* Patch loads reset per-osc portamento_alpha; reassert unconditionally
     * (0 is a valid "off" reassert) - the arp_rebuild discipline. */
    live_push_glide();
}

/* Configure the slot for s_patch with the effective unison, then re-push the
 * authored state. The reconfigure kills sounding voices (osc topology may
 * change), so the held bitmap is cleared and stale note-offs are not replayed
 * later. */
static void live_rebuild(void)
{
    for (uint8_t w = 0; w < 4; w++) s_held[w] = 0;
    voice_unison_t eff = live_unison_eff();
    sequencer_core_configure_synth_slot(LIVE_SYNTH, s_patch, LIVE_VOICES,
                                        (eff.count > 1u) ? &eff : NULL);
    live_apply_authored();
}

bool live_play_is_ready(void)
{
    return s_ready;
}

void live_play_ensure_ready(void)
{
    if (s_ready) return;
    if (!s_vp_inited) {
        live_seed_defaults();
        s_vp_inited = true;
    }
    live_rebuild();
    s_ready = true;
    ESP_LOGI(TAG, "live slot %u ready (patch %u, %u voices)",
             (unsigned)LIVE_SYNTH, (unsigned)s_patch, (unsigned)LIVE_VOICES);
}

void live_play_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    (void)channel;
    if (!s_ready || note > 127) return;
    s_held[note >> 5] |= 1u << (note & 31u);
    live_note(note, (float)velocity * (1.0f / 127.0f));
}

void live_play_note_off(uint8_t channel, uint8_t note)
{
    (void)channel;
    if (!s_ready || note > 127) return;
    s_held[note >> 5] &= ~(1u << (note & 31u));
    live_note(note, 0.0f);
}

void live_play_all_notes_off(void)
{
    if (!s_ready) return;
    for (uint8_t w = 0; w < 4; w++) {
        uint32_t bits = s_held[w];
        s_held[w] = 0;
        while (bits) {
            uint8_t bit = (uint8_t)__builtin_ctz(bits);
            bits &= bits - 1u;
            live_note((uint8_t)(w * 32u + bit), 0.0f);
        }
    }
}

uint16_t live_play_get_patch(void)
{
    return s_patch;
}

void live_play_set_patch(uint16_t patch_number)
{
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_FULL_MAX);
    if (s_patch == patch_number) return;
    s_patch = patch_number;
    if (s_ready) live_rebuild();
}

void live_play_get_unison(voice_unison_t *out)
{
    if (!out) return;
    /* First-turn defaults, as on a row; count 1 keeps them inert. */
    voice_unison_t u = { 1u, 12u, 50u, 100u, 0u };
    if (s_unison.count != 0u) u = s_unison;
    u.layout = sequencer_core_get_unison_layout();
    *out = u;
}

uint8_t live_play_unison_max(void)
{
    return sequencer_core_unison_max_for(s_patch, LIVE_VOICES);
}

void live_play_set_unison(const voice_unison_t *u)
{
    if (!u) return;
    uint8_t was = live_unison_copies();
    voice_unison_t v = *u;
    v.count        = SEQ_CLAMP_U8(v.count, 1u, VOICE_UNISON_MAX_COPIES);
    v.detune_cents = SEQ_CLAMP_U8(v.detune_cents, 0u, VOICE_UNISON_MAX_DETUNE);
    v.spread_pct   = SEQ_CLAMP_U8(v.spread_pct, 0u, 100u);
    v.blend_pct    = SEQ_CLAMP_U8(v.blend_pct, 0u, 100u);
    v.layout       = 0u;   /* storage ignores it: the backend is global */
    s_unison = v;
    if (!s_ready) return;   /* live_play_ensure_ready() builds it */
    uint8_t now = live_unison_copies();
    if (now != was) {
        live_rebuild();
    } else if (now > 1u) {
        voice_unison_t eff = live_unison_eff();
        voice_push_unison_live(LIVE_SYNTH, &eff, 1.0f);
    }
}

bool live_play_get_wave_mode(void)
{
    return s_wave_mode;
}

void live_play_set_wave_mode(bool wave)
{
    if (wave == s_wave_mode) return;
    uint16_t restore;
    if (wave) { s_patch_mem_patch = s_patch; restore = s_patch_mem_wave;  }
    else      { s_patch_mem_wave  = s_patch; restore = s_patch_mem_patch; }
    s_wave_mode = wave;
    live_play_set_patch(restore);
}

uint16_t live_play_get_glide_ms(void)
{
    return s_glide_ms;
}

void live_play_set_glide_ms(uint16_t ms)
{
    ms = SEQ_CLAMP_U16(ms, 0, LIVE_PLAY_GLIDE_MAX_MS);
    if (s_glide_ms == ms) return;
    s_glide_ms = ms;
    if (s_ready) live_push_glide();
}

bool live_play_lfo_native_eligible(void)
{
#if CONFIG_SEQ_LFO_SHARED
    /* Only a shared LFO synth carries the wobble; the stepper has none. */
    return s_live_lfo_ref != LFO_BANK_NONE;
#else
    return sequencer_core_lfo_native_layout(s_patch, NULL, NULL);
#endif
}

/* Retune the BPM-synced carrier pair after a tempo change. Full re-apply
 * rather than the arp's targeted freq pushes: menu-rate, and it keeps the
 * depth/wobble coupling in one authoring path. The software stepper needs
 * nothing here - it reads the BPM every frame. A no-op with
 * CONFIG_SEQ_LFO_SHARED: no per-voice carriers, and the shared synth is
 * retuned by lfo_bank_set_bpm(). */
void live_play_refresh_lfo_freq(void)
{
#if !CONFIG_SEQ_LFO_SHARED
    if (!s_ready || !s_vp.lfo_authored || !s_vp.lfo.enabled) return;
    live_apply_lfo();
#endif
}

void live_play_refresh_key_track(void)
{
    const seq_filter_t *f = &s_vp.filter;
    if (s_ready && s_vp.filter_authored && f->enabled && f->key_track != 0.0f)
        live_apply_filter(f);
}

/* ── PATCH-mode software LFO fallback ────────────────────────────────────
 * The 20 Hz stepper for voices without a shared LFO synth and, without
 * CONFIG_SEQ_LFO_SHARED, without a native layout (live_apply_lfo serves the
 * rest); mirrors arp_swlfo_service. Split and target coverage:
 * ENGINE-SEMANTICS.md, "LFO: native carrier vs software stepper". */
static float   s_swlfo_phase   = 0.0f;
static float   s_swlfo_rnd     = 0.0f;
static bool    s_swlfo_active  = false;
static uint8_t s_swlfo_targets = 0;   /* rails driven while active - the set
                                       * to restore, even if edited since */

/* seq_core_editors.c internals shared with this stepper. Mirrored prototypes:
 * seq_core_internal.h cannot be included here (it defines a TU-local TAG). */
float lfo_next_rand(void);
void  lfo_push_amp(uint8_t synth_id, uint16_t patch, float amp);
void  lfo_push_target_neutral(uint8_t synth_id, uint16_t patch,
                              lfo_target_t target);

static inline float live_swlfo_hz(note_div_t rate, uint16_t bpm)
{
    float hz = lfo_rate_to_hz(rate, bpm);
    return (hz > SEQ_LFO_SW_MAX_HZ) ? SEQ_LFO_SW_MAX_HZ : hz;
}

static float live_swlfo_eval(lfo_wave_t wave, float ph)
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

void live_play_lfo_service(void)
{
    const seq_lfo_t *lfo = &s_vp.lfo;
#if CONFIG_SEQ_LFO_SHARED
    /* A shared LFO synth serves the voice: stand down (restoring below). With
     * no per-voice carriers, every unbound patch, wave or string, is ours. */
    const bool native = false;
    bool want = s_ready && s_vp.lfo_authored && lfo->enabled &&
                lfo->targets != 0 && s_live_lfo_ref == LFO_BANK_NONE;
#else
    /* The stepper is fully off on native-eligible patches (mirrors the
     * arp/sequencer steppers). */
    bool native = live_play_lfo_native_eligible();
    bool want = s_ready && s_vp.lfo_authored && lfo->enabled &&
                lfo->targets != 0 && !native;
#endif

    if (!want) {
        if (s_swlfo_active) {
            /* Restore every rail the stepper drove (melodic
             * lfo_restore_target_neutrals rule: FILTER re-pushes the stored
             * filter, the rest a neutral constant). */
            s_swlfo_active = false;
            for (int t = 0; t < LFO_TARGET_COUNT; t++) {
                if (!(s_swlfo_targets & LFO_TGT_BIT(t))) continue;
                if (t == LFO_TARGET_FILTER)
                    live_apply_filter(&s_vp.filter);
                else if (t == LFO_TARGET_DIST_DRIVE || t == LFO_TARGET_DIST_MIX) {
                    /* One restore event covers both dist bits. */
                    if (t == LFO_TARGET_DIST_DRIVE ||
                        !(s_swlfo_targets & LFO_TGT_BIT(LFO_TARGET_DIST_DRIVE)))
                        voice_apply_dist(LIVE_SYNTH, &s_vp.dist);
                }
                else
                    lfo_push_target_neutral(LIVE_SYNTH, s_patch, (lfo_target_t)t);
            }
        }
        return;
    }

    if (!s_swlfo_active) {
        s_swlfo_active = true;
        s_swlfo_phase  = 0.0f;
    }
    /* want requires !native, so the stepper only runs on PATCH-mode voices -
     * record the full target set for the deactivation restore. */
    s_swlfo_targets = lfo->targets;

    float ph = s_swlfo_phase +
               live_swlfo_hz(lfo->rate, sequencer_core_get_bpm()) * 0.05f;
    if (ph >= 1.0f) {
        ph -= 1.0f;
        if (lfo->wave == LFO_WAVE_RANDOM) s_swlfo_rnd = lfo_next_rand();
    }
    s_swlfo_phase = ph;

    float val = live_swlfo_eval(lfo->wave, ph);
    float d   = (float)lfo->depth / 100.0f;

    if (!native) {
    amy_event *e = amy_helpers_event_begin();
    e->synth = LIVE_SYNTH;
    if (LFO_HAS_TGT(lfo, LFO_TARGET_FILTER)) {
        float base = (s_vp.filter.enabled && s_vp.filter.cutoff_hz > 0.0f)
                     ? sequencer_core_filter_const_hz(&s_vp.filter) : 1000.0f;
        e->filter_freq_coefs[COEF_CONST] =
            base * powf(2.0f, voice_lfo_filter_octaves(lfo) * val);
    }
    if (LFO_HAS_TGT(lfo, LFO_TARGET_PAN))
        e->pan_coefs[COEF_CONST] = 0.5f + d * 0.5f * val;
    /* SCAN needs a wavetable voice - wave patches / native only. */
    amy_helpers_event_send(e);
    if (LFO_HAS_TGT(lfo, LFO_TARGET_AMP))
        lfo_push_amp(LIVE_SYNTH, s_patch, 1.0f - d * (0.5f - 0.5f * val));
    } /* !native */

    /* DIST on a PATCH-mode voice: step around the committed dist block
     * (voice_push_dist_lfo). */
    if (!native && (lfo->targets & LFO_TGT_DIST_MASK))
        voice_push_dist_lfo(LIVE_SYNTH, &s_vp.dist, lfo, val);

    if (!native && LFO_HAS_TGT(lfo, LFO_TARGET_PITCH)) {
        /* Absolute Hz anchored on SEQ_LFO_PITCH_BASE_HZ, sent to osc 0 only
         * (ENGINE-SEMANTICS.md, software stepper). */
        amy_event *pe = amy_helpers_event_begin();
        pe->synth = LIVE_SYNTH;
        pe->osc   = 0;
        pe->freq_coefs[COEF_CONST] = SEQ_LFO_PITCH_BASE_HZ * powf(2.0f, d * VOICE_LFO_DEPTH_PITCH * val);
        amy_helpers_event_send(pe);
    }
}

/* ── Runtime-editable voice params (shared editors) ──────────────────────
 * Structural twin of the arp block (arp_core.c): one voice, no layer/track
 * scope, authored flags gating re-application after a patch change. */

void live_play_get_envelope(seq_env_t *out)
{
    if (out) *out = live_vp()->env;
}

void live_play_set_envelope(const seq_env_t *env)
{
    if (!env) return;
    voice_params_t *vp = live_vp();
    vp->env = *env;
    vp->env.attack_ms  = SEQ_CLAMP_U32(vp->env.attack_ms,
                                       VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    vp->env.release_ms = SEQ_CLAMP_U32(vp->env.release_ms,
                                       VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    vp->env_authored = true;
    if (s_ready) sequencer_core_push_envelope_osc(LIVE_SYNTH, sequencer_core_patch_voice_osc(s_patch), &vp->env);
}

void live_play_get_envelope2(seq_env_t *out)
{
    if (out) *out = live_vp()->env1;
}

void live_play_set_envelope2(const seq_env_t *env)
{
    if (!env) return;
    voice_params_t *vp = live_vp();
    vp->env1 = *env;
    vp->env1.attack_ms  = SEQ_CLAMP_U32(vp->env1.attack_ms,
                                        VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    vp->env1.release_ms = SEQ_CLAMP_U32(vp->env1.release_ms,
                                        VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    vp->env1_authored = true;
    if (s_ready) sequencer_core_push_envelope_eg1(LIVE_SYNTH, 0, &vp->env1);
}

void live_play_get_filter(seq_filter_t *out)
{
    if (out) *out = live_vp()->filter;
}

void live_play_set_filter(const seq_filter_t *f)
{
    if (!f) return;
    voice_params_t *vp = live_vp();
    vp->filter = *f;
    vp->filter_authored = true;
    if (s_ready) live_apply_filter(&vp->filter);
}

void live_play_get_dist(seq_dist_t *out)
{
    if (out) *out = live_vp()->dist;
}

void live_play_set_dist(const seq_dist_t *d)
{
    if (!d) return;
    voice_params_t *vp = live_vp();
    vp->dist = *d;
    voice_dist_clamp(&vp->dist);
    vp->dist_authored = true;
    if (s_ready) live_push_dist(&vp->dist);
}

void live_play_preview_dist(const seq_dist_t *d)
{
    if (d && s_ready) live_push_dist(d);
}

/* Re-assert after a voice rebuild (patch change) clears the per-osc stage. */
void live_play_reapply_dist(void)
{
    voice_params_t *vp = live_vp();
    if (vp->dist_authored && s_ready) live_push_dist(&vp->dist);
}

void live_play_get_lfo(seq_lfo_t *out)
{
    if (out) *out = live_vp()->lfo;
}

/* A shared LFO synth or the native carriers take the edit now
 * (live_apply_lfo); the software stepper picks it up on its next frame. */
void live_play_set_lfo(const seq_lfo_t *lfo)
{
    if (!lfo) return;
    voice_params_t *vp = live_vp();
    vp->lfo = *lfo;
    vp->lfo_authored = true;
    /* One LFO per target (seq_model.h): LFO1 keeps a shared one. */
    uint8_t shared = vp->lfo2.targets & vp->lfo.targets;
    vp->lfo2.targets &= (uint8_t)~shared;
    if (!s_ready) return;
    if (shared) live_apply_lfo2();
    live_apply_lfo();
}

bool live_play_get_lfo2(seq_lfo_t *out)
{
    voice_params_t *vp = live_vp();
    if (out) *out = vp->lfo2;
    return vp->lfo2_authored;
}

void live_play_set_lfo2(const seq_lfo_t *lfo, bool open)
{
    if (!lfo) return;
    voice_params_t *vp = live_vp();
    bool collapse = vp->lfo2_authored && !open;
    vp->lfo2 = *lfo;
    vp->lfo2.targets &= (uint8_t)~vp->lfo.targets;   /* one LFO per target */
    if (!s_ready) {   /* live_play_ensure_ready() binds it */
        vp->lfo2_authored = open;
        return;
    }
    if (collapse && lfo_targets_patch_owns_slot0(s_patch)) {
        /* Only a patch re-apply brings the patch's own LFO back. */
        vp->lfo2_authored = false;
        live_rebuild();
        return;
    }
    if (collapse) live_wire_lfo2(NULL);   /* plain unwire: the patch leaves slot 0 free */
    vp->lfo2_authored = open;
    live_apply_lfo2();
}

bool live_play_lfo2_bound(void)
{
#if CONFIG_SEQ_LFO_SHARED
    return s_live_lfo2_ref != LFO_BANK_NONE;
#else
    return false;
#endif
}

float live_play_get_amp_scale(void)
{
    return live_vp()->amp_trim;
}

void live_play_set_amp_scale(float v)
{
    live_vp()->amp_trim = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
}

/* ── Editor live-preview (AMY only; store + authored flags untouched) ────── */

void live_play_preview_envelope(const seq_env_t *env)
{
    if (env && s_ready) sequencer_core_push_envelope_osc(LIVE_SYNTH, sequencer_core_patch_voice_osc(s_patch), env);
}

void live_play_preview_envelope2(const seq_env_t *env)
{
    if (env && s_ready) sequencer_core_push_envelope_eg1(LIVE_SYNTH, 0, env);
}

void live_play_preview_filter(const seq_filter_t *f)
{
    if (s_ready) live_apply_filter(f);
}

#endif /* CONFIG_SYNTH_WIRELESS */
