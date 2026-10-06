#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "u8g2.h"
#include "display_seq.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef display_seq_state_t synth_ui_state_t;

/* Initialise the display, create the drum layer (index 0), and start
 * the FreeRTOS UI task. Must be called after amy_start(). */
void synth_ui_init(u8g2_t *u8g2);

/* Add a new sequencer layer (drum or melodic). Returns the layer index
 * or 0xFF if the layer table is full. Safe to call after init. */
uint8_t synth_ui_add_layer(seq_layer_type_t type, uint8_t num_steps);
void    synth_ui_request_add_layer(void);
void    synth_ui_request_delete_active_layer(void);
/* Resize layer li to 16 or 32 steps: core first, then the UI mirror; the
 * step cursor is clamped into the new range. Input/UI path only. */
bool    synth_ui_set_layer_steps(uint8_t li, uint8_t num_steps);

/* Advance the active layer displayed/edited on screen.
 * Resets the cursor to track 0, step 0. */
void synth_ui_cycle_active_layer(void);

/* Input dispatch ── called from encoder / button tasks. The user-facing
 * control scheme (buttons, chords, per-screen behavior) is CONTROLS.md; the
 * gesture dispatcher is main/input_dispatch.c. */
void synth_ui_handle_encoder(long delta);
void synth_ui_handle_button(void);
/* Toggle the grid step under the cursor. Returns true if a step was
 * toggled. */
bool synth_ui_toggle_step_at_cursor(void);
void synth_ui_toggle_playing(void);
void synth_ui_set_bpm(uint16_t bpm);
void synth_ui_adjust_track_note(int delta);
void synth_ui_cycle_melodic_patch(int delta);
/* Cycle the selected drum track's patch through the curated drum list. Active
 * layer must be a drum layer; otherwise a no-op. */
void synth_ui_cycle_drum_patch(int delta);
/* Shift+Turn on the SEQ screen: step the active melodic layer's FM algorithm
 * live (DX7 bank + FM presets; shows a banner, non-FM patches say NOT FM).
 * Core 0 input path only. */
void synth_ui_cycle_fm_algo(int delta);
void synth_ui_set_drum_select_mode(bool held);
void synth_ui_set_patch_select_mode(bool held);

/* ── Menu overlay ────────────────────────────────────────────────────────
 * A modal overlay above the active screen (below the graph editor); while
 * open it captures the encoder and encoder click. Opening reopens the page and
 * row the menu was closed on; the Projects page comes back disarmed (no
 * pending load/save confirm, no rename). */
void synth_ui_menu_toggle(void);
bool synth_ui_menu_is_active(void);
bool synth_ui_menu_handle_encoder(long delta); /* true if consumed */
bool synth_ui_menu_handle_button(void);        /* true if consumed */
/* Leave any sub-page for the main list, on the row the sub-page was entered
 * from; ends value editing. No-op (false) while the menu is closed or already
 * on the main list. UI input task only. */
bool synth_ui_menu_go_main(void);
/* Layer-page shortcut. Menu closed: open it on the Layer page - on the row it
 * was left on if the menu was closed on that page, else on the Gate row, with
 * the main list's cursor on the Layer row. Menu open on the Layer page: close
 * it. Menu open on any other page, or the graph editor showing: no-op. UI
 * input task only. */
void synth_ui_menu_toggle_layer_page(void);

/* Projects-page rename editor hooks (buttons: CONTROLS.md). They live on the
 * menu overlay, which composes the projects module's rename state with its own
 * page tracking, so _active() is authoritative (false unless the menu is open
 * on the projects page mid-rename) and button dispatch can gate on it with no
 * cross-module stale-flag cleanup. It also drives the naming-aware hint
 * labels. No-ops when CONFIG_SYNTH_PROJECT_STORE is off. */
bool synth_ui_menu_rename_active(void);
void synth_ui_menu_rename_save(void);
void synth_ui_menu_rename_discard(void);

/* True while the menu overlay is open on its Wireless page. The Wireless page
 * is an overlay, not a ui_mode: the mode underneath is whatever screen the
 * user came from, so the editors bind to the BLE MIDI live-play voice on this
 * predicate, tested before any ui_mode, and the editor open chord uses it to
 * allow the editor from a page at all. Stays true while an editor draws over
 * the overlay. Always false when CONFIG_SYNTH_WIRELESS is off. */
bool synth_ui_wireless_page_is_open(void);

