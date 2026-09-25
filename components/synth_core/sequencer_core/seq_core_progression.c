#include "sequencer_core/seq_core_internal.h"

/* ── State definitions — owns chord progression ──────────────────────── */
chord_progression_t s_prog = {
    .entries = {
        { .root = 0, .chord_type = CHORD_MAJ7, .duration_bars = 1 },  /* 1st chord -> 1b */
    },
    .count   = 1,
    .current = 0,
    .entry_start_bar = 0,
    .enabled = false,
};

/* The generator writes whole progressions into s_prog.entries, so its output
 * cap and the entry array must be the same size. */
_Static_assert(PROG_GEN_MAX_ENTRIES == CHORD_PROG_MAX_ENTRIES,
               "prog_gen output must fit the progression entry array");

/* Settings that produced the current progression; persisted alongside it. */
static prog_gen_params_t s_prog_gen_params = {
    .style = 0, .len = 4, .bars = 1, .ext = PROG_GEN_EXT_TRIAD, .var = 0, .seed = 0
};

/* Set by input-task entry points that change chord state, consumed once per
 * tick by sequencer_core_progression_service() on synth_ui_task. That makes
 * synth_ui_task the SINGLE task calling chord_progression_apply_current(), so
 * chord edits never race the periodic advance or each other. */
volatile bool s_prog_apply_pending = false;

/* Set alongside s_prog_apply_pending when the apply is a correction (play-start
 * re-sync) rather than a musical edit, so it bypasses the BAR
 * launch-quantize hold and lands on the next service tick. */
volatile bool s_prog_apply_immediate = false;

/* Rows every melodic layer should hold, requested by the Prog Gen Generate
 * handler (widest generated chord); 0 = no request. Consumed by the next
 * drain, before the chord apply, so grown rows are snapped in that apply. */
static volatile uint8_t s_prog_rows_wanted = 0;

/* BAR launch mode: bar index at which a held pending apply was armed;
 * UINT32_MAX = not armed. Drains once the lead-adjusted bar count moves past
 * this value. */
static uint32_t s_apply_armed_bar = UINT32_MAX;

/* Chord changes are applied this far ahead of the bar line. A step's pitch is
 * baked into its periodic AMY entry, and step 0 fires one tick after the
 * line, so an apply that waits for the line lands after the downbeat has
 * already played the old chord. The lead covers one 50 ms service period
 * plus the trip through AMY's event queue. The ending bar's last 16th fires
 * 11 ticks before the line, so it takes the new chord early from about
 * 211 BPM (a tick of 5.9 ms or less), or sooner with swing or nudge; the
 * lead stays uncapped, because a cap brings back the late downbeat at those
 * same tempos. */
#define PROG_APPLY_LEAD_US 65000u

static uint32_t prog_apply_lead_ticks(void)
{
    uint32_t upt = amy_global.us_per_tick;
    return upt ? (PROG_APPLY_LEAD_US + upt - 1u) / upt : 0u;
}

/* ── Private helpers ─────────────────────────────────────────────────── */

/* Map a chord type to the closest diatonic scale for arp snap quality. */
static uint8_t chord_type_to_scale_index(chord_type_t ct)
{
    /* Scale indices mirror s_scales[] in quantizer.c:
     * 0=Chromatic, 1=Major, 2=Natural Minor, 3=Dorian, 4=Phrygian,
     * 5=Lydian, 6=Mixolydian, 7=Minor Pent, 8=Major Pent,
     * 9=Harmonic Minor, 10=Locrian, 11=Whole Tone */
    switch (ct) {
        case CHORD_MAJ:  return 1;
        case CHORD_MIN:  return 2;
        case CHORD_MAJ7: return 1;
        case CHORD_MIN7: return 3;  /* Dorian has the natural 6 common in min7 contexts */
        case CHORD_DOM7: return 6;  /* Mixolydian */
        case CHORD_SUS2: return 8;  /* Major Pentatonic - open, no 3rd */
        case CHORD_SUS4: return 8;
        case CHORD_DIM:  return 10; /* Locrian - its b5 IS the chord tone; a
                                       natural 5 clashes a semitone against it */
        case CHORD_AUG:  return 11; /* Whole Tone - contains the #5, same
                                       natural-5 clash otherwise */
        case CHORD_MIN9: return 3;  /* Dorian - same family as min7 */
        case CHORD_MAJ9: return 1;  /* Major */
        default:         return 1;
    }
}

