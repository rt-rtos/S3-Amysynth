#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "synth_ui.h"          /* synth_ui_state_t, seq_layer_type_t */
#include "display_drone.h"     /* drone_view_t */
#include "display_prog.h"      /* prog_view_t */
#include "display_dev.h"       /* dev_view_t (CONFIG_SYNTH_DEV_MENU) */
#include "display_menu.h"      /* menu_view_t */
#include "display_arp.h"       /* arp_view_t */
#include "display_stepedit.h"  /* stepedit_view_t */
#include "display_fm.h"        /* fm_view_t (CONFIG_SYNTH_CUSTOM_FM) */
#include "display_wt.h"        /* wt_view_t (CONFIG_SYNTH_CUSTOM_WT) */
#include "u8g2.h"              /* u8g2_t */

/* ─── Cross-file shared state (owners noted; only these need extern) ──── */
/* UI state lives behind a pointer so CONFIG_SEQ_STATE_IN_PSRAM can place it
 * in PSRAM (static internal storage when off). The macro keeps every existing
 * seq_state.field / &seq_state.field expression source-compatible. Owner:
 * synth_ui_state.c. Valid from synth_ui_state_alloc() onward, which
 * synth_ui_init() calls first - nothing may touch seq_state before
 * synth_ui_init(). UI/input task context only - never ISR, never render. */
extern synth_ui_state_t *seq_state_ptr;
#define seq_state (*seq_state_ptr)
void synth_ui_state_alloc(void);
extern volatile bool     s_force_redraw;   /* owner: synth_ui_task.c */
extern uint8_t           s_graph_layer;    /* owner: ui_editors.c; task clamps it */
extern uint8_t           s_graph_track;    /* owner: ui_editors.c; task clamps it */
extern bool              s_filter_active;  /* owner: ui_editors.c; task reads for cascade */
extern bool              s_lfo_active;     /* owner: ui_editors.c; task reads for cascade */
extern bool              s_dist_active;    /* owner: ui_editors.c; task reads for cascade */
extern bool              s_drone_vis_open; /* owner: ui_screen_drone.c; task reads for V_DRONE_VIS */

/* ─── FNV-1a render-on-change (all view signature functions use this) ── */
#define FNV1A_OFFSET 2166136261u
#define FNV1A_PRIME  16777619u
[[gnu::const]] static inline uint32_t fnv1a_bytes(uint32_t h,
                                                    const void *data, size_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) { h ^= b[i]; h *= FNV1A_PRIME; }
    return h;
}

/* ─── Shared private helpers ─────────────────────────────────────────── */
void     ui_note_name(uint8_t midi_note, char buf[4]);
void     sync_layer_to_core(uint8_t li);

/* Re-sync the UI mirror (seq_state) from the core after a bulk out-of-band
 * change to layer topology/content (project load): copies every core layer's
 * persistable state into seq_state.layers[], resets transport and cursor to a
 * safe idle default and forces one redraw. Never touches the core.
 * Applier-task only (synth_ui_task). */
void     synth_ui_reload_mirror_from_core(void);

/* Re-export one layer into the UI mirror after the core changed its row count,
 * clamping the sequencer cursor onto a row that exists. Applier-task only
 * (synth_ui_task). */
void     synth_ui_reexport_layer(uint8_t li);

/* Queue a row-count change for layer li (sequencer_core_set_layer_tracks),
 * applied and re-exported by synth_ui_task on its next frame. Any Core-0
 * context. */
void     synth_ui_request_layer_tracks(uint8_t li, uint8_t num_tracks);

/* Layer swing editing, shared by the Layer page and the graph editor's SWG
 * stop (defined in ui_screen_layermenu.c). ui_swing_step moves swing_pct by
 * `dir` engine ticks and returns the smallest pct for the new tick, clamped to
 * 0..SEQ_SWING_MAX. ui_swing_format writes the long/short ratio of a 16th pair
 * ("54%", triplet "67T") into buf. Pure; any task. */
uint8_t  ui_swing_step(uint8_t swing_pct, int dir);
void     ui_swing_format(char *buf, size_t len, uint8_t swing_pct);

/* ─── View signatures (each defined in its screen/editor file) ──────────
 * CONTRACT: signature functions are side-effect-free. The redraw gate calls one
 * per frame and compares its hash against the previous frame's; any state
 * mutation belongs in the live-service hooks that run before the gate.
 * View-struct screens build the view once and return it through `out` alongside
 * the hash, so the draw switch reuses it instead of repeating the snprintf
 * build in the same frame. */
