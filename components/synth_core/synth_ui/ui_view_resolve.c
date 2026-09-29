#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "custompatches/drone_core.h"
#include "sdkconfig.h"

/* ════════════════════════════════════════════════════════════════════════
 *  View resolver + the descriptor table over it
 * ════════════════════════════════════════════════════════════════════════
 * Precedence is documented at synth_ui_active_view() in synth_ui.h. The
 * STEPEDIT branch is only correct because synth_ui_stepedit_is_active()
 * self-gates to the sequencer screen. */

ui_view_id_t synth_ui_active_view(void)
{
    if (s_filter_active)                return UI_VIEW_FILTER;
    if (s_lfo_active)                   return UI_VIEW_LFO;
    if (s_dist_active)                  return UI_VIEW_DIST;
    if (synth_ui_stepedit_is_active())  return UI_VIEW_STEPEDIT; /* self-gated */
    if (synth_ui_graph_is_active())     return UI_VIEW_GRAPH;
    if (seq_state.menu_open)            return UI_VIEW_MENU;
    switch (seq_state.ui_mode) {
        case UI_MODE_ARP:       return UI_VIEW_ARP;
        case UI_MODE_DRONE:     return s_drone_vis_open ? UI_VIEW_DRONE_VIS : UI_VIEW_DRONE;
        case UI_MODE_DRONE_STD: return UI_VIEW_DRONE_STD;
        case UI_MODE_PROG:      return UI_VIEW_PROG;
#if CONFIG_SYNTH_CUSTOM_FM
        case UI_MODE_FM:        return UI_VIEW_FM;
#endif
#if CONFIG_SYNTH_DEV_MENU
        case UI_MODE_DEV:       return UI_VIEW_DEV;
#endif
        default:                return UI_VIEW_SEQ;
    }
}

/* ─── Signature thunks (build vw once, return the FNV render-gate hash) ─── */
static uint32_t sig_filter(ui_view_vw_t *vw)    { (void)vw; return filter_view_signature(); }
static uint32_t sig_lfo(ui_view_vw_t *vw)       { (void)vw; return lfo_view_signature(); }
static uint32_t sig_dist(ui_view_vw_t *vw)      { (void)vw; return dist_view_signature(); }
static uint32_t sig_stepedit(ui_view_vw_t *vw)  { return stepedit_view_signature(&vw->stepedit); }
static uint32_t sig_graph(ui_view_vw_t *vw)     { (void)vw; return graph_view_signature(); }
static uint32_t sig_menu(ui_view_vw_t *vw)      { return menu_view_signature(&vw->menu); }
static uint32_t sig_arp(ui_view_vw_t *vw)       { return arp_view_signature(&vw->arp); }
static uint32_t sig_drone(ui_view_vw_t *vw)     { return drone_view_signature(&vw->drone); }
static uint32_t sig_drone_std(ui_view_vw_t *vw) { return drone_std_view_signature(&vw->drone); }
static uint32_t sig_prog(ui_view_vw_t *vw)      { return prog_view_signature(&vw->prog); }
static uint32_t sig_seq(ui_view_vw_t *vw)       { (void)vw; return seq_view_signature(); }

/* ─── Draw thunks (reuse the vw the signature already built) ───────────── */
static void draw_filter(u8g2_t *g, ui_view_vw_t *vw)    { (void)vw; synth_ui_filter_view_draw(g); }
static void draw_lfo(u8g2_t *g, ui_view_vw_t *vw)       { (void)vw; synth_ui_lfo_view_draw(g); }
static void draw_dist(u8g2_t *g, ui_view_vw_t *vw)      { (void)vw; synth_ui_dist_view_draw(g); }
static void draw_stepedit(u8g2_t *g, ui_view_vw_t *vw)  { display_stepedit_draw_frame(g, &vw->stepedit); }
static void draw_graph(u8g2_t *g, ui_view_vw_t *vw)     { (void)vw; synth_ui_graph_view_draw(g); }
static void draw_menu(u8g2_t *g, ui_view_vw_t *vw)      { display_menu_draw_frame_titled(g, menu_page_title(), &vw->menu); }
static void draw_arp(u8g2_t *g, ui_view_vw_t *vw)       { display_arp_draw_frame(g, &vw->arp); }
static void draw_drone(u8g2_t *g, ui_view_vw_t *vw)     { display_drone_draw_frame_titled(g, "STUTTER", &vw->drone); }
static void draw_drone_std(u8g2_t *g, ui_view_vw_t *vw) { display_drone_draw_frame_titled(g, "DRONE", &vw->drone); }
static void draw_prog(u8g2_t *g, ui_view_vw_t *vw)      { display_prog_draw_frame(g, &vw->prog); }
static void draw_seq(u8g2_t *g, ui_view_vw_t *vw)       { (void)vw; display_seq_draw_frame(g, &seq_state, seq_get_bpm()); }

/* DRONE_VIS composites the sweep/amp visualiser from the live drone getters
 * rather than a prebuilt view struct; the signature still uses drone_view for
 * render-gating. */