void chord_progression_apply_current(void)
{
    if (s_prog.count == 0) return;
    const chord_prog_entry_t *e = &s_prog.entries[s_prog.current];

    /* Update every melodic layer's chord state and re-resolve all tracks. */
    for (uint8_t li = 0; li < s_num_layers; li++) {
        seq_layer_t *layer = &s_layers[li];
        if (layer->type != SEQ_LAYER_MELODIC) continue;
        layer->chord_mode  = true;
        layer->chord_root  = e->root;
        layer->chord_type  = e->chord_type;
    }
    sequencer_refresh_melodic_layers(false);

    /* Drive arp root + scale to the new chord, capturing the user's own values
     * the first time the progression takes over; disable restores them. */
    if (!s_prog.arp_saved) {
        s_prog.saved_arp_root  = arp_get_root_note();
        s_prog.saved_arp_scale = arp_get_scale();
        s_prog.arp_saved = true;
    }
    arp_set_root_note((uint8_t)(e->root + 60));   /* pitch class → MIDI octave 4 */
    arp_set_scale(chord_type_to_scale_index(e->chord_type));
}

/* Called from synth_ui_task at 20 Hz - the SINGLE task that emits chord changes
 * to AMY. It drains s_prog_apply_pending (set by input tasks after mutating
 * chord state) and advances the progression when the current entry expires.
 * Funnelling every emit through one task removes the need for a lock: there is
 * only one writer. */
uint8_t sequencer_core_progression_service(void)
{
    uint8_t grown = 0;

    /* Bar count as it will read at the next bar line's lead point: every
     * bar-quantized apply below keys on this, so it lands just before the
     * line instead of just after it. */
    uint32_t bars_ahead = sequencer_bars_elapsed_ahead(prog_apply_lead_ticks());

    /* Drain deferred applies first, regardless of playing/enabled state. In BAR
     * launch mode a musical edit holds until the next bar line while playing;
     * corrections and drains while stopped bypass the hold. */
    if (s_prog_apply_pending) {
        bool drain_now = true;
        if (s_prog.apply_at_bar && s_playing && !s_prog_apply_immediate) {
            /* Arm on the bar the edit was made in, so an edit inside the lead
             * window still lands on the upcoming line. */
            if (s_apply_armed_bar == UINT32_MAX) s_apply_armed_bar = sequencer_bars_elapsed();
            drain_now = (bars_ahead != s_apply_armed_bar); /* reached the lead point */
        }
        if (drain_now) {
            s_apply_armed_bar = UINT32_MAX;
            s_prog_apply_pending = false;
            s_prog_apply_immediate = false;
            uint8_t rows = s_prog_rows_wanted;
            s_prog_rows_wanted = 0;
            for (uint8_t li = 0; rows && li < s_num_layers; li++) {
                if (s_layers[li].type != SEQ_LAYER_MELODIC) continue;
                if (s_layers[li].num_tracks >= rows) continue;
                if (sequencer_core_set_layer_tracks(li, rows)) grown |= (uint8_t)(1u << li);
            }
            if (s_prog.enabled && s_prog.count > 0) {
                chord_progression_apply_current();
            } else {
                /* Disabled or empty: the caller already cleared chord_mode, or
                 * a manual per-layer chord changed. Re-resolve every melodic
                 * layer against its own chord/scale state. */
                sequencer_refresh_melodic_layers(false);
            }
        }
    } else {
        s_apply_armed_bar = UINT32_MAX;
    }

    if (!s_prog.enabled || s_prog.count == 0 || !s_playing) return grown;

    uint32_t bars = bars_ahead;
    /* Mid-session anchors point at the NEXT bar line (entry_start_bar = bars+1),
     * and the unsigned subtraction below would wrap until it passes. */
    if (bars < s_prog.entry_start_bar) return grown;
    const chord_prog_entry_t *e = &s_prog.entries[s_prog.current];

    if (bars - s_prog.entry_start_bar >= e->duration_bars) {
        uint8_t next = (uint8_t)((s_prog.current + 1) % s_prog.count);
        s_prog.current = next;
        /* Advance by the expiring entry's duration, never "= bars": a service
         * stall longer than a bar then catches up over successive ticks instead
         * of permanently shifting the form. */
        s_prog.entry_start_bar += e->duration_bars;
        chord_progression_apply_current();
        ESP_LOGI(TAG, "progression -> entry %u (root=%u type=%u)",
                 next, s_prog.entries[next].root, (unsigned)s_prog.entries[next].chord_type);
    }
    return grown;
}

