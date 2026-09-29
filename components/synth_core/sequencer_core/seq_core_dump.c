/* ── Sequencer state dump (DEV menu one-shot) ──
 * Prints every layer's track config and step exceptions to the console, so
 * runtime-tuned values (drum pitches/presets above all) can be read back in one
 * block. Scoped to sequencer layer/track/step state; FX, arp and drone have
 * their own screens.
 *
 * Plain printf, BEGIN/END markers with the firmware version and a printed-line
 * count (a truncated copy is detectable). Step state and voice params print
 * lazily: one line per step only where it differs from the plain-step neutral
 * values, eg1/flt/dist only when authored, trim only when off unity; eg0
 * always, since raw-wave rows get it pushed regardless. */

#include "seq_core_internal.h"
#include "sequencer_core.h"
#include "esp_app_desc.h"
#include <stdio.h>

/* One printed line per call site, counted for the END marker. */
#define DP(...) do { printf(__VA_ARGS__); printf("\n"); n_lines++; } while (0)

static const char *const PCM_MODE_NAMES[] =
    { "DFLT", "PLAY", "LOOP", "LOOPST", "FRVR" };
static const char *const XFORM_NAMES[] =
    { "NONE", "RAND", "RAMPUP", "RAMPDN" };

static const char *pcm_mode_name(uint8_t m)
{
    return (m < sizeof PCM_MODE_NAMES / sizeof *PCM_MODE_NAMES)
               ? PCM_MODE_NAMES[m] : "?";
}

static const char *xform_name(uint8_t x)
{
    return (x < sizeof XFORM_NAMES / sizeof *XFORM_NAMES)
               ? XFORM_NAMES[x] : "?";
}

/* One voice block (a row's own, or the layer's shared one). The eg0 line
 * prints authored or not: a raw-wave row gets its stored env pushed either
 * way (sequencer_configure_melodic_envelope). Returns the lines printed. */
static unsigned dump_vp(const char *tag, const voice_params_t *vp)
{
    unsigned n_lines = 0;
    DP("  %s eg0 a=%lu d=%lu s=%u r=%lu type=%u auth=%u",
       tag, (unsigned long)vp->env.attack_ms,
       (unsigned long)vp->env.decay_ms, vp->env.sustain_pct,
       (unsigned long)vp->env.release_ms, vp->env.eg_type,
       (unsigned)vp->env_authored);
    if (vp->env1_authored) {
        DP("  %s eg1 a=%lu d=%lu s=%u r=%lu type=%u",
           tag, (unsigned long)vp->env1.attack_ms,
           (unsigned long)vp->env1.decay_ms, vp->env1.sustain_pct,
           (unsigned long)vp->env1.release_ms, vp->env1.eg_type);
    }
    if (vp->filter_authored) {
        DP("  %s flt type=%u en=%u cut=%.0f res=%.2f fb=%.2f dto=%.2f "
           "eg0=%.2f/%.2f/%.2f/%.2f eg1=%.2f/%.2f/%.2f/%.2f",
           tag, vp->filter.filter_type,
           (unsigned)vp->filter.enabled, (double)vp->filter.cutoff_hz,
           (double)vp->filter.resonance,
           (double)vp->filter.feedback,
           (double)vp->filter.ks_duty_ofs,
           (double)vp->filter.eg_depth[0][SEQ_EGT_PITCH],
           (double)vp->filter.eg_depth[0][SEQ_EGT_CUTOFF],
           (double)vp->filter.eg_depth[0][SEQ_EGT_DRIVE],
           (double)vp->filter.eg_depth[0][SEQ_EGT_MIX],
           (double)vp->filter.eg_depth[1][SEQ_EGT_PITCH],
           (double)vp->filter.eg_depth[1][SEQ_EGT_CUTOFF],
           (double)vp->filter.eg_depth[1][SEQ_EGT_DRIVE],
           (double)vp->filter.eg_depth[1][SEQ_EGT_MIX]);
    }
    if (vp->dist_authored || vp->lfo_authored) {
        DP("  %s dist auth=%u type=%u drive=%u bits=%u rate=%u mix=%u lfo auth=%u",
           tag, (unsigned)vp->dist_authored, vp->dist.type,
           vp->dist.drive, vp->dist.bits, vp->dist.rate, vp->dist.mix,
           (unsigned)vp->lfo_authored);
    }
    if (vp->amp_trim != 1.0f) {
        DP("  %s trim=%.2f", tag, (double)vp->amp_trim);
    }
    return n_lines;
}