static void draw_drone_vis(u8g2_t *g, ui_view_vw_t *vw)
{
    (void)vw;
    const float SWEEP_MIN = 100.0f, SWEEP_MAX = 8000.0f;
    float span = SWEEP_MAX - SWEEP_MIN;
    float c = drone_get_amp_peak();
    float m = drone_get_amp_duck();
    float fl, cl;
    drone_get_amp_levels_norm(&fl, &cl);
    drone_vis_t dvis = {
        .sweep_lo_norm  = (drone_get_sweep_lo() - SWEEP_MIN) / span,
        .sweep_hi_norm  = (drone_get_sweep_hi() - SWEEP_MIN) / span,
        .amp_const      = c,
        .amp_mod        = m,
        .amp_floor_norm = fl,
        .amp_ceil_norm  = cl,
        .resonance      = drone_get_resonance(),
        .rate_idx       = (uint8_t)drone_get_rate(),
        .sweep_bars     = drone_get_sweep_bars(),
        .pattern_mask   = (uint8_t)0xFF, /* filled below */
        .gate_len       = drone_get_gate_len(),
        .wave_mode      = (drone_get_source() == DRONE_SRC_WAVE),
    };
    static const uint8_t pat_masks[] = { 0xFF, 0x55, 0xAA, 0x5B, 0x51 };
    drone_pattern_t pat = drone_get_pattern();
    if ((size_t)pat < sizeof(pat_masks))
        dvis.pattern_mask = pat_masks[pat];
    display_drone_vis_draw(g, &dvis);
}

#if CONFIG_SYNTH_CUSTOM_FM
static uint32_t sig_fm(ui_view_vw_t *vw)  { return fm_view_signature(&vw->fm); }
static void     draw_fm(u8g2_t *g, ui_view_vw_t *vw) { display_fm_draw_frame(g, &vw->fm); }
#endif

#if CONFIG_SYNTH_DEV_MENU
static uint32_t sig_dev(ui_view_vw_t *vw)  { return dev_view_signature(&vw->dev); }
static void     draw_dev(u8g2_t *g, ui_view_vw_t *vw) { display_dev_draw_frame(g, &vw->dev); }
#endif

/* ─── Dynamic hint labels (state the view id does not carry) ─────────────
 * SHIFT gestures are not shown on the 3-button strip. */
/* MENU b1/b2: Save/Discard while a project name is being edited, the only menu
 * sub-state that gives buttons 1/2 a job; otherwise "-", since the menu owns
 * the encoder and the patch/pitch holds have nothing to turn.
 * synth_ui_menu_rename_active() is false when the project store is off. */
static const char *hint_menu_b1(void)
{
    return synth_ui_menu_rename_active() ? "Save" : "-";
}
static const char *hint_menu_b2(void)
{
    return synth_ui_menu_rename_active() ? "Disc" : "-";
}
/* MENU SHOULDER: back to the main list, shown only on a sub-page. */
static const char *hint_menu_bs(void)
{
    return menu_shoulder_goes_main() ? "Main" : NULL;
}

/* ─── The table: one row per view, indexed by ui_view_id_t ─────────────── */
/* Preferred x of the BLE badge per view; 0 lets display_badge_draw() choose.
 * The badge never draws over lit pixels, so these only need to be close. */
const ui_view_desc_t ui_view_table[UI_VIEW_COUNT] = {
    [UI_VIEW_FILTER]    = { "FILTER", sig_filter,    draw_filter,    "On/Off", "-",     "Next",  NULL,               NULL,                   52 },
    [UI_VIEW_LFO]       = { "LFO",    sig_lfo,       draw_lfo,       "-",      "-",     "Next",  NULL,               NULL,                   68 },
    [UI_VIEW_DIST]      = { "DIST",   sig_dist,      draw_dist,      "-",      "-",     "Next",  NULL,               NULL,                    0 },
    [UI_VIEW_STEPEDIT]  = { "STEP",   sig_stepedit,  draw_stepedit,  "-",      "-",     "Close", NULL,               NULL,                    0 },
    [UI_VIEW_GRAPH]     = { "GRAPH",  sig_graph,     draw_graph,     "Type",   NULL,    "Next",  NULL,               synth_ui_graph_hint_b2, 52 },
    [UI_VIEW_MENU]      = { "MENU",   sig_menu,      draw_menu,      NULL,     NULL,    "Menu",  hint_menu_b1,       hint_menu_b2,           52, hint_menu_bs },
    [UI_VIEW_ARP]       = { "ARP",    sig_arp,       draw_arp,       "Patch",  "-",     "Menu",  NULL,               NULL,                   52 },
    [UI_VIEW_DRONE_VIS] = { "DRONEV", sig_drone,     draw_drone_vis, "Patch",  "-",     "Menu",  NULL,               NULL,                   58 },
    [UI_VIEW_DRONE]     = { "DRONE",  sig_drone,     draw_drone,     "Patch",  "-",     "Menu",  NULL,               NULL,                   52 },
    [UI_VIEW_DRONE_STD] = { "DRONST", sig_drone_std, draw_drone_std, "Patch",  "-",     "Menu",  NULL,               NULL,                   52 },
    [UI_VIEW_PROG]      = { "PROG",   sig_prog,      draw_prog,      "Del",    "+Add",  "Menu",  NULL,               NULL,                   30 },
#if CONFIG_SYNTH_CUSTOM_FM
    [UI_VIEW_FM]        = { "FM",     sig_fm,        draw_fm,        "Patch",  "-",     "Menu",  NULL,               NULL,                   64 },
#else
    [UI_VIEW_FM]        = { "FM",     NULL,          NULL,           "Patch",  "-",     "Menu",  NULL,               NULL,                   64 },
#endif
#if CONFIG_SYNTH_DEV_MENU
    [UI_VIEW_DEV]       = { "DEV",    sig_dev,       draw_dev,       "-",      "-",     "Menu",  NULL,               NULL,                   30 },
#else
    [UI_VIEW_DEV]       = { "DEV",    NULL,          NULL,           "-",      "-",     "Menu",  NULL,               NULL,                   30 },
#endif
    [UI_VIEW_SEQ]       = { "SEQ",    sig_seq,       draw_seq,       "Patch",  "Pitch", "Menu",  NULL,               NULL,                   44 },
};
