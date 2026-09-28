#pragma once

#include "seq_chords.h"   /* SEQ_CHORD_MAX_NOTES for the chord tag-space math */
#include "synth_slots.h"  /* the AMY synth slot map (single source of truth) */
#include "arp_core.h"     /* ARP_MAX_SLOTS / ARP_OCT_MAX size the arp tag block */

/* ── Kconfig defaults ────────────────────────────────────────────────────── */
#ifndef CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED
#define CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED 1
#endif
#ifndef CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE
#define CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE 60
#endif
#ifndef CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE
#define CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE 0
#endif
#ifndef CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS
#define CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS 1
#endif
#ifndef CONFIG_SEQ_MELODIC_GATE_NUMERATOR
#define CONFIG_SEQ_MELODIC_GATE_NUMERATOR 5
#endif
#ifndef CONFIG_SEQ_MELODIC_GATE_DENOMINATOR
#define CONFIG_SEQ_MELODIC_GATE_DENOMINATOR 6
#endif
#ifndef CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED
#define CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED 1
#endif
#ifndef CONFIG_SEQ_MELODIC_PATCH
#define CONFIG_SEQ_MELODIC_PATCH 138
#endif
#ifndef CONFIG_SEQ_DRUM_GATE_NUMERATOR
#define CONFIG_SEQ_DRUM_GATE_NUMERATOR 1
#endif
#ifndef CONFIG_SEQ_DRUM_GATE_DENOMINATOR
#define CONFIG_SEQ_DRUM_GATE_DENOMINATOR 2
#endif
#ifndef CONFIG_SEQ_ENV_DEBUG_DUMP
#define CONFIG_SEQ_ENV_DEBUG_DUMP 0
#endif

/* ── Timing ──────────────────────────────────────────────────────────── */
#define SEQ_TICKS_PER_STEP    (AMY_SEQUENCER_PPQ / 4)
/* Max swing: percent of one step by which odd 16ths are delayed. Mirrors
 * DRONE_SWING_MAX and must stay < 100 so a swung note-on never crosses into the
 * next step's slot. The Digitakt 50-80% scalar maps to 0-60 here. */
#define SEQ_SWING_MAX         66
/* Delay in ticks that swing_pct gives an odd step. Integer floor, so pct values
 * that land on the same tick play identically (8 distinct delays at 48 PPQ). */
#define SEQ_SWING_TICKS(pct)  (((uint32_t)SEQ_TICKS_PER_STEP * (uint32_t)(pct)) / 100u)
/* Smallest swing_pct whose delay is `ticks`: the inverse, for per-tick editors. */
#define SEQ_SWING_PCT_FOR_TICKS(ticks) \
    ((uint8_t)(((uint32_t)(ticks) * 100u + SEQ_TICKS_PER_STEP - 1u) / SEQ_TICKS_PER_STEP))
/* A musical bar = 16 steps. Fixed regardless of layer length so the bar
 * counter and repeat-rate are independent of which layers are active. */
#define SEQ_TICKS_PER_BAR     (16u * SEQ_TICKS_PER_STEP)
/* Per-step micro-timing range in sequencer ticks, folded in at the
 * step->absolute-tick conversion on both emit paths (sequencer_emit_step() and
 * trig_schedule_ratchets()). +-6 is half a step - a strong but musical
 * push/drag, safely below the smallest loop period. A chosen range, not a
 * spec; the step_nudge setter clamps to +-this. */
#define SEQ_STEP_NUDGE_MAX    6
/* Per-step velocity offset range in signed percentage points of full scale,
 * added after the track's amp_trim on both emit paths; the sum is clamped to
 * 0..1 there, so +-100 spans silence to full without the setter needing a
 * wider range. */
#define SEQ_STEP_VEL_ADJ_MAX  100
/* Ratchet taper range: percent of full velocity removed per sub-hit, so sub-hit
 * k scales by (1 - taper*k%). Positive decays toward the tail, negative ramps
 * up; +-100 lets the second sub-hit already reach silence or full. */
