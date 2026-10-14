#include "lfo_targets.h"
#include "sequencer_core.h"   /* SEQ_PATCH_* ranges */

/* Osc layouts per family: lfo_targets.h. */
bool lfo_targets_of_patch(uint16_t patch, lfo_target_map_t *out)
{
    lfo_target_map_t m;
    if (patch < SEQ_PATCH_DX7_BASE) {
        m = (lfo_target_map_t){ .voice_mask = 0x01u,   /* SILENT head      */
                                .pitch_mask = 0x1Cu,   /* oscs 2, 3, 4     */
                                .scan_mask  = 0x14u }; /* pulse oscs 2, 4  */
    } else if ((patch >= SEQ_PATCH_DX7_BASE && patch <= SEQ_PATCH_DX7_MAX) ||
               (patch >= SEQ_PATCH_FM_BASE && patch <= SEQ_PATCH_FM_MAX)) {
        m = (lfo_target_map_t){ .voice_mask = 0x01u,   /* ALGO control osc */
                                .pitch_mask = 0x01u,
                                .scan_mask  = 0x00u };
    } else if (patch >= SEQ_PATCH_BASS_BASE && patch <= SEQ_PATCH_BASS_MAX) {
        m = (lfo_target_map_t){ .voice_mask = 0x03u,   /* oscs 0-1 audible */
                                .pitch_mask = 0x03u,
                                .scan_mask  = 0x03u };
    } else if (patch >= SEQ_PATCH_ADDITIVE_BASE && patch <= SEQ_PATCH_ADDITIVE_MAX) {
        m = (lfo_target_map_t){ .voice_mask = 0x01u,   /* BYO_PARTIALS parent */
                                .pitch_mask = 0x01u,
                                .scan_mask  = 0x00u };
    } else {
        return false;
    }
    if (out) *out = m;
    return true;
}

bool lfo_targets_patch_owns_slot0(uint16_t patch)
{
    return patch <= SEQ_PATCH_DX7_MAX;   /* Juno 0..127, DX7 bank */
}