/* ── Progression public API ─────────────────────────────────────────────── */

void sequencer_core_progression_request_rows(uint8_t rows)
{
    if (rows > SEQ_TRACKS) rows = SEQ_TRACKS;
    s_prog_rows_wanted = rows;
}

void sequencer_core_progression_set_enabled(bool en)
{
    s_prog.enabled = en;
    if (en && s_prog.count > 0) {
        /* The chord applies now (or at the next bar line in BAR mode), but its
         * duration window counts from the NEXT bar line, so a mid-bar enable
         * still gives the first entry its full duration - the partial bar is a
         * free lead-in, which the service's bars < entry_start_bar guard
         * covers. */
        s_prog.current = 0;
        s_prog.entry_start_bar = sequencer_bars_elapsed() + 1;
        /* Defer the AMY emit to the service tick (single-applier). */
        s_prog_apply_pending = true;
    } else if (!en) {
        /* Drop chord mode everywhere so layers return to the scale quantizer;
         * the re-resolve emit is deferred to the service tick. */
        for (uint8_t li = 0; li < s_num_layers; li++) {
            s_layers[li].chord_mode = false;
        }
        /* Return the arp to the user's pre-progression root/scale. These are
         * state-only setters - the arp marks itself dirty and arp_core_service
         * re-emits on the UI task - so the single-applier rule holds. */
        if (s_prog.arp_saved) {
            arp_set_root_note(s_prog.saved_arp_root);
            arp_set_scale(s_prog.saved_arp_scale);
            s_prog.arp_saved = false;
        }
        s_prog_apply_pending = true;
    }
}

bool sequencer_core_progression_get_enabled(void) { return s_prog.enabled; }

/* arp_saved spans exactly the window in which the arp's root/scale hold
 * progression chords rather than user values: set on the first chord apply,
 * cleared when disable restores them. */
bool sequencer_core_progression_arp_owned(void) { return s_prog.arp_saved; }

/* Launch quantization: false = chord applies land on the next service tick
 * (instant), true = musical edits hold until the next bar line while playing. */
void sequencer_core_progression_set_apply_at_bar(bool at_bar)
{
    s_prog.apply_at_bar = at_bar;
}

bool sequencer_core_progression_get_apply_at_bar(void)
{
    return s_prog.apply_at_bar;
}

/* Drop the captured pre-progression arp root/scale WITHOUT restoring it.
 * Project load calls this before applying the loaded progression state: the
 * snapshot's arp values are the new baseline, so a capture from the pre-load
 * session must not be restored over them. */
void sequencer_core_progression_reset_arp_capture(void)
{
    s_prog.arp_saved = false;
}

void sequencer_core_progression_set_entry(uint8_t idx, uint8_t root,
                                          chord_type_t chord_type,
                                          uint8_t duration_bars)
{
    if (idx >= CHORD_PROG_MAX_ENTRIES) return;
    /* Refuse holes: idx == count appends, anything past that legalizes the
     * zero-filled entries in between, whose duration_bars 0 would make the
     * service advance every tick - a 20 Hz chord strobe. */
    if (idx > s_prog.count) return;
    s_prog.entries[idx].root          = root % 12;
    s_prog.entries[idx].chord_type    = (chord_type < CHORD_TYPE_COUNT)
                                        ? chord_type : CHORD_MAJ;
    s_prog.entries[idx].duration_bars = (duration_bars > 0) ? duration_bars : 4;
    if (idx >= s_prog.count) s_prog.count = (uint8_t)(idx + 1);
    /* Editing the live entry defers a re-apply so the audible chord follows. */
    if (s_prog.enabled && idx == s_prog.current) s_prog_apply_pending = true;
}

void sequencer_core_progression_get_entry(uint8_t idx, uint8_t *root,
                                          chord_type_t *chord_type,
                                          uint8_t *duration_bars)
{
    /* Defaults first: an out-of-range idx must not leave the caller's
     * (possibly uninitialised) locals unwritten. */
    if (root)          *root          = 0;
    if (chord_type)    *chord_type    = CHORD_MAJ;
    if (duration_bars) *duration_bars = 1;
    if (idx >= s_prog.count || idx >= CHORD_PROG_MAX_ENTRIES) return;
    if (root)          *root          = s_prog.entries[idx].root;
    if (chord_type)    *chord_type    = s_prog.entries[idx].chord_type;
    if (duration_bars) *duration_bars = s_prog.entries[idx].duration_bars;
}