#define SEQ_STEP_TAPER_MAX    100
/* How far ahead of the sequencer clock sequencer_core_service_tick() evaluates
 * decorated steps, so an early nudge can be honoured: the step is detected
 * when the advanced clock crosses its grid boundary and its sub-hits are
 * scheduled at the absolute grid tick + 1 + swing + nudge. Detection lags the
 * boundary by r ticks (0 or 1 per block below ~230 BPM, 2 after a skipped
 * tick), so the earliest fire tick is now + LOOKAHEAD - r + 1 - NUDGE_MAX,
 * which stays >= now + 1 for r <= 2 - the same pump deadline the old
 * "fire at now + 1" gave. */
#define SEQ_TRIG_LOOKAHEAD_TICKS (SEQ_STEP_NUDGE_MAX + 2)
/* Tempo-synced LFO frequency ceilings. NATIVE keeps AMY-carrier LFOs
 * sub-audible - fast rates at high BPM would otherwise cross 20 Hz into
 * AM/sideband territory. SW also protects the 20 Hz software stepper: under ~4
 * steps per LFO cycle the stepped waveform degenerates (a sine at exactly 2
 * steps/cycle aliases to DC). Fast rates cap here rather than being hidden from
 * the pickers, so the selectable range stays uniform across surfaces. */
#define SEQ_LFO_NATIVE_MAX_HZ 20.0f
#define SEQ_LFO_SW_MAX_HZ     5.0f
/* Drum gate: fraction of a step the note is held before its note-off, i.e.
 * choke vs ring; a patch's own release tail still plays out afterwards.
 * Kconfig-tunable, default 1/2 step. NOTE: the default drum engine is PCM
 * (gamma banks) rather than tonal patches. */
#define SEQ_GATE_DRUM         ((SEQ_TICKS_PER_STEP * CONFIG_SEQ_DRUM_GATE_NUMERATOR) / CONFIG_SEQ_DRUM_GATE_DENOMINATOR)
/* Default per-layer melodic gate (the NoteFX GATE control) as a % of the step.
 * Derived from the same num/den fraction as SEQ_GATE_MELODIC, and defined from
 * the fraction rather than SEQ_TICKS_PER_STEP so it stays PPQ-independent and
 * usable in TUs that do not include amy.h (project_snapshot.c). Control spans
 * 10..100% (100% = legato). */
#if CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS
#define SEQ_GATE_MELODIC      ((SEQ_TICKS_PER_STEP * CONFIG_SEQ_MELODIC_GATE_NUMERATOR) / CONFIG_SEQ_MELODIC_GATE_DENOMINATOR)
#define SEQ_MELODIC_GATE_DEFAULT_PCT \
    ((100u * CONFIG_SEQ_MELODIC_GATE_NUMERATOR + CONFIG_SEQ_MELODIC_GATE_DENOMINATOR / 2u) \
     / CONFIG_SEQ_MELODIC_GATE_DENOMINATOR)
#else
#define SEQ_GATE_MELODIC              ((SEQ_TICKS_PER_STEP * 2) / 3)
#define SEQ_MELODIC_GATE_DEFAULT_PCT  67u   /* round(100 * 2/3) */
#endif
/* Melodic glide (NoteFX Glide) ceiling, ms. Matches ARP_PORTAMENTO_MAX_MS so
 * both glide controls share the same feel. */
#define SEQ_MELODIC_PORTAMENTO_MAX_MS  100u
#define SEQ_MIN_BPM           40
#define SEQ_MAX_BPM           300
/* SEQ_DEFAULT_BPM is declared in sequencer_core.h (shared with synth_ui). */

/* ── Drum layer slot ─────────────────────────────────────────────────────
 * Layer slot 0 is always the drum layer and is permanent; the rest are melodic
 * and deletable. An invariant, not a preference - several paths assume
 * s_layers[SEQ_DRUM_LAYER_IDX].type == SEQ_LAYER_DRUM. Deliberately NOT coupled
 * to the SEQ_LAYER_DRUM enum (also 0): one is a slot index, the other a type
 * tag, and separate constants avoid a silent alias if the enum is renumbered. */
#define SEQ_DRUM_LAYER_IDX    0u