void sequencer_core_dump_state(void)
{
    unsigned n_lines = 0;

    DP("==== SEQDUMP BEGIN fw=%s layers=%u engine=%s ====",
       esp_app_get_description()->version, (unsigned)s_num_layers,
       sequencer_core_get_drum_engine() == SEQ_DRUM_PCM ? "PCM" : "SYNTH");

    for (uint8_t li = 0; li < s_num_layers; li++) {
        const seq_layer_t *L = &s_layers[li];
        bool drum = (L->type == SEQ_LAYER_DRUM);

        if (drum) {
            DP("L%u DRUM steps=%u swing=%u groove=%u",
               li + 1u, L->num_steps, L->swing_pct, L->groove_pct);
        } else {
            DP("L%u MELODIC steps=%u patch=%u scope=%s rows=%u/%u/%u/%u"
               " voices=%u swing=%u gate=%u"
               " porta=%u groove=%u chord=%s root=%u type=%u",
               li + 1u, L->num_steps, L->patch,
               L->patch_scope == SEQ_PATCH_SCOPE_TRACK ? "TRACK" : "LAYER",
               L->track_patch[0], L->track_patch[1],
               L->track_patch[2], L->track_patch[3],
               L->num_voices, L->swing_pct,
               L->gate_pct, L->portamento_ms, L->groove_pct,
               L->chord_mode ? "on" : "off", L->chord_root,
               (unsigned)L->chord_type);
        }

        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            /* Track line: pitch and timbre first (the harvest columns). */
            if (drum) {
                /* PCM preset/mode via the core getters; the seq_layer_t
                 * fields are stale on this side (seq_model.h). */
                uint16_t pcm = sequencer_core_get_drum_pcm_preset(li, t);
                uint8_t pcm_mode = sequencer_core_get_drum_pcm_mode(li, t);
                DP("  T%u note=%u patch=%u pcm=%u mode=%u(%s) rep=%u"
                   " mute=%u solo=%u",
                   t + 1u, L->track_base_note[t], L->track_patch[t],
                   pcm, pcm_mode, pcm_mode_name(pcm_mode),
                   L->repeat_rate[t],
                   (unsigned)L->mute[t], (unsigned)L->solo[t]);
            } else {
                DP("  T%u note=%u rep=%u mute=%u solo=%u",
                   t + 1u, L->track_base_note[t], L->repeat_rate[t],
                   (unsigned)L->mute[t], (unsigned)L->solo[t]);
            }

            /* The row's OWN block, whichever one it currently reads; the
             * shared layer block follows the track lines. */
            const voice_params_t *vp = &L->vp[t];
            if (L->vp_src[t] == SEQ_VP_SRC_LAYER) {
                DP("  T%u src=LAYER", t + 1u);
            }
            char tag[4];
            snprintf(tag, sizeof tag, "T%u", t + 1u);
            n_lines += dump_vp(tag, vp);

            char grid[SEQ_MAX_STEPS + 1];
            for (uint8_t s = 0; s < L->num_steps; s++)
                grid[s] = L->grid[t][s] ? 'X' : '.';
            grid[L->num_steps] = '\0';
            DP("  T%u |%s|", t + 1u, grid);

            /* Step exceptions: only active steps, only non-neutral fields. */
            for (uint8_t s = 0; s < L->num_steps; s++) {
                if (!L->grid[t][s]) continue;
                bool off_note = (L->step_note[t][s] != L->track_base_note[t]);
                bool dec = off_note ||
                           L->step_pitch_ofs[t][s] ||
                           L->step_prob[t][s] != 100 ||
                           L->step_ratchet[t][s] != 1 ||
                           L->step_every[t][s] > 1 ||
                           L->step_prev[t][s] ||
                           L->step_transform[t][s] ||
                           L->step_quant_bypass[t][s] ||
                           L->step_nudge[t][s] ||
                           L->step_velocity_adj[t][s] ||
                           L->step_ratchet_taper[t][s];
                if (!dec) continue;

                char line[128];
                int n = snprintf(line, sizeof line, "  T%u s%u",
                                 t + 1u, s + 1u);
                #define AP(...) \
                    n += snprintf(line + n, sizeof line - (size_t)n, __VA_ARGS__)
                if (off_note)                    AP(" note=%u", L->step_note[t][s]);
                if (L->step_pitch_ofs[t][s])     AP(" pofs=%+d", L->step_pitch_ofs[t][s]);
                if (L->step_prob[t][s] != 100)   AP(" prob=%u", L->step_prob[t][s]);
                if (L->step_ratchet[t][s] != 1)  AP(" rat=%u", L->step_ratchet[t][s]);
                if (L->step_ratchet_taper[t][s]) AP(" taper=%d", L->step_ratchet_taper[t][s]);
                if (L->step_every[t][s] > 1)     AP(" every=%u", L->step_every[t][s]);
                if (L->step_prev[t][s])          AP(" prev");
                if (L->step_transform[t][s])     AP(" xform=%s", xform_name(L->step_transform[t][s]));
                if (L->step_quant_bypass[t][s])  AP(" qbyp");
                if (L->step_nudge[t][s])         AP(" nudge=%d", L->step_nudge[t][s]);
                if (L->step_velocity_adj[t][s])  AP(" vel=%d", L->step_velocity_adj[t][s]);
                #undef AP
                DP("%s", line);
            }
        }

        /* The layer's shared voice block: which groups it has authority over
         * (rows with src=LAYER read these; unauthored groups fall through to
         * the patch). */
        if (!drum) {
            const voice_params_t *lv = &L->vp_layer;
            DP("  LAYER-BLK eg0=%u eg1=%u flt=%u lfo=%u dist=%u",
               (unsigned)lv->env_authored, (unsigned)lv->env1_authored,
               (unsigned)lv->filter_authored, (unsigned)lv->lfo_authored,
               (unsigned)lv->dist_authored);
            n_lines += dump_vp("LB", lv);
        }
    }

    DP("==== SEQDUMP END layers=%u lines=%u ====",
       (unsigned)s_num_layers, n_lines + 1u);
    fflush(stdout);
}