uint32_t seq_view_signature(void);
uint32_t graph_view_signature(void);
uint32_t filter_view_signature(void);
uint32_t lfo_view_signature(void);
uint32_t dist_view_signature(void);
uint32_t arp_view_signature(arp_view_t *out);
uint32_t menu_view_signature(menu_view_t *out);
uint32_t drone_view_signature(drone_view_t *out);
uint32_t drone_std_view_signature(drone_view_t *out);
uint32_t prog_view_signature(prog_view_t *out);
uint32_t stepedit_view_signature(stepedit_view_t *out);
uint32_t fm_view_signature(fm_view_t *out);
uint32_t wt_view_signature(wt_view_t *out);         /* CONFIG_SYNTH_CUSTOM_WT */
uint32_t dev_view_signature(dev_view_t *out);       /* CONFIG_SYNTH_DEV_MENU */
bool     synth_ui_dev_is_active(void);

/* ─── DEV status bars, heap and dropout (mechanism in ui_screen_dev.c;
 *     CONFIG_SYNTH_DEV_MENU) ───────────────────────────────────────────────
 * poll() samples (throttled); call it from the UI task's pre-gate service pass
 * only. active()/text()/sig() are side-effect-free; sig() is 0 while off,
 * text() is only meaningful after a poll() with the bar active. UI task
 * context only. */
void        synth_ui_dev_heapbar_poll(void);
bool        synth_ui_dev_heapbar_active(void);
const char *synth_ui_dev_heapbar_text(void);
uint32_t    synth_ui_dev_heapbar_sig(void);
void        synth_ui_dev_dropbar_poll(void);
bool        synth_ui_dev_dropbar_active(void);
const char *synth_ui_dev_dropbar_text(void);
uint32_t    synth_ui_dev_dropbar_sig(void);

/* ─── DEV KS riff A/B (ui_dev_riff.c; CONFIG_SYNTH_DEV_MENU) ────────────
 * play_a: six bare KS strings on six spare synths; play_b: the
 * same riff on the first melodic layer's rows with their live voice. Each
 * stops the transport and schedules the whole riff on AMY's clock; a press
 * while one is sounding is ignored. dump: AMY's own osc state for both riffs'
 * synths and the global KS settings, to the console. UI/input task context. */
void  synth_ui_dev_riff_play_a(void);
void  synth_ui_dev_riff_play_b(void);
void  synth_ui_dev_riff_dump(void);
bool  synth_ui_dev_riff_busy(void);
float synth_ui_dev_riff_duty(void);
void  synth_ui_dev_riff_toggle_duty(void);

/* ─── View descriptor table (draw + hint), defined in ui_view_resolve.c ──
 * One scratch union holds whichever view struct the active screen builds:
 * signature() fills it and returns the FNV hash, draw() reuses it. Indexed by
 * synth_ui_active_view() (synth_ui.h). */
typedef union {
    menu_view_t      menu;
    fm_view_t        fm;
    wt_view_t        wt;
    arp_view_t       arp;
    drone_view_t     drone;      /* DRONE and DRONE_VIS */
    prog_view_t      prog;
    stepedit_view_t  stepedit;
    dev_view_t       dev;
} ui_view_vw_t;

typedef struct {
    const char *name;
    uint32_t  (*signature)(ui_view_vw_t *vw);   /* builds vw, returns FNV hash */
    void      (*draw)(u8g2_t *g, ui_view_vw_t *vw);
    /* Button-hint labels. A NULL static label means "compute dynamically" via
     * the matching *_fn (b1_fn, b2_fn, b3_fn), for cells that depend on state
     * the view id does not carry. */
    const char *b1, *b2, *b3;
    const char *(*b1_fn)(void);
    const char *(*b2_fn)(void);
    /* Preferred X of the BLE badge (see display_badge.h); 0 = let the probe
     * choose. */
    uint8_t badge_x;
    /* Optional SHOULDER cell, appended as " LB:<label>" when it returns
     * non-NULL (LB = the left shoulder button, SHOULDER; RB = the right one,
     * SHIFT). NULL fn (the default for rows that omit it) = no cell. */
    const char *(*bs_fn)(void);
    /* Dynamic button 3 label for a NULL b3; rows that omit it leave it NULL. */
    const char *(*b3_fn)(void);
} ui_view_desc_t;

extern const ui_view_desc_t ui_view_table[UI_VIEW_COUNT];

/* GRAPH b2 hint (owner: ui_editors.c): "Amp" on EG0, "Env" on EG1. */
const char *synth_ui_graph_hint_b2(void);

/* ─── Build-view helpers called from synth_ui_task draw switch ────────── */
void     drone_build_view(drone_view_t *out);
void     drone_std_build_view(drone_view_t *out);
void     prog_build_view(prog_view_t *out);
void     menu_build_view(menu_view_t *out);
void     arp_build_view(arp_view_t *out);
void     stepedit_build_view(stepedit_view_t *out);
void     fm_build_view(fm_view_t *out);
void     wt_build_view(wt_view_t *out);