/* ── Drum synth slots ────────────────────────────────────────────────────
 * Each drum track owns its own synth slot, like melodic rows. Fixed block
 * 6..10 (SEQ_TRACKS wide) in the static pool (synth_slots.h), below the
 * melodic base, so the melodic running allocator is untouched. */
#define SEQ_DRUM_VOICES       1  /* one pitch at a time per row, as melodic */
/* Drum tracks play real pitches, so they clamp to the same musical range as
 * melodic rows rather than a GM-drum note span. */
#define SEQ_MIDI_NOTE_MIN     24    /* C1 */
#define SEQ_MIDI_NOTE_MAX     96    /* C7 */

/* ── Melodic synth defaults ──────────────────────────────────────────── */
#define SEQ_MEL_PATCH         CONFIG_SEQ_MELODIC_PATCH
/* Wave-patch ID constants live in sequencer_core.h so arp_core and drone_core
 * can use them too. */
/* One AMY synth PER ROW; voice count is Kconfig-driven (see
 * CONFIG_SEQ_MEL_VOICES help for the osc-cost rationale). */
#define SEQ_MEL_VOICES        CONFIG_SEQ_MEL_VOICES
/* SEQ_MEL_SYNTH_BASE / SEQ_MAX_SYNTH come from synth_slots.h: the melodic
 * range is the open-ended arena above the static slot pool. */
#define SEQ_MEL_NOTE_MIN      24    /* C1 */
#define SEQ_MEL_NOTE_MAX      96    /* C7 */

/* ── Arpeggiator synth slot ──────────────────────────────────────────────
 * SEQ_ARP_SYNTH lives in synth_slots.h's static pool, below the melodic
 * base, so it never collides with a melodic layer's per-row block. */
#define SEQ_ARP_VOICES        4     /* allow note overlap at fast rates       */

/* One-shot preview fires this many ticks after an adjustment */
#define SEQ_PREVIEW_DELAY_TICKS 4

/* Per-track oscillator budget for string-patch loads: num_voices is clamped
 * so oscs_per_voice x voices <= this. 32 keeps the standard polyphony intact
 * (DX7 = 8 oscs/voice x 4 voices, Juno = 6 x 4) while the 25-osc/voice
 * built-in piano degrades to 1 voice/track instead of exhausting the 250-osc
 * pool and internal heap on a layer-wide load (4 tracks x 4 voices x 25 =
 * 400 oscs attempted; see seq_clamp_patch_voices in seq_core_synth.c). */
#define SEQ_TRACK_OSC_BUDGET 32

/* ── Arpeggiator tag space ───────────────────────────────────────────────
 * Arp tags sit just above the sequencer's tag space:
 *   step on/off : 0 .. MAX_LAYERS*SEQ_TRACKS*SEQ_MAX_STEPS*2 - 1   (0..1279)
 *   previews    : 1280 .. 1280 + MAX_LAYERS*SEQ_TRACKS*2 - 1       (..1319)
 * so the arp starts at 1320 and needs ARP_MAX_SLOTS*ARP_OCT_MAX*2 = 64 tags.
 *
 * AMY's sequencer_add_wire rejects `tag >= max_sequences`, so sequences[]
 * needs at least (highest_tag + 1) user-addressable entries. Keep main.c's
 * amy_cfg.max_sequencer_tags in sync. */
#define SEQ_STEP_TAG_COUNT    (MAX_LAYERS * SEQ_TRACKS * SEQ_MAX_STEPS * 2u) /* 1280 */
#define SEQ_PREVIEW_TAG_COUNT (MAX_LAYERS * SEQ_TRACKS * 2u)                 /* 40 */
#define SEQ_ARP_TAG_BASE      (SEQ_STEP_TAG_COUNT + SEQ_PREVIEW_TAG_COUNT)    /* 1320 */
#define SEQ_ARP_TAG_COUNT     (ARP_MAX_SLOTS * ARP_OCT_MAX * 2)  /* = 64 */
#define SEQ_ARP_TAG_MAX       (SEQ_ARP_TAG_BASE + SEQ_ARP_TAG_COUNT - 1)  /* 1383 */

