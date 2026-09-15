#include "sdkconfig.h"
#if CONFIG_SYNTH_WIRELESS

#include "live_play.h"
#include "amy_helpers.h"
#include "sequencer_core.h"
#include "seq_core_config.h"  /* SEQ_LFO_SW_MAX_HZ, SEQ_MEL_PATCH */
#include "voice_config.h"   /* voice_params_init_defaults, env bounds, LFO apply */
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
 * the arp / melodic NoteFX Glide (1 ms per encoder detent). */
static uint16_t s_glide_ms = 0;

/* Runtime-editable voice params, the same block every other engine embeds.
 * Defaults land on first ensure_ready so amp_trim starts at unity, not 0. */
static voice_params_t s_vp;
static bool           s_vp_inited = false;

/* Out-of-the-box editor values, voiced for held keys rather than a running
 * sequence: high sustain, controlled release. Everything stays unauthored
 * until the user commits, except the EG0 shape that live_apply_authored()
 * force-pushes for wave patches - a raw wave with no envelope reads AMY's
 * empty breakpoint set as a permanently open gate and rings forever after
 * note-off (the melodic force_wave rule). */
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
    s_vp.lfo.rate    = LFO_RATE_1BAR;
    s_vp.lfo.depth   = 50;
    s_vp.lfo.targets = LFO_TGT_BIT(LFO_TARGET_FILTER);
}

/* The editors can open before ensure_ready() installs the defaults, and a
 * zero-initialised block has amp_trim 0.0 - which the graph editor reads as 0%
 * and commits as silence. Every accessor goes through this. UI task only, so
 * no guard needed; live_note() deliberately skips it, running on the transport
 * task where ensure_ready() always precedes any note. */
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

/* Push the stored filter including the bipolar EG1->cutoff sweep depth.
 * Mirrors arp_apply_filter() minus its KS branch (the live slot is always a
 * patch voice). EG1 breakpoints go out alongside a nonzero sweep so
 * filter_freq_coefs[COEF_EG1] never reads AMY's never-configured
 * always-open unity gate. */
