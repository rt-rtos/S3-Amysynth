#include "custompatches/fm_presets.h"
#include "custompatches/fm_voice.h"
#include "sequencer_core.h"   /* SEQ_PATCH_FM_* */

/* Operator-array index -> role, for algorithm 0 (index convention: fm_voice.h):
 *   0,1,2 : deep modulator stack (feeds index 3)
 *   3     : carrier A (chain-A output)
 *   4     : modulator (feeds index 5)
 *   5     : carrier B (chain-B output)
 * All four presets silence index 0 (op6, the only FB_IN slot in this
 * algorithm), so `feedback` goes unused here; it stays available via the
 * FM_CUSTOM voice. */

static void fm_preset_bass(fm_voice_t *v)
{
    fm_voice_default(v);
    v->algorithm = 0;
    v->feedback  = 0.0f;
    /* Chain A silent. */
    v->op_level[0] = 0.0f; v->op_level[1] = 0.0f; v->op_level[2] = 0.0f; v->op_level[3] = 0.0f;
    /* Chain B: mellow, round low end. */
    v->op_ratio[4] = 1.0f; v->op_level[4] = 0.35f;
    v->op_ratio[5] = 1.0f; v->op_level[5] = 1.0f;
}

static void fm_preset_epiano(fm_voice_t *v)
{
    fm_voice_default(v);
    v->algorithm = 0;
    v->feedback  = 0.0f;
    /* Chain A: index1 (bright, high inharmonic ratio) modulates index2, which
     * modulates carrier index3 - the classic FM E.Piano bell attack. */
    v->op_level[0] = 0.0f;
    v->op_ratio[1] = 14.0f; v->op_level[1] = 0.20f;
    v->op_ratio[2] = 1.0f;  v->op_level[2] = 0.40f;
    v->op_ratio[3] = 1.0f;  v->op_level[3] = 0.80f;
    /* Chain B: warm sub-layer under the bell attack. */
    v->op_ratio[4] = 1.0f;  v->op_level[4] = 0.30f;
    v->op_ratio[5] = 1.0f;  v->op_level[5] = 0.60f;
}

static void fm_preset_bell(fm_voice_t *v)
{
    fm_voice_default(v);
    v->algorithm = 0;
    v->feedback  = 0.0f;
    /* Chain A: inharmonic ratios throughout for a metallic partial. */
    v->op_level[0] = 0.0f;
    v->op_ratio[1] = 3.5f;  v->op_level[1] = 0.50f;
    v->op_ratio[2] = 1.4f;  v->op_level[2] = 0.40f;
    v->op_ratio[3] = 1.0f;  v->op_level[3] = 0.70f;
    /* Chain B: a second inharmonic shimmer partial. */
    v->op_ratio[4] = 7.0f;  v->op_level[4] = 0.35f;
    v->op_ratio[5] = 1.0f;  v->op_level[5] = 0.60f;
}

static void fm_preset_lead(fm_voice_t *v)
{
    fm_voice_default(v);
    v->algorithm = 0;
    v->feedback  = 0.0f;
    /* Chain A silent. */
    v->op_level[0] = 0.0f; v->op_level[1] = 0.0f; v->op_level[2] = 0.0f; v->op_level[3] = 0.0f;
    /* Chain B: brighter/edgier than the bass preset. */
    v->op_ratio[4] = 2.0f; v->op_level[4] = 0.60f;
    v->op_ratio[5] = 1.0f; v->op_level[5] = 1.0f;
}

void fm_preset_configure_track(uint8_t synth_id, uint16_t patch,
                               uint16_t num_voices)
{
    fm_voice_t v;
    switch (patch) {
        case SEQ_PATCH_FM_BASS:   fm_preset_bass(&v);   break;
        case SEQ_PATCH_FM_EPIANO: fm_preset_epiano(&v); break;
        case SEQ_PATCH_FM_BELL:   fm_preset_bell(&v);   break;
        case SEQ_PATCH_FM_LEAD:   fm_preset_lead(&v);   break;
        default:                  fm_voice_default(&v); break;
    }
    fm_voice_configure_track(synth_id, num_voices, &v);
}