/* ── Ratchet tag space ────────────────────────────────────────────────────
 * A decorated step never uses the plain per-step ON/OFF pair: instead
 * sequencer_core_service_tick() one-shot schedules up to SEQ_MAX_RATCHET
 * note-on/off pairs per firing, each on its own statically assigned tag so
 * sub-hits never overwrite each other. Sits just above the arp's tag space.
 * Same off-by-one rule: main.c's amy_cfg.max_sequencer_tags must stay
 * >= SEQ_RATCHET_TAG_MAX + 2. */
#define SEQ_RATCHET_TAG_BASE  (SEQ_ARP_TAG_MAX + 1u)                          /* 1384 */
#define SEQ_RATCHET_TAG_COUNT (MAX_LAYERS * SEQ_TRACKS * SEQ_MAX_RATCHET * 2) /* 160 */
#define SEQ_RATCHET_TAG_MAX   (SEQ_RATCHET_TAG_BASE + SEQ_RATCHET_TAG_COUNT - 1) /* 1543 */

/* ── Chord tag space ──────────────────────────────────────────────────────
 * Chord steps always take the decorated one-shot path (a chord sentinel in
 * step_note makes the step decorated, see seq_core_trig.c), so chord tones need
 * one-shot tags only, never a periodic per-step block. Tone 0 reuses the
 * ratchet pair; higher tones get a statically assigned pair per (layer, track,
 * ratchet-slot, tone), plus a second small block for the edit-preview path.
 * Each tag slot (sequence_info_t) is 16 B of internal DRAM in AMY, plus its
 * wire string while occupied, which is why the one-shot scheme (800 tags)
 * beat a periodic per-step chord block (~3.5 K tags). Same
 * off-by-one rule: main.c must stay >= SEQ_CHORD_PREVIEW_TAG_MAX + 2 - it
 * derives max_sequencer_tags from that expression rather than a literal,
 * because this ceiling moves with SEQ_CHORD_MAX_NOTES. */
#define SEQ_CHORD_TAG_BASE    (SEQ_RATCHET_TAG_MAX + 1u)                      /* 1544 */
#define SEQ_CHORD_TAG_COUNT   (MAX_LAYERS * SEQ_TRACKS * SEQ_MAX_RATCHET \
                               * (SEQ_CHORD_MAX_NOTES - 1) * 2)               /* 640 */
#define SEQ_CHORD_TAG_MAX     (SEQ_CHORD_TAG_BASE + SEQ_CHORD_TAG_COUNT - 1)  /* 2183 */
#define SEQ_CHORD_PREVIEW_TAG_BASE  (SEQ_CHORD_TAG_MAX + 1u)                  /* 2184 */
#define SEQ_CHORD_PREVIEW_TAG_COUNT (MAX_LAYERS * SEQ_TRACKS \
                                     * (SEQ_CHORD_MAX_NOTES - 1) * 2)         /* 160 */
#define SEQ_CHORD_PREVIEW_TAG_MAX   (SEQ_CHORD_PREVIEW_TAG_BASE + \
                                     SEQ_CHORD_PREVIEW_TAG_COUNT - 1)         /* 2343 */

/* ── Bounce clip start tags ──────────────────────────────────────────────
 * Two per clip slot (clip_player.c): an aux config entry that sets the
 * right-channel osc's start parameters, then the note-on that starts the clip
 * at its end tick or re-anchors it on a bar line. The aux tag is the lower
 * one so it fires first within the tick. Same off-by-one rule: main.c derives
 * max_sequencer_tags from this ceiling. */
#define SEQ_CLIP_TAG_BASE     (SEQ_CHORD_PREVIEW_TAG_MAX + 1u)                /* 2344 */
#define SEQ_CLIP_TAG_COUNT    (2u * CLIP_SLOT_COUNT)
#define SEQ_CLIP_TAG_MAX      (SEQ_CLIP_TAG_BASE + SEQ_CLIP_TAG_COUNT - 1)    /* 2347 */

/* ── Global chord progression ────────────────────────────────────────────── */
#define CHORD_PROG_MAX_ENTRIES 8
