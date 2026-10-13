#pragma once

/* ── Mixer: a fader and a mute per sound source ──────────────────────────
 * Owns one position (0..100) and one soft mute per source: master, each
 * layer, each layer track, the arp, both drones, the live voice and each
 * clip slot. A source's gain is (pos / 100)^2; a track's gain is its
 * layer's times its own, 0 while either is muted; a drone's gain covers its
 * main and sub synths. The gain reaches AMY as the mixer factor of
 * amy_event.synth_level (synth_level.h), which scales a synth's audible
 * output after its voices: a muted source keeps playing silently, notes and
 * tails run on, and an unmute is seamless. Master is not a synth: its
 * position writes amy_fx_set_master_volume() directly, unity at 100.
 *
 * Contract:
 * - Execution context: UI task (Core 0) only. Sends go through
 *   synth_level.h / amy_fx.h (amy_helpers), never under amy_queue_lock;
 *   nothing here allocates.
 * - Setters only mark state; mixer_service() (the 50 ms frame) sends. The
 *   master position is the exception and is written at once.
 * - The mixer never calls a source's own level or mute setter (amp_trim,
 *   arp/drone/clip/live levels, sequencer_core_set_track_mute,
 *   arp_set_muted) and never writes an amp coefficient.
 * - A synth is only sent a level while its source is present
 *   (mixer_src_present()); a source that turns present is brought to its
 *   gain on the next frame. Present synths below unity are re-sent round
 *   robin, a few per frame, so a level reset by a patch load comes back
 *   within a few frames. While every source sits at 100 and unmuted,
 *   nothing is sent.
 * - Positions are clamped to 0..100 on every write path, mute bits to the
 *   source widths. */

#include <stdbool.h>
#include <stdint.h>
#include "seq_model.h"     /* MAX_LAYERS, SEQ_TRACKS */

typedef enum {
    MIX_SRC_MASTER = 0, MIX_SRC_LAYER, MIX_SRC_TRACK, MIX_SRC_ARP,
    MIX_SRC_DRONE_STD, MIX_SRC_DRONE_STUT, MIX_SRC_LIVE, MIX_SRC_CLIP,
} mix_src_kind_t;

/* a = layer index (LAYER, TRACK) or clip slot (CLIP); b = track (TRACK). */
typedef struct { uint8_t kind; uint8_t a; uint8_t b; } mix_src_t;

/* The sources without a layer index, in snapshot order. */
enum {
    MIX_MISC_ARP = 0, MIX_MISC_DRONE_STD, MIX_MISC_DRONE_STUT, MIX_MISC_LIVE,
    MIX_MISC_CLIP0, MIX_MISC_CLIP1,
    MIX_MISC_COUNT
};

#define MIXER_POS_MAX 100u

/* Persistent state, field for field the project section's layout. */
typedef struct {
    uint8_t layer_pos[MAX_LAYERS];
    uint8_t layer_mute;                         /* bit L */
    uint8_t track_pos[MAX_LAYERS][SEQ_TRACKS];
    uint8_t track_mute[MAX_LAYERS];             /* bit t */
    uint8_t misc_pos[MIX_MISC_COUNT];
    uint8_t misc_mute;                          /* bit MIX_MISC_* */
} mixer_snapshot_t;

/* Defaults (all 100, no mute); nothing sent. Once from synth_ui_init(),
 * after synth_level_init(). */
void    mixer_init(void);
/* All 100, no mute; every present source is re-sent on the next frame. */
void    mixer_reset_defaults(void);
bool    mixer_src_present(mix_src_t s);
/* MASTER: round(100 * sqrt(min(master volume, 1))). */
uint8_t mixer_get_pos(mix_src_t s);
/* Clamped to 0..100. MASTER writes amy_fx_set_master_volume() at once. */
void    mixer_set_pos(mix_src_t s, uint8_t pos);
bool    mixer_has_mute(mix_src_t s);   /* false for MASTER */
bool    mixer_get_mute(mix_src_t s);
void    mixer_set_mute(mix_src_t s, bool on);
/* The 50 ms frame: sends what the setters and hooks marked, brings sources
 * that turned present to their gain, re-sends a few sub-unity synths. Call
 * from ui_services() only. */
void    mixer_service(void);
/* Layer table hooks, called by the UI task right after the structural edit:
 * added resets idx to defaults; deleted shifts idx+1.. down one and resets
 * the vacated last entry. */
void    mixer_on_layer_added(uint8_t idx);
void    mixer_on_layer_deleted(uint8_t idx);
/* Codec. import clamps every field, resets layers at or past the live
 * layer count to defaults and re-sends every present source. */
void    mixer_export(mixer_snapshot_t *out);
void    mixer_import(const mixer_snapshot_t *in);
