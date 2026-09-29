#include "sdkconfig.h"
#if CONFIG_SYNTH_DEV_MENU

/* DEV: KS riff A/B. A fixed six-string Karplus-Strong riff, played two ways
 * so a bare KS string and the device's own melodic voice can be heard on the
 * same hardware:
 *
 *   A - bare strings: six fresh synths, one voice and one KS osc each
 *       (one string per synth), feedback 0.996, amp = velocity, no envelope,
 *       duty 0.50 or 0.37; a string's next note cuts its last one, and nothing
 *       else releases until the tail ends. The synths are the six slots after
 *       the first melodic layer's block, free while only layers 1 and 2 exist.
 *   B - the same notes and timing on the first melodic layer's row synths,
 *       with their live voice (patch, envelope, filter, FX bus). Each note is
 *       released after the layer's gate % of one step at the current BPM, as
 *       the sequencer would; string i plays on row i % rows, so a 4-row layer
 *       needs 2 voices per row for the final six-string strum.
 *
 * Both passes stop the transport and schedule every event on AMY's clock at
 * the press, so nothing runs afterwards. Both render on the melodic bus, so
 * that bus's FX apply to both. */

#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "synth_slots.h"
#include "amy_helpers.h"
#include "amy.h"

#define RIFF_EIGHTH_MS  220u
#define RIFF_STRUM_MS   15u
#define RIFF_TAIL_MS    2600u
#define RIFF_LEAD_MS    150u     /* scheduling headroom after the press */
#define RIFF_STRINGS    6u
#define RIFF_FEEDBACK   0.996f

typedef struct { uint8_t t8; uint8_t string; uint8_t note; float vel; } riff_ev_t;

/* Riff events: 8th-note offset, string, MIDI note, velocity. */
static const riff_ev_t s_evs[] = {
    { 0, 0, 40, 0.8f }, { 0, 1, 47, 0.8f }, { 0, 2, 52, 0.8f },
    { 2, 3, 55, 0.7f }, { 3, 3, 57, 0.7f }, { 4, 4, 59, 0.7f }, { 5, 3, 55, 0.7f },
    { 6, 0, 40, 0.8f }, { 6, 1, 47, 0.8f }, { 6, 2, 52, 0.8f },
    { 8, 1, 48, 0.8f }, { 8, 2, 55, 0.8f }, { 8, 3, 60, 0.8f },
    { 10, 1, 50, 0.8f }, { 10, 2, 57, 0.8f }, { 10, 3, 62, 0.8f },
    { 12, 5, 64, 0.9f }, { 13, 5, 67, 0.9f }, { 14, 5, 69, 0.9f }, { 15, 5, 67, 0.9f },
    { 16, 0, 40, 0.9f }, { 16, 1, 47, 0.9f }, { 16, 2, 52, 0.9f },
    { 16, 3, 55, 0.9f }, { 16, 4, 59, 0.9f }, { 16, 5, 64, 0.9f },
};
#define RIFF_NEV (sizeof s_evs / sizeof *s_evs)

static float    s_duty = 0.5f;
static uint32_t s_busy_until;    /* amy_sysclock() ms; 0 = idle */

/* Offset in ms of event i from the riff start: its 8th, plus STRUM per
 * string below it in the same shape (its rank). */
static uint32_t riff_on_ms(unsigned i)
{
    unsigned rank = 0;
    for (unsigned j = 0; j < i; j++)
        if (s_evs[j].t8 == s_evs[i].t8) rank++;
    return (uint32_t)s_evs[i].t8 * RIFF_EIGHTH_MS + rank * RIFF_STRUM_MS;
}

/* Offset of the next note on event i's string, or 0 if it is the last. */
static uint32_t riff_next_on_ms(unsigned i)
{
    for (unsigned j = i + 1; j < RIFF_NEV; j++)
        if (s_evs[j].string == s_evs[i].string) return riff_on_ms(j);
    return 0;
}

static void riff_note(uint8_t synth, uint8_t note, float vel, uint32_t at)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth     = synth;
    e->midi_note = note;
    e->velocity  = vel;
    e->time      = at;
    amy_helpers_event_send(e);
}