void sequencer_core_progression_set_count(uint8_t count)
{
    if (count > CHORD_PROG_MAX_ENTRIES) count = CHORD_PROG_MAX_ENTRIES;
    s_prog.count = count;
    /* An active entry that fell out of range wraps to 0, restarts its bar
     * window from the next bar line (same anchor as enable) and re-applies. */
    if (s_prog.current >= s_prog.count && s_prog.count > 0) {
        s_prog.current = 0;
        s_prog.entry_start_bar = sequencer_bars_elapsed() + 1;
        if (s_prog.enabled) s_prog_apply_pending = true;
    }
}

uint8_t sequencer_core_progression_get_count(void) { return s_prog.count; }
uint8_t sequencer_core_progression_get_current(void) { return s_prog.current; }
uint8_t sequencer_core_progression_get_max(void) { return CHORD_PROG_MAX_ENTRIES; }

/* Bars elapsed within the current entry (0-based), for the UI status bar. */
uint8_t sequencer_core_progression_bars_in_current(void)
{
    /* Ticks are monotonic through stop, so without this gate the counter would
     * keep climbing (and uint8-wrap) while paused. */
    if (!s_playing) return 0;
    if (!s_prog.enabled || s_prog.count == 0) return 0;
    uint32_t bars = sequencer_bars_elapsed();
    if (bars < s_prog.entry_start_bar) return 0;   /* baseline just moved */
    return (uint8_t)(bars - s_prog.entry_start_bar);
}

/* Append a default entry (Cmaj) if room remains. The Nth chord defaults to an
 * N-bar duration so a fresh progression staircases 1/2/3/4 bars; past the 4th
 * it clamps to 4, since 5/6/7 are not in the selectable duration set
 * {1,2,3,4,8,16}. */
bool sequencer_core_progression_add_entry(void)
{
    if (s_prog.count >= CHORD_PROG_MAX_ENTRIES) return false;
    uint8_t idx = s_prog.count;
    uint8_t entry_num = (uint8_t)(idx + 1);         /* 1-based position */
    s_prog.entries[idx].root          = 0;          /* C */
    s_prog.entries[idx].chord_type    = CHORD_MAJ;
    s_prog.entries[idx].duration_bars = entry_num <= 4 ? entry_num : 4;
    s_prog.count = entry_num;
    return true;
}

/* Delete entry idx, shifting later entries down. Keeps at least one entry. */
void sequencer_core_progression_delete_entry(uint8_t idx)
{
    if (idx >= s_prog.count || s_prog.count <= 1) return;
    for (uint8_t i = idx; i + 1 < s_prog.count; i++) {
        s_prog.entries[i] = s_prog.entries[i + 1];
    }
    s_prog.count--;
    /* Fix the active index/window if it was at or past the deletion point. */
    bool active_changed = false;
    if (s_prog.current == idx) {
        if (s_prog.current >= s_prog.count) s_prog.current = 0;
        active_changed = true;
    } else if (s_prog.current > idx) {
        s_prog.current--;   /* same entry, new slot - no audible change */
    }
    if (active_changed) {
        s_prog.entry_start_bar = sequencer_bars_elapsed() + 1;
        if (s_prog.enabled) s_prog_apply_pending = true;
    }
}

void sequencer_core_progression_gen_params_set(const prog_gen_params_t *p)
{
    if (p == NULL) return;
    s_prog_gen_params = *p;
}

void sequencer_core_progression_gen_params_get(prog_gen_params_t *out)
{
    if (out == NULL) return;
    *out = s_prog_gen_params;
}

void sequencer_core_progression_set_layer_chord(uint8_t layer_idx,
                                                uint8_t root,
                                                chord_type_t chord_type)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return;
    layer->chord_mode = true;
    layer->chord_root = root % 12;
    layer->chord_type = chord_type;
    /* Defer the re-resolve emit to the service tick (single-applier). */
    s_prog_apply_pending = true;
}

void sequencer_core_progression_clear_layer_chord(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return;
    s_layers[layer_idx].chord_mode = false;
    /* Defer the re-resolve emit to the service tick (single-applier). */
    s_prog_apply_pending = true;
}

void sequencer_core_get_layer_chord(uint8_t layer_idx, bool *chord_mode,
                                    uint8_t *root, chord_type_t *chord_type)
{
    if (layer_idx >= s_num_layers) return;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (chord_mode) *chord_mode = layer->chord_mode;
    if (root)       *root       = layer->chord_root;
    if (chord_type) *chord_type = layer->chord_type;
}