/* ── Arp screen ──────────────────────────────────────────────────────────
 * Active when seq_state.ui_mode == UI_MODE_ARP and no overlay is up. */
bool synth_ui_arp_is_active(void);
void synth_ui_arp_handle_encoder(long delta);
void synth_ui_arp_handle_button(void);
/* Cycle the arp's own patch (hold+turn gesture on the arp screen). */
void synth_ui_arp_cycle_patch(int delta);
/* Live-play slot patch (Wireless menu page); defined under CONFIG_SYNTH_WIRELESS. */
void synth_ui_cycle_live_patch(int delta);

/* ── Drone screen ────────────────────────────────────────────────────────
 * Standalone "stutter house drone" synth (custompatches/drone_core). Active
 * when seq_state.ui_mode == UI_MODE_DRONE and no overlay is up. */
bool synth_ui_drone_is_active(void);
void synth_ui_drone_handle_encoder(long delta);
void synth_ui_drone_handle_button(void);
/* Cycle the drone's PATCH-mode preset (hold+turn gesture on the drone screen). */
void synth_ui_drone_cycle_patch(int delta);

/* ── Normal drone screen ─────────────────────────────────────────────────
 * Free-running drone (custompatches/drone_std_core). Active when
 * seq_state.ui_mode == UI_MODE_DRONE_STD and no overlay is up. */
bool synth_ui_drone_std_is_active(void);
void synth_ui_drone_std_handle_encoder(long delta);
void synth_ui_drone_std_handle_button(void);
void synth_ui_drone_std_cycle_patch(int delta);

/* Chord-progression screen — active when seq_state.ui_mode == UI_MODE_PROG and no
 * overlay (menu/graph) is up.  Returns false if not active (caller should fall through). */
bool synth_ui_prog_is_active(void);
bool synth_ui_prog_handle_encoder(int delta);
bool synth_ui_prog_handle_button(void);
bool synth_ui_prog_add_entry(void);
bool synth_ui_prog_delete_entry(void);

/* DEV menu screen (CONFIG_SYNTH_DEV_MENU) — temporary controls / diagnostics.
 * Active when seq_state.ui_mode == UI_MODE_DEV and no overlay is up. */
bool synth_ui_dev_handle_encoder(int delta);
bool synth_ui_dev_handle_button(void);

/* FM/ALGO operator editor for the live SEQ_PATCH_FM_CUSTOM voice (see
 * custompatches/fm_voice.h). Active when seq_state.ui_mode == UI_MODE_FM and
 * no overlay is up; the handle/toggle calls return false and do nothing
 * otherwise. Two pages (ui_screen_fm.c): page 0 the operator graph and panel,
 * page 1 the selected operator's frequency and envelope.
 * toggle_mute flips the selected operator's audition mute (op_mute),
 * toggle_page ends link mode and flips the page, page() reads it for the
 * hint strip. step_algorithm is SHIFT+turn: the ALG row's step, ignored
 * while linking.
 *
 * Link mode (page 0 only): link_button is the Button 1 press. Not linking,
 * it starts with the selected operator as the source and the link cursor on
 * its box; linking, it applies the click rule (CONTROLS.md) to the box under
 * the link cursor, which the encoder moves. link_end leaves it (Button 3,
 * encoder click, SHOULDER). link_active is false off the screen or off page
 * 0, so state left over from leaving the screen never counts; link_button
 * discards it before deciding to start.
 * All UI/input task. */
bool    synth_ui_fm_is_active(void);
bool    synth_ui_fm_handle_encoder(int delta);
bool    synth_ui_fm_handle_button(void);
bool    synth_ui_fm_toggle_mute(void);
bool    synth_ui_fm_toggle_page(void);
uint8_t synth_ui_fm_page(void);
bool    synth_ui_fm_step_algorithm(int delta);
bool    synth_ui_fm_link_button(void);
bool    synth_ui_fm_link_active(void);
void    synth_ui_fm_link_end(void);