/* ─── FX hub: one dive row per AMY bus plus the settings that are not per-bus
 *     (item model in ui_screen_fxmenu.c; the page state and input routing
 *     live in ui_screen_menu.c) ─────────────────────────────────────────── */
const menu_item_view_t *fxhub_build_items(void);
uint8_t  fxhub_item_count(void);
bool     fxhub_item_is_bus(uint8_t idx, uint8_t *bus_out);  /* dive into a bus */
bool     fxhub_item_is_value(uint8_t idx);
bool     fxhub_item_is_back(uint8_t idx);
void     fxhub_edit_value(uint8_t idx, int delta);

/* ─── Per-bus FX page, reached from a bus row on the hub. The page is bound
 *     to one bus by fx_menu_set_bus() on the way in. ───────────────────── */
const menu_item_view_t *fx_menu_build_items(void);
uint8_t  fx_menu_item_count(void);
bool     fx_menu_item_is_value(uint8_t idx);
bool     fx_menu_item_is_back(uint8_t idx);
void     fx_menu_edit_value(uint8_t idx, int delta);
void     fx_menu_set_bus(uint8_t bus);
const char *fx_menu_title(void);     /* header-bar title for the bus page */
const char *menu_page_title(void);   /* header-bar title for the active page */
/* True when a SHOULDER press would leave a menu sub-page for the main list
 * (synth_ui_menu_go_main); drives the hint strip's LB cell. */
bool     menu_shoulder_goes_main(void);

/* ─── Layer page: everything scoped to the active layer - steps, swing, melodic
 *     patch scope, gate/glide/groove, the manual chord, and the per-track
 *     repeat/mute/solo block (item model in ui_screen_layermenu.c; page state
 *     and input routing live in ui_screen_menu.c). Reached from the `Layer >`
 *     dive row on the main list. The visible row list is dynamic (ClrSolo
 *     comes and goes with the global solo state, Frame with the target
 *     track's patch, the Unison/PCM dive rows with the layer type and drum
 *     engine), so the count is a call, not a
 *     constant, and the handlers clamp the shared menu cursor. ─────────── */
typedef enum {
    LM_STEPS = 0,
    LM_SWING,
    LM_PATCH_SCOPE,
    LM_GATE,
    LM_GLIDE,
    LM_GROOVE,
    LM_CHORD,
    LM_ROOT,
    LM_TYPE,
    LM_TRACK,
    LM_FOLLOW,
    LM_REPEAT,
    LM_MUTE,
    LM_SOLO,
    LM_CLRSOLO,
    LM_FRAME,       /* wavetable frame position, shown on a wavetable track */
    LM_UNISON,      /* dive row: Unison sub-page, melodic layers */
    LM_PCM,         /* dive row: PCM sub-page, drum layer on the PCM engine */
    LM_BACK,
    LM_COUNT
} layer_item_id_t;

const menu_item_view_t *layermenu_menu_build_items(void);
uint8_t  layermenu_menu_item_count(void);
bool     layermenu_menu_item_is_back(uint8_t idx);
bool     layermenu_menu_handle_click(uint8_t idx);
void     layermenu_menu_edit_value(uint8_t idx, int delta);
void     layermenu_menu_reset(void);
void     layermenu_menu_clamp_cursor(void);
/* Visible-list index of row `id` (layer_item_id_t), or 0xFF while that row is
 * not shown. */
uint8_t  layermenu_menu_row_index(uint8_t id);
/* The track the page targets (its Track row): Follow/Repeat/Mute/Solo and the
 * Unison/PCM sub-pages. */
uint8_t  layermenu_menu_track(void);
const char *layermenu_menu_title(void);

/* ─── Layer sub-pages, dived into from the Layer page (item model in
 *     ui_screen_layer_sub.c; page state and input routing in
 *     ui_screen_menu.c). Both edit the active layer at the Layer page's track
 *     (layermenu_menu_track). Unison: Count/Detune/Spread/Blend, following
 *     the layer's patch scope. PCM: the drum track's PCM playback mode. ─── */
const menu_item_view_t *layer_uni_menu_build_items(void);
uint8_t  layer_uni_menu_item_count(void);
bool     layer_uni_menu_item_is_back(uint8_t idx);
bool     layer_uni_menu_handle_click(uint8_t idx);
void     layer_uni_menu_edit_value(uint8_t idx, int delta);
const char *layer_uni_menu_title(void);
const menu_item_view_t *layer_pcm_menu_build_items(void);
uint8_t  layer_pcm_menu_item_count(void);
bool     layer_pcm_menu_item_is_back(uint8_t idx);
bool     layer_pcm_menu_handle_click(uint8_t idx);
void     layer_pcm_menu_edit_value(uint8_t idx, int delta);
const char *layer_pcm_menu_title(void);