static int riff_first_melodic_layer(void)
{
    for (uint8_t li = 0; li < sequencer_core_get_num_layers(); li++)
        if (sequencer_core_get_layer_type(li) == SEQ_LAYER_MELODIC) return li;
    return -1;
}

bool synth_ui_dev_riff_busy(void)
{
    return s_busy_until && !AMY_TIME_GEQ(amy_sysclock(), s_busy_until);
}

/* Stop the transport so the grid does not play over the riff. */
static void riff_stop_transport(void)
{
    if (seq_state.playing) {
        seq_state.playing = false;
        sequencer_core_set_playing(false);
    }
}

float synth_ui_dev_riff_duty(void) { return s_duty; }

void synth_ui_dev_riff_toggle_duty(void)
{
    s_duty = (s_duty > 0.45f) ? 0.37f : 0.5f;
}

void synth_ui_dev_riff_play_a(void)
{
    int li = riff_first_melodic_layer();
    if (li < 0 || synth_ui_dev_riff_busy()) return;
    uint8_t base = (uint8_t)(sequencer_core_get_track_synth((uint8_t)li, 0) + SEQ_TRACKS);
    if (base + RIFF_STRINGS - 1u > SEQ_MAX_SYNTH) return;
    riff_stop_transport();

    for (uint8_t s = 0; s < RIFF_STRINGS; s++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth          = (uint8_t)(base + s);
        e->num_voices     = 1;
        e->oscs_per_voice = 1;
        amy_helpers_event_send(e);

        e = amy_helpers_event_begin();
        e->synth                  = (uint8_t)(base + s);
        e->osc                    = 0;
        e->wave                   = KS;
        e->feedback               = RIFF_FEEDBACK;
        e->freq_coefs[COEF_NOTE]  = 1.0f;
        /* amp = velocity: the same dB-model gain as a per-note
         * COEF_CONST = vel with COEF_VEL 0 (amp_combine_controls; a CONST
         * of 1 adds 0 dB). CONST must stay nonzero: render_osc_wave skips
         * an osc whose amp CONST is 0. */
        e->amp_coefs[COEF_CONST]  = 1.0f;
        e->amp_coefs[COEF_VEL]    = 1.0f;
        e->amp_coefs[COEF_EG0]    = 0.0f;
        e->duty_coefs[COEF_CONST] = s_duty;
        amy_helpers_event_send(e);
    }

    uint32_t t0 = amy_sysclock() + RIFF_LEAD_MS;
    uint32_t end = (uint32_t)s_evs[RIFF_NEV - 1].t8 * RIFF_EIGHTH_MS + RIFF_TAIL_MS;
    for (unsigned i = 0; i < RIFF_NEV; i++) {
        uint8_t synth = (uint8_t)(base + s_evs[i].string);
        riff_note(synth, s_evs[i].note, s_evs[i].vel, t0 + riff_on_ms(i));
        /* A string's next note cuts this one, 1 ms ahead so the voice is free;
         * the last note on each string rings to the end of the tail. */
        uint32_t next = riff_next_on_ms(i);
        riff_note(synth, s_evs[i].note, 0.0f, t0 + (next ? next - 1u : end));
    }
    s_busy_until = t0 + end;
}