/* Custom wavetable editor (CONFIG_SYNTH_CUSTOM_WT) over the builder's
 * parameters (custompatches/wt_builder.h). Active when seq_state.ui_mode ==
 * UI_MODE_WT and no overlay is up; the calls return false and do nothing
 * otherwise. One tab is focused: a keyframe (A, M or B) or the scan tab. On
 * a keyframe the cursor walks its SHP, WID, BRT, SYN, PK, then HRM (the harmonic count; turning up raises it)
 * (ui_screen_wt.c) and handle_button toggles adjusting. On the scan tab the
 * encoder steps the displayed frame 0..63 (view only: no parameter and no
 * note changes), handle_button flips between that frame's waveform and its
 * harmonics, and copy/reset do nothing. next_keyframe (SHOULDER)
 * cycles the focus A -> M -> B -> scan -> A and keeps the cursor stop, the
 * adjusting state and the scan frame. copy_keyframe (Button 1)
 * copies the focused keyframe to the next one (A -> M, M -> B, B -> A),
 * nothing on HRM; reset_keyframe (Button 2) resets a focused A or B to the
 * saw, sets a focused M to the halfway blend of A and B, or the harmonic count to its
 * default on HRM. on_range() is true while the cursor is on HRM and
 * keyframe() returns the focused tab (0..2, 3 = scan), both for the hint
 * strip. All
 * UI/input task. */
bool    synth_ui_wt_is_active(void);
bool    synth_ui_wt_handle_encoder(int delta);
bool    synth_ui_wt_handle_button(void);
bool    synth_ui_wt_next_keyframe(void);
bool    synth_ui_wt_copy_keyframe(void);
bool    synth_ui_wt_reset_keyframe(void);
bool    synth_ui_wt_on_harmonics(void);
uint8_t synth_ui_wt_keyframe(void);

/* Global-FX reassert after a patch load (every Juno patch ends with global
 * EQ/chorus commands) is declared in amy_fx.h - include that header. */

/* Accessors for the module-private UI state: seq_state is static in
 * synth_ui.c, so other modules read through these rather than the struct. */
uint16_t seq_get_bpm(void);
uint8_t  seq_get_active_layer_idx(void);

/* ── Graph pop-up integration ────────────────────────────────────────────────
 * Hooks for the reusable graph_popup widget, called from the gesture
 * dispatcher; the pop-up state and U8g2 plumbing stay inside the synth_ui
 * module. */

/* True while the graph pop-up overlay is open. */
bool synth_ui_graph_is_active(void);

/* Open the curve editor seeded from the current melodic ADSR envelope. */
void synth_ui_graph_open_envelope(void);

/* SHIFT+0 loop-bounce chord (the gesture dispatcher): a press starts a bounce
 * into the first empty slot, cancels one still waiting for its bar line, or
 * stops a running one on the next pattern-period boundary; a long press
 * discards a running one. */
void synth_ui_bounce_chord(bool long_press);

/* Route input to the pop-up while it is active. Each returns true if the
 * pop-up consumed the event (caller should then skip normal sequencer input).
 * synth_ui_graph_handle_button(is_long): is_long=true => long-press/cancel. */
bool synth_ui_graph_handle_encoder(long delta);
bool synth_ui_graph_handle_button(bool is_long);

/* Commit the current edits and close the editor. Distinct from
 * synth_ui_graph_handle_button(true), which discards. */
bool synth_ui_graph_close_commit(void);

/* Cycle the graph editor's MY_BUTTON_2 sub-modes: the encoder adjusts the
 * target's amplitude trim (0..1), the layer swing, or an envelope routing
 * depth (PIT/CUT/DRV/MIX) instead of moving ADSR points; which stops exist
 * for a target is decided in ui_editors.c. Committed on close (confirm),
 * reset on every open. */
void synth_ui_graph_toggle_amp_mode(void);

/* Flip the sign of the depth the target stop is editing, on either envelope
 * page. No-op with no stop up and at 0.0 depth. */
void synth_ui_graph_flip_depth_polarity(void);

/* Cycle the shown EG's curve type Normal->Linear->DX7->TrueExp (AMY eg_type
 * 0..3). Applies to AMY immediately, honoring the current apply scope. */
void synth_ui_graph_cycle_eg_type(void);

/* ── Filter editor (per-synth LPF/HPF/BPF/LPF24 curve editor) ─────────────── */
bool synth_ui_filter_is_active(void);
void synth_ui_filter_open(void);
bool synth_ui_filter_handle_encoder(long delta);
bool synth_ui_filter_handle_button(bool is_long);
bool synth_ui_filter_close_commit(void);

/* Toggle the filter on/off. No-op when closed. */
void synth_ui_filter_toggle_enabled(void);

/* ── LFO editor (per-track tempo-synced modulator) ─────────────────────────
 * The third tab in the ADSR -> Filter -> LFO -> DIST cycle (synth_ui_cycle_editor). */