static void live_apply_filter(const seq_filter_t *f)
{
    if (!f) return;
    amy_event *e = amy_helpers_event_begin();
    e->synth = LIVE_SYNTH;
    if (f->enabled) {
        e->filter_type = f->filter_type;
        e->filter_freq_coefs[COEF_CONST] = f->cutoff_hz;
        e->resonance = f->resonance;
        if (f->filter_env_amount != 0.0f) {
            e->filter_freq_coefs[COEF_EG1] = f->filter_env_amount;
        }
    } else {
        e->filter_type = FILTER_NONE;
    }
    amy_helpers_event_send(e);

    if (f->enabled && f->filter_env_amount != 0.0f) {
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

/* Apply or park the native LFO on any patch with a reserved carrier pair
 * (sequencer_core_lfo_native_layout); patch strings own their whole osc layout
 * and get the software stepper below instead. Parking a never-authored voice
 * is a coupled-osc-only clear - the carrier pair stays unmaterialized
 * (voice_config.h). */
static void live_apply_lfo(void)
{
    uint8_t carrier, coupled;
    if (!sequencer_core_lfo_native_layout(s_patch, &carrier, &coupled)) return;
    bool on = s_vp.lfo_authored && s_vp.lfo.enabled;
    voice_apply_native_lfo_topo(LIVE_SYNTH, on ? &s_vp.lfo : NULL,
                                sequencer_core_get_bpm(), carrier,
                                coupled, coupled);
}

/* Re-push whatever the user has authored. Called after every slot configure,
 * since a patch load rebuilds the voice and drops our overrides. */
static void live_apply_authored(void)
{
    bool wave = sequencer_core_is_wave_patch(s_patch);
    /* Wave patches carry no envelope of their own: with nothing pushed a
     * note-off never releases, so force the default shape even unauthored.
     * Patch strings keep their built-in envelope until the user commits. */
    if (s_vp.env_authored || wave)
        sequencer_core_push_envelope(LIVE_SYNTH, &s_vp.env);
    if (s_vp.env1_authored)   sequencer_core_push_envelope_eg1(LIVE_SYNTH, 0, &s_vp.env1);
    if (s_vp.filter_authored) live_apply_filter(&s_vp.filter);
    if (s_vp.dist_authored)   voice_apply_dist(LIVE_SYNTH, &s_vp.dist);
    live_apply_lfo();   /* internal layout guard: no-op for patch strings */
    /* Patch loads reset per-osc portamento_alpha; reassert unconditionally
     * (0 is a valid "off" reassert) - the arp_rebuild discipline. */
    live_push_glide();
}

void live_play_ensure_ready(void)
{
    if (s_ready) return;
    if (!s_vp_inited) {
        live_seed_defaults();
        s_vp_inited = true;
    }
    sequencer_core_configure_synth_slot(LIVE_SYNTH, s_patch, LIVE_VOICES);
    live_apply_authored();
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
    if (s_ready) {
        /* Reconfigure kills sounding voices (osc topology may change);
         * clear the bitmap so stale note-offs are not replayed later. */
        for (uint8_t w = 0; w < 4; w++) s_held[w] = 0;
        sequencer_core_configure_synth_slot(LIVE_SYNTH, s_patch, LIVE_VOICES);
        live_apply_authored();
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
    return sequencer_core_lfo_native_layout(s_patch, NULL, NULL);
}

/* Retune the BPM-synced carrier pair after a tempo change. Full re-apply
 * rather than the arp's targeted freq pushes: menu-rate, and it keeps the
 * depth/wobble coupling in one authoring path. The software stepper needs
 * nothing here - it reads the BPM every frame. */
void live_play_refresh_lfo_freq(void)
{
    if (!s_ready || !s_vp.lfo_authored || !s_vp.lfo.enabled) return;
    live_apply_lfo();
}

/* ── PATCH-mode software LFO fallback ────────────────────────────────────
 * Wave patches get the AMY-native voice-local LFO (live_apply_lfo). A patch
 * string owns its whole osc layout, so it runs the same 20 Hz software stepper
 * as non-wave melodic tracks and the arp's PATCH source (mirrors
 * arp_swlfo_service), modulating each checked target's COEF_CONST rail.
 * WOBBLE and SCAN have no software analog and are ignored. */
static float   s_swlfo_phase   = 0.0f;
static float   s_swlfo_rnd     = 0.0f;
static bool    s_swlfo_active  = false;
static uint8_t s_swlfo_targets = 0;   /* rails driven while active - the set
                                       * to restore, even if edited since */

/* seq_core_editors.c internals shared with this stepper. Mirrored prototypes:
 * seq_core_internal.h cannot be included here (it defines a TU-local TAG). */
float lfo_next_rand(void);
void  lfo_push_target_neutral(uint8_t synth_id, lfo_target_t target);

static inline float live_swlfo_hz(lfo_rate_t rate, uint16_t bpm)
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
    /* DIST has COEF_MOD rails now (dist_*_coefs), so on native-eligible patches
     * it rides the carrier like every other target and the stepper is fully off
     * there (mirrors the arp/sequencer steppers). */
    bool native = live_play_lfo_native_eligible();
    bool want = s_ready && s_vp.lfo_authored && lfo->enabled &&
                lfo->targets != 0 && !native;

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
                    lfo_push_target_neutral(LIVE_SYNTH, (lfo_target_t)t);
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
                     ? s_vp.filter.cutoff_hz : 1000.0f;
        e->filter_freq_coefs[COEF_CONST] =
            base * powf(2.0f, voice_lfo_filter_octaves(lfo) * val);
    }
    if (LFO_HAS_TGT(lfo, LFO_TARGET_AMP))
        e->amp_coefs[COEF_CONST] = 1.0f - d * (0.5f - 0.5f * val);
    if (LFO_HAS_TGT(lfo, LFO_TARGET_PAN))
        e->pan_coefs[COEF_CONST] = 0.5f + d * 0.5f * val;
    /* SCAN needs a wavetable voice - wave patches / native only. */
    amy_helpers_event_send(e);
    } /* !native */

    /* DIST targets on a PATCH-mode voice (no carrier): step around the committed
     * dist block, same law as the native rail (voice_push_dist_lfo). Native
     * voices drive it via COEF_MOD, so !native excludes them like the rest. */
    if (!native && (lfo->targets & LFO_TGT_DIST_MASK))
        voice_push_dist_lfo(LIVE_SYNTH, &s_vp.dist, lfo, val);

    if (!native && LFO_HAS_TGT(lfo, LFO_TARGET_PITCH)) {
        /* Absolute Hz: see SEQ_LFO_PITCH_BASE_HZ - a bare ratio here lands
         * ~8.8 octaves down. Osc 0 only: a synth-wide push would rewrite
         * patch-internal modulator oscs' rates (stopgap - revisit, see the
         * sequencer stepper). */
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
    if (s_ready) sequencer_core_push_envelope(LIVE_SYNTH, &vp->env);
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
    if (s_ready) voice_apply_dist(LIVE_SYNTH, &vp->dist);
}

void live_play_preview_dist(const seq_dist_t *d)
{
    if (d && s_ready) voice_apply_dist(LIVE_SYNTH, d);
}

/* Re-assert after a voice rebuild (patch change) clears the per-osc stage. */
void live_play_reapply_dist(void)
{
    voice_params_t *vp = live_vp();
    if (vp->dist_authored && s_ready) voice_apply_dist(LIVE_SYNTH, &vp->dist);
}

void live_play_get_lfo(seq_lfo_t *out)
{
    if (out) *out = live_vp()->lfo;
}

/* Native for wave patches (reserved carrier pair); patch strings get picked up
 * by live_play_lfo_service on its next frame - the melodic/arp split. */
void live_play_set_lfo(const seq_lfo_t *lfo)
{
    if (!lfo) return;
    voice_params_t *vp = live_vp();
    vp->lfo = *lfo;
    vp->lfo_authored = true;
    if (s_ready) live_apply_lfo();
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
    if (env && s_ready) sequencer_core_push_envelope(LIVE_SYNTH, env);
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