void synth_ui_dev_riff_play_b(void)
{
    int li = riff_first_melodic_layer();
    if (li < 0 || synth_ui_dev_riff_busy()) return;
    uint8_t rows = sequencer_core_get_layer_tracks((uint8_t)li);
    if (rows == 0) return;
    riff_stop_transport();
    uint16_t bpm = seq_get_bpm();
    if (bpm == 0) bpm = 120;
    /* One 16th step at the current tempo, held for the layer's gate %. */
    uint32_t gate = 15000u * sequencer_core_get_layer_gate_pct((uint8_t)li)
                    / 100u / bpm;
    if (gate < 1u) gate = 1u;

    uint32_t t0 = amy_sysclock() + RIFF_LEAD_MS;
    uint32_t end = (uint32_t)s_evs[RIFF_NEV - 1].t8 * RIFF_EIGHTH_MS + RIFF_TAIL_MS;
    for (unsigned i = 0; i < RIFF_NEV; i++) {
        uint8_t synth = sequencer_core_get_track_synth((uint8_t)li,
                                                       (uint8_t)(s_evs[i].string % rows));
        uint32_t on = riff_on_ms(i);
        uint32_t off = on + gate;
        uint32_t next = riff_next_on_ms(i);
        if (next && off >= next) off = next - 1u;
        riff_note(synth, s_evs[i].note, s_evs[i].vel, t0 + on);
        riff_note(synth, s_evs[i].note, 0.0f, t0 + off);
    }
    s_busy_until = t0 + end;
}

extern uint16_t *voice_to_base_osc;   /* patches.c */

/* Every osc of every voice of one synth, as AMY holds it. */
static void riff_dump_synth(uint8_t slot)
{
    uint16_t voices[MAX_VOICES_PER_INSTRUMENT];
    int nv = instrument_get_num_voices(slot, voices);
    fprintf(stderr, "synth %u: %d voices, bus %d, level %.3f\n", (unsigned)slot, nv,
            nv ? instrument_get_bus(slot) : -1,
            nv ? (double)instrument_get_level(slot) : 0.0);
    for (int v = 0; v < nv; v++) {
        uint16_t base = voice_to_base_osc[voices[v]];
        if (AMY_IS_UNSET(base)) continue;
        for (uint16_t o = base; o < AMY_OSCS && osc_to_voice[o] == voices[v]; o++) {
            print_osc_debug(o, true);
            /* Fields print_osc_debug leaves out: the dist stage, and the KS
             * ring (two sounding oscs on one ring = stealing). */
            if (synth[o] != NULL)
                fprintf(stderr, "  voice %u dist stages=%u bits=%u rate=%u ks_index=%u\n",
                        (unsigned)voices[v], (unsigned)synth[o]->dist_stages,
                        (unsigned)synth[o]->dist_bits, (unsigned)synth[o]->dist_rate,
                        (unsigned)synth[o]->ks_index);
        }
    }
}

/* Console dump of AMY's own state for both riffs' synths and the global KS
 * settings, to compare against what the app believes it configured. Reads
 * without amy_queue_lock: holding it through a UART dump would stall the
 * render, and synth[] entries are never freed, so the worst a racing edit
 * does is print a torn value. */
void synth_ui_dev_riff_dump(void)
{
    bool tune; uint8_t stages; float stiff;
    float soft, hard, pick, shape, comb;
    ks_loop_get(&tune, &stages, &stiff);
    ks_excite_get(&soft, &hard, &pick, &shape, &comb);
    fprintf(stderr, "==== RIFF AMY DUMP ks: tune=%d stages=%u stiff=%g release=%d "
            "excite soft=%g hard=%g pick=%g shape=%g comb=%g ====\n",
            tune, (unsigned)stages, (double)stiff, ks_release_get(),
            (double)soft, (double)hard, (double)pick, (double)shape, (double)comb);
    int li = riff_first_melodic_layer();
    if (li >= 0) {
        uint8_t rows = sequencer_core_get_layer_tracks((uint8_t)li);
        fprintf(stderr, "-- B: L%d rows\n", li + 1);
        for (uint8_t r = 0; r < rows; r++)
            riff_dump_synth(sequencer_core_get_track_synth((uint8_t)li, r));
        uint8_t base = (uint8_t)(sequencer_core_get_track_synth((uint8_t)li, 0) + SEQ_TRACKS);
        fprintf(stderr, "-- A: strings\n");
        for (uint8_t s = 0; s < RIFF_STRINGS && base + s <= SEQ_MAX_SYNTH; s++)
            riff_dump_synth((uint8_t)(base + s));
    }
    fprintf(stderr, "==== RIFF AMY DUMP END ====\n");
}

#endif /* CONFIG_SYNTH_DEV_MENU */