bool synth_ui_lfo_is_active(void);
void synth_ui_lfo_open(void);
bool synth_ui_lfo_handle_encoder(long delta);
bool synth_ui_lfo_handle_button(bool is_long);
bool synth_ui_lfo_close_commit(void);

/* Flip the target checklist to the other tab. The panel shows five checkbox
 * rows, so the distortion targets live on a second tab. A cursor parked on a checkbox row of the tab
 * being hidden moves to the first row of the tab being shown; cursors on the
 * shared parameter rows are untouched. No-op when the editor is closed.
 * UI-task only. */
void synth_ui_lfo_toggle_target_tab(void);

/* ── Distortion editor (per-target waveshaper: CLIP / FOLD / CRUSH) ────────
 * The fourth tab in the same cycle. Type OFF is the bypass, so there is no
 * separate enable gesture. Applies to whatever the target's base osc is,
 * patch-backed or wave-backed alike. */
bool synth_ui_dist_is_active(void);
void synth_ui_dist_open(void);
bool synth_ui_dist_handle_encoder(long delta);
bool synth_ui_dist_handle_button(bool is_long);
bool synth_ui_dist_close_commit(void);

/* Flip the selected row between its own voice block and the layer's shared
 * one (block semantics: sequencer_core.h). The open ADSR/filter/LFO/DIST
 * editor commits its pending edits to the departing block, the engine
 * re-pushes the row, and the editor re-seeds from the block now selected.
 * Returns true when an editor over a melodic row was active; ARP/DRONE/LIVE
 * have no track scope and drum layers no layer block. */
bool synth_ui_toggle_editor_source(void);

/* Hand the open editor's tab (EG0 or EG1 page, filter, LFO, DIST) back to the
 * patch: the group's authored flag is cleared on the block the row reads, the
 * layer reloads so the patch's own values sound, and the editor re-seeds
 * showing the P badge. Same gating as the flip. */
bool synth_ui_editor_release_to_patch(void);

/* Cycle ADSR (EG0, EG1) -> Filter -> LFO -> DIST -> ADSR. Commits the
 * departing editor and opens the next. */
void synth_ui_cycle_editor(void);

/* ── Step Trig editor (per-step pitch / probability / ratchet / conditional) ─
 * Full-screen popup addressed by the sequencer grid's own cursor (active layer
 * / selected track / selected step). Opened and closed from the gesture
 * dispatcher's SHIFT+2 chord; input model in ui_screen_stepedit.c. */
bool synth_ui_stepedit_is_active(void);
void synth_ui_stepedit_open(void);
void synth_ui_stepedit_close(void);
bool synth_ui_stepedit_handle_encoder(long delta);
bool synth_ui_stepedit_handle_button(void);

/* ─── View precedence - the single source of truth ──────────────────────
 * synth_ui_active_view() is the ONLY place the "which screen/overlay is
 * showing" precedence lives. Every consumer (draw, hint strip, both gesture
 * dispatcher entry points, ui_view_table[]) resolves once and dispatches on
 * the result, so input and draw can never disagree.
 *
 * Order (high to low): FILTER > LFO > DIST > STEPEDIT > GRAPH > MENU >
 * mode-tail. The first six are the input-capturing overlays
 * (UI_VIEW_IS_OVERLAY); the mode-tail comes from seq_state.ui_mode when no
 * overlay is up. */
typedef enum {
    UI_VIEW_FILTER = 0,
    UI_VIEW_LFO,
    UI_VIEW_DIST,
    UI_VIEW_STEPEDIT,
    UI_VIEW_GRAPH,
    UI_VIEW_MENU,
    UI_VIEW_ARP,
    UI_VIEW_DRONE_VIS,
    UI_VIEW_DRONE,
    UI_VIEW_DRONE_STD,
    UI_VIEW_PROG,
    UI_VIEW_FM,
    UI_VIEW_DEV,
    UI_VIEW_WT,
    UI_VIEW_SEQ,
    UI_VIEW_COUNT
} ui_view_id_t;

/* True for the six leading overlays, which capture encoder/button input
 * outright ahead of any mode screen. */
#define UI_VIEW_IS_OVERLAY(v)  ((v) <= UI_VIEW_MENU)

ui_view_id_t synth_ui_active_view(void);

#ifdef __cplusplus
}
#endif