/* ─── Projects storage page (item model in ui_screen_projects.c; page state
 *     and input routing live in ui_screen_menu.c). Declared unconditionally:
 *     the implementation compiles to nothing and these go unused when
 *     CONFIG_SYNTH_PROJECT_STORE is off, since every call site is guarded by
 *     that symbol. ──────────────────────────────────────────────────────── */
const menu_item_view_t *projects_menu_build_items(void);
uint8_t  projects_menu_item_count(void);
bool     projects_menu_item_is_back(uint8_t idx);
bool     projects_menu_item_is_value(uint8_t idx);
bool     projects_menu_handle_click(uint8_t idx);
void     projects_menu_edit_value(uint8_t idx, int delta);
void     projects_menu_reset(void);
void     projects_menu_service(void);   /* drains the deferred load/save */
/* Rename-editor primitives, composed by the menu overlay's public
 * synth_ui_menu_rename_* hooks (which add the page-state gating). */
bool     projects_menu_is_renaming(void);
void     projects_menu_rename_commit(void);
void     projects_menu_rename_cancel(void);

/* ─── Wireless page: BLE MIDI session control (item model in
 *     ui_screen_wireless.c; page state and input routing live in
 *     ui_screen_menu.c). Declared unconditionally like the Projects page -
 *     every call site is guarded by CONFIG_SYNTH_WIRELESS. ─────────────── */
const menu_item_view_t *wireless_menu_build_items(void);
uint8_t  wireless_menu_item_count(void);
bool     wireless_menu_item_is_back(uint8_t idx);
bool     wireless_menu_handle_click(uint8_t idx);
void     wireless_menu_edit_value(uint8_t idx, int delta);
void     wireless_menu_reset(void);
/* synth_ui_wireless_page_is_open() (public, synth_ui.h) lives in
 * ui_screen_menu.c alongside the page state; the editors and main.c's shift
 * chord bind the live-play voice on it instead of a ui_mode. */

/* ─── Chords page: chord-preset editor (item model in ui_screen_chords.c;
 *     page state and input routing in ui_screen_menu.c). Slot list + per-slot
 *     edit view; every edit commits through seq_chords_set and auditions on
 *     the selected melodic track. ───────────────────────────────────────── */
const menu_item_view_t *chords_menu_build_items(void);
uint8_t  chords_menu_item_count(void);
bool     chords_menu_item_is_back(uint8_t idx);
bool     chords_menu_handle_click(uint8_t idx);
void     chords_menu_edit_value(uint8_t idx, int delta);
void     chords_menu_reset(void);
const char *chords_menu_title(void);

/* ─── Bounce page: loop-bounce recorder and the clip players (item model in
 *     ui_screen_bounce.c; page state and input routing in ui_screen_menu.c).
 *     Bounce shape + Rec on top, three rows per clip slot below. ────────── */
const menu_item_view_t *bounce_menu_build_items(void);
uint8_t  bounce_menu_item_count(void);
bool     bounce_menu_item_is_back(uint8_t idx);
bool     bounce_menu_item_is_value(uint8_t idx);
bool     bounce_menu_handle_click(uint8_t idx);
void     bounce_menu_edit_value(uint8_t idx, int delta);
void     bounce_menu_reset(void);
const char *bounce_menu_title(void);
/* Redraw pump: the render task moves the bounce and slot states, so the page
 * polls them from synth_ui_task's loop. No-op unless the page is open. */
void     bounce_menu_service(void);
/* True while the overlay is showing the Bounce page (page state lives in
 * ui_screen_menu.c), which is what scopes the service above. */
bool     menu_bounce_page_open(void);

/* ─── Prog Gen page: the chord-progression generator (item model in
 *     ui_screen_proggen.c; page state and input routing in ui_screen_menu.c).
 *     Generator parameters on top, then Generate and a one-level Undo. ──── */
const menu_item_view_t *proggen_menu_build_items(void);
uint8_t  proggen_menu_item_count(void);
bool     proggen_menu_item_is_back(uint8_t idx);
bool     proggen_menu_item_is_value(uint8_t idx);
bool     proggen_menu_handle_click(uint8_t idx);
void     proggen_menu_edit_value(uint8_t idx, int delta);
void     proggen_menu_reset(void);
const char *proggen_menu_title(void);

/* Editor live-preview service: flushes any pending throttled apply (the graph
 * editor's amp trim, whose melodic apply re-emits the track's steps). Called
 * from synth_ui_task's 50 ms loop; no-op when nothing is pending. */
void     synth_ui_editors_live_service(void);

/* ─── Draw wrappers (encapsulate private s_fgraph/s_lfo_view/s_graph_popup) */
void     synth_ui_graph_view_draw(u8g2_t *u8g2);
void     synth_ui_filter_view_draw(u8g2_t *u8g2);
void     synth_ui_lfo_view_draw(u8g2_t *u8g2);
void     synth_ui_dist_view_draw(u8g2_t *u8g2);
