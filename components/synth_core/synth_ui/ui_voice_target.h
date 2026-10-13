#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "seq_model.h"   /* seq_filter_t, voice_unison_t */

/* ─── Voice-target provider: the per-voice controls one Layer page entry edits
 *     (level, filter key tracking, mute, solo, gate, glide), behind one call
 *     per control so the page builds the same rows for a sequencer track, the
 *     arpeggiator and the BLE live-play voice.
 *
 * VT_MELODIC is the Layer page's track: seq_state.active_layer_idx and
 * seq_state.selected_track, read by the backend itself on every call. VT_ARP
 * is arp_core, VT_LIVE the live_play voice (CONFIG_SYNTH_WIRELESS; without it
 * VT_LIVE has no controls, its getters return neutral values and its setters
 * do nothing).
 *
 * Execution context: UI task only. Every setter pushes through the owning
 * backend's own setter (sequencer_core, arp_core, live_play) and inherits its
 * contract; nothing here touches AMY directly. A vt_has_*() of false means
 * the matching getter returns a neutral value and the setter is a no-op. ── */

typedef enum { VT_MELODIC = 0, VT_ARP, VT_LIVE } voice_target_kind_t;

/* Amp trim, 0..1. */
float    vt_get_level(voice_target_kind_t kind);
void     vt_set_level(voice_target_kind_t kind, float v);

/* The voice's filter. get returns false (and leaves *out alone) when there is
 * none to edit: VT_MELODIC on a non-melodic layer, VT_LIVE without wireless.
 * set stores and pushes it; a no-op in the same cases. */
bool     vt_get_filter(voice_target_kind_t kind, seq_filter_t *out);
void     vt_set_filter(voice_target_kind_t kind, const seq_filter_t *f);

/* User mute: the track's mute, or the arp's own (arp_set_muted). The live
 * voice has none. */
bool     vt_has_mute(voice_target_kind_t kind);
bool     vt_get_mute(voice_target_kind_t kind);
void     vt_set_mute(voice_target_kind_t kind, bool muted);

/* Solo-set membership: the track's solo, or the arp's
 * (sequencer_core_set_arp_solo). The live voice is never part of the set. */
bool     vt_has_solo(voice_target_kind_t kind);
bool     vt_get_solo(voice_target_kind_t kind);
void     vt_set_solo(voice_target_kind_t kind, bool solo);

/* Note gate, percent of a step. VT_MELODIC is the layer gate with its
 * multi-step values and SEQ_GATE_HOLD (sequencer_core_set_layer_gate_pct);
 * VT_ARP is 10..100 (arp_set_gate_pct clamps). The live voice has none. */
bool     vt_has_gate(voice_target_kind_t kind);
uint16_t vt_get_gate_pct(voice_target_kind_t kind);
void     vt_set_gate_pct(voice_target_kind_t kind, uint16_t pct);

/* Glide, ms, 0 = off. set clamps to the backend's ceiling
 * (SEQ_MELODIC_PORTAMENTO_MAX_MS, ARP_PORTAMENTO_MAX_MS,
 * LIVE_PLAY_GLIDE_MAX_MS). VT_MELODIC is the active layer's glide. */
bool     vt_has_glide(voice_target_kind_t kind);
uint16_t vt_get_glide_ms(voice_target_kind_t kind);
void     vt_set_glide_ms(voice_target_kind_t kind, uint16_t ms);

/* Unison spec of the voice (voice_unison_t, seq_model.h). VT_MELODIC is the
 * Layer page's track on a melodic layer and follows its patch scope on set:
 * TRACK scope writes the track (sequencer_core_set_track_unison), LAYER scope
 * every track (sequencer_core_set_layer_unison); get and max read the track.
 * VT_ARP is arp_core's spec (persisted), VT_LIVE live_play's (session-only).
 * max: the highest effective copy count the voice's patch allows, 0 = it
 * cannot carry unison. set clamps and stores; the backend rebuilds or pushes
 * live per its own contract. */
bool     vt_has_unison(voice_target_kind_t kind);
void     vt_get_unison(voice_target_kind_t kind, voice_unison_t *out);
uint8_t  vt_unison_max(voice_target_kind_t kind);
void     vt_set_unison(voice_target_kind_t kind, const voice_unison_t *u);

/* Page title for the entry: "ARP", "LIVE", or NULL for VT_MELODIC (the Layer
 * page keeps its own layer/track title). */
const char *vt_title(voice_target_kind_t kind);
