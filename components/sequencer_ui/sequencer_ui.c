#include "sequencer_ui.h"
#include "priv_u8g2_seq.h"
#include "seq_clamp.h"
#include "sequencer_core.h"
#include "graph_popup.h"
#include "patch_names.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char *TAG = "sequencer_ui";

/* ── Graph pop-up integration (isolated, easily removable) ───────────────────
 * Everything between this block and the matching "graph pop-up: end" marker is
 * the demo wiring for the reusable graph_popup widget. Deleting this block plus
 * the single gated branch in sequencer_ui_task() and the three public
 * sequencer_ui_graph_* entry points fully restores the original behaviour.
 *
 * The melodic envelope is defined by compile-time CONFIG_SEQ_MELODIC_ENV_*
 * values (see sequencer_core.c). We mirror those defaults here to seed the
 * editor, then map them to/from the widget via the AMY adapter. */
#ifndef CONFIG_SEQ_MELODIC_ENV_ATTACK_MS
#define CONFIG_SEQ_MELODIC_ENV_ATTACK_MS 12
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_DECAY_MS
#define CONFIG_SEQ_MELODIC_ENV_DECAY_MS 220
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT
#define CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT 58
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_RELEASE_MS
#define CONFIG_SEQ_MELODIC_ENV_RELEASE_MS 280
#endif

static gpopup_t s_graph_popup;
static bool     s_graph_popup_inited = false;

/* Set by mode/layout transitions to force one redraw regardless of the
 * render-on-change signature (guards first-frame-after-transition staleness). */
static volatile bool s_force_redraw = true;

/* Which (layer,track) the open editor is bound to, captured at open time so the
 * write-back targets the same row even if the selection moves underneath. */
static uint8_t  s_graph_layer = 0;
static uint8_t  s_graph_track = 0;

/* ── Time-range mapping ──────────────────────────────────────────────────────
 * The graph X axis is normalised 0..1. We map it to absolute milliseconds with
 * a switchable full-width budget so the same 3-point editor serves both plucky
 * short notes and long pads.
 *   SHORT: 0..2000 ms, LINEAR  (fine control where short notes live)
 *   LONG : 0..15000 ms, LOG-SQUASHED (the long right-hand tail is compressed so
 *          the musically-interesting early portion keeps most of the pixels). */
#define GRAPH_RANGE_SHORT_MS 2000u
#define GRAPH_RANGE_LONG_MS  15000u
/* Curvature of the long-view squash: larger = more compression of the tail. */
#define GRAPH_LONG_SQUASH    9.0f

static bool s_graph_long_range = false;   /* false = SHORT, true = LONG */

/* Normalised X (0..1) -> milliseconds, range/curve aware. */
static uint32_t graph_x_to_ms(float x)
{
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    if (!s_graph_long_range) {
        return (uint32_t)(x * (float)GRAPH_RANGE_SHORT_MS + 0.5f);
    }
    /* Long view: expand the squashed display X back to a linear time fraction.
     * Display compresses with log1p(k*t)/log1p(k); invert it here. */
    float k = GRAPH_LONG_SQUASH;
    float t = (expf(x * logf(1.0f + k)) - 1.0f) / k;   /* 0..1 linear time */
    return (uint32_t)(t * (float)GRAPH_RANGE_LONG_MS + 0.5f);
}

/* Milliseconds -> normalised X (0..1), range/curve aware (inverse of above). */
static float graph_ms_to_x(uint32_t ms)
{
    if (!s_graph_long_range) {
        float x = (float)ms / (float)GRAPH_RANGE_SHORT_MS;
        return x > 1.0f ? 1.0f : x;
    }
    float k = GRAPH_LONG_SQUASH;
    float t = (float)ms / (float)GRAPH_RANGE_LONG_MS;   /* 0..1 linear time */
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    /* Compress: more pixels to small t, fewer to the long tail. */
    return logf(1.0f + k * t) / logf(1.0f + k);
}

/* Yellow top-bar height on the dual-colour panel: rows 0..15 render yellow and
 * are used as the editor's context bar, the plot fills rows 16..63. */
#define GRAPH_TOPBAR_H 16

static void graph_popup_ensure_init(void)
{
    if (s_graph_popup_inited) return;
    /* Full-screen plot under the yellow context bar (rows 16..63). */
    graph_popup_init(&s_graph_popup, 0, GRAPH_TOPBAR_H, 128,
                     (uint8_t)(64 - GRAPH_TOPBAR_H));
    graph_popup_set_style(&s_graph_popup, GPOPUP_STYLE_ADSR);
    s_graph_popup_inited = true;
}

/* Push the bottom-margin time tick positions for the active range. Mapped
 * through graph_ms_to_x() so spacing reflects the (non-linear in LONG) axis. */
static void graph_update_ticks(void)
{
    float xs[6];
    uint8_t n = 0;
    if (!s_graph_long_range) {
        /* SHORT (2s linear): 0, 0.5, 1, 1.5, 2 s. */
        static const uint32_t tms[] = { 0, 500, 1000, 1500, 2000 };
        for (uint8_t i = 0; i < 5; ++i) xs[n++] = graph_ms_to_x(tms[i]);
    } else {
        /* LONG (15s log-squashed): 0, 0.1, 0.5, 1, 5, 15 s. */
        static const uint32_t tms[] = { 0, 100, 500, 1000, 5000, 15000 };
        for (uint8_t i = 0; i < 6; ++i) xs[n++] = graph_ms_to_x(tms[i]);
    }
    graph_popup_set_ticks(&s_graph_popup, xs, n);
}

bool sequencer_ui_graph_is_active(void)
{
    return graph_popup_is_active(&s_graph_popup);
}

/* Seed the editor's 3 points from the stored envelope, applying the current
 * time-range mapping to the X (time) axis. Cumulative time is squashed segment
 * by segment so the curve shape matches what write-back will reconstruct. */
static void graph_seed_from_env(const seq_env_t *env)
{
    uint32_t cum_a = env->attack_ms;
    uint32_t cum_d = env->attack_ms + env->decay_ms;
    uint32_t cum_r = env->attack_ms + env->decay_ms + env->release_ms;

    gpopup_point_t pts[4];
    pts[0].x = 0.0f;                 pts[0].y = 0.0f;                       /* origin     */
    pts[1].x = graph_ms_to_x(cum_a); pts[1].y = 1.0f;                       /* attack peak*/
    pts[2].x = graph_ms_to_x(cum_d); pts[2].y = (float)env->sustain_pct / 100.0f; /* sustain */
    pts[3].x = graph_ms_to_x(cum_r); pts[3].y = 0.0f;                       /* release end*/
    graph_popup_set_points(&s_graph_popup, pts, 4);
}

/* Open the editor seeded from the SELECTED ROW's stored melodic envelope. */
void sequencer_ui_graph_open_envelope(void)
{
    graph_popup_ensure_init();

    /* Bind to the currently selected melodic row. If the active layer is not
     * melodic, fall back to the compile-time defaults purely for display. */
    s_graph_layer = seq_state.active_layer_idx;
    s_graph_track = seq_state.selected_track;

    seq_env_t env;
    if (!sequencer_core_get_melodic_envelope(s_graph_layer, s_graph_track, &env)) {
        env.attack_ms   = CONFIG_SEQ_MELODIC_ENV_ATTACK_MS;
        env.decay_ms    = CONFIG_SEQ_MELODIC_ENV_DECAY_MS;
        env.sustain_pct = CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT;
        env.release_ms  = CONFIG_SEQ_MELODIC_ENV_RELEASE_MS;
        env.eg_type     = CONFIG_SEQ_MELODIC_ENV_EG0_TYPE;
    }

    graph_seed_from_env(&env);
    graph_update_ticks();
    graph_popup_open(&s_graph_popup, GPOPUP_MODE_EDIT, NULL);
    graph_popup_set_style(&s_graph_popup, GPOPUP_STYLE_ADSR);
    s_force_redraw = true;
    ESP_LOGI(TAG, "graph editor open: L%u row%u range=%s",
             s_graph_layer, s_graph_track, s_graph_long_range ? "LONG" : "SHORT");
}

/* Read the edited points back, convert X->ms via the active range mapping, and
 * push the result to the bound row's envelope (which applies it to AMY). */
static void graph_commit_to_env(void)
{
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    if (n < 4) return;   /* expect origin + A + D + R */

    uint32_t cum_a = graph_x_to_ms(pts[1].x);
    uint32_t cum_d = graph_x_to_ms(pts[2].x);
    uint32_t cum_r = graph_x_to_ms(pts[3].x);

    /* Convert cumulative times back to per-segment durations (clamp monotonic). */
    uint32_t a = cum_a;
    uint32_t d = (cum_d > cum_a) ? (cum_d - cum_a) : 0;
    uint32_t r = (cum_r > cum_d) ? (cum_r - cum_d) : 0;

    seq_env_t env;
    if (!sequencer_core_get_melodic_envelope(s_graph_layer, s_graph_track, &env)) return;
    env.attack_ms   = a;
    env.decay_ms    = d;
    env.release_ms  = r;
    env.sustain_pct = (uint8_t)(pts[2].y * 100.0f + 0.5f);
    sequencer_core_set_melodic_envelope(s_graph_layer, s_graph_track, &env);
}

/* Toggle SHORT<->LONG time range while the editor is open and re-seed so the
 * displayed curve stays anchored to the same underlying envelope. */
bool sequencer_ui_graph_toggle_range(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;

    /* Convert the CURRENT on-screen points through the range change instead of
     * re-seeding from storage, so in-progress edits are preserved (no reset).
     * Snapshot the points, read each X back to ms under the OLD range, flip the
     * range, then re-map ms -> X under the NEW range. Y is range-independent. */
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);

    uint32_t ms[GPOPUP_MAX_POINTS];
    for (uint8_t i = 0; i < n; ++i) {
        ms[i] = graph_x_to_ms(pts[i].x);   /* OLD range mapping */
    }

    s_graph_long_range = !s_graph_long_range;

    for (uint8_t i = 0; i < n; ++i) {
        pts[i].x = graph_ms_to_x(ms[i]);   /* NEW range mapping */
    }
    /* Preserve cursor / edit state across the conversion. */
    uint8_t  saved_cursor   = s_graph_popup.cursor;
    bool     saved_editing  = s_graph_popup.editing_value;
    bool     saved_axis_y   = s_graph_popup.adjust_axis_y;
    graph_popup_set_points(&s_graph_popup, pts, n);
    s_graph_popup.cursor        = saved_cursor;
    s_graph_popup.editing_value = saved_editing;
    s_graph_popup.adjust_axis_y = saved_axis_y;

    graph_update_ticks();
    ESP_LOGI(TAG, "graph range -> %s", s_graph_long_range ? "LONG(15s)" : "SHORT(2s)");
    return true;
}

/* Route an encoder delta to the pop-up. Returns true if the pop-up consumed it
 * (host should then skip its normal sequencer routing). */
bool sequencer_ui_graph_handle_encoder(long delta)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;
    graph_popup_handle_encoder(&s_graph_popup, delta);
    return true;
}

/* Route a button event to the pop-up. is_long selects short vs long press.
 * Returns true if the pop-up consumed the event. On confirm/cancel the pop-up
 * is closed here. */
bool sequencer_ui_graph_handle_button(bool is_long)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;

    gpopup_result_t r = is_long
        ? graph_popup_handle_button_long(&s_graph_popup)
        : graph_popup_handle_button(&s_graph_popup);

    if (r == GPOPUP_RESULT_CONFIRMED) {
        ESP_LOGI(TAG, "graph popup confirmed -> writing envelope");
        graph_commit_to_env();
        graph_popup_close(&s_graph_popup);
    } else if (r == GPOPUP_RESULT_CANCELLED) {
        ESP_LOGI(TAG, "graph popup cancelled (envelope unchanged)");
        graph_popup_close(&s_graph_popup);
    }
    return true;
}

/* Flip the active adjust axis (vertical level <-> horizontal time). Only does
 * anything while the editor is open; returns true when it consumed the event so
 * the host knows the press belonged to the graph and not a background action. */
bool sequencer_ui_graph_toggle_axis(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;
    bool toggled = graph_popup_toggle_axis(&s_graph_popup);
    if (toggled) {
        ESP_LOGI(TAG, "graph popup axis -> %s",
                 graph_popup_axis_is_y(&s_graph_popup) ? "vertical" : "horizontal");
    }
    /* Consume the event whenever the graph is open, even if the toggle was a
     * no-op (e.g. VIEW mode), so the button never leaks to layer switching. */
    return true;
}
/* ── graph pop-up: end ──────────────────────────────────────────────────── */

#if !CONFIG_SEQ_PATCH_BROWSE_FULL_RANGE
/* Runtime patch cycling shortlist: intentionally small and musical.
 * Values map to AMY built-ins (Juno/DX7/piano). Used only when the browse mode
 * is "preselected"; the full-range mode walks 0..SEQ_PATCH_FULL_MAX instead. */
static const uint16_t s_melodic_patch_cycle[] = {
    138, /* DX7 E.PIANO 1 */
    135, /* DX7 PIANO 1 */
    141, /* DX7 SYN-LEAD 1 */
    151, /* DX7 FLUTE 1 */
    7,   /* Juno A18 Piano I */
    104, /* Juno B61 E. Piano with Tremolo */
    256, /* Built-in piano */
};
#define SEQ_RUNTIME_PATCH_COUNT ((int)(sizeof(s_melodic_patch_cycle) / sizeof(s_melodic_patch_cycle[0])))
#endif

/* Full-range browse covers every AMY built-in: Juno 0..127, DX7 128..255,
 * built-in piano 256 (matches the sequencer_core clamp upper bound). */
#define SEQ_PATCH_FULL_MAX 256

sequencer_ui_state_t seq_state = {
    /* layers[] is zero-initialized by C99 partial-init rules */
    .num_layers       = 0,
    .active_layer_idx = 0,
    .bpm              = 120,
    .current_pattern  = 1,
    .current_step     = 0,
    .playing          = true,
    .selected_track   = 0,
    .selected_step    = 0,
    .edit_mode        = true,
    .drum_select_mode = false,
};

static u8g2_t *s_u8g2 = NULL;

/* Mirror the UI's grid of active steps into the audio core so the core
 * schedules notes for every step the user has toggled on. Only "on" steps are
 * pushed; "off" steps are the core's default after a fresh layer add. */
static void sync_layer_to_core(uint8_t li)
{
    seq_layer_t *layer = &seq_state.layers[li];
    for (int t = 0; t < SEQ_TRACKS; t++) {
        for (int s = 0; s < layer->num_steps; s++) {
            if (layer->grid[t][s]) {
                sequencer_core_set_step(li, t, s, true);
            }
        }
    }
}

#if !CONFIG_SEQ_PATCH_BROWSE_FULL_RANGE
static int sequencer_patch_cycle_index_for(uint16_t patch)
{
    for (int i = 0; i < SEQ_RUNTIME_PATCH_COUNT; i++) {
        if (s_melodic_patch_cycle[i] == patch) {
            return i;
        }
    }
    return 0;
}
#endif

static void sequencer_ui_sync_melodic_patch_cache(void)
{
    uint16_t patch = sequencer_core_get_melodic_patch();
    for (uint8_t i = 0; i < seq_state.num_layers; i++) {
        if (seq_state.layers[i].type == SEQ_LAYER_MELODIC) {
            seq_state.layers[i].patch = patch;
        }
    }
}

/* ── Render-on-change support ────────────────────────────────────────────────
 * The OLED is full-buffer; a SendBuffer is ~20 ms of blocking I2C. At 20 Hz we
 * were redrawing unconditionally even when nothing changed. We compute a cheap
 * 32-bit FNV-1a signature of everything the frame depends on and only run the
 * Clear/draw/SendBuffer cycle when it differs from the last rendered one. */
#define FNV1A_OFFSET 2166136261u
#define FNV1A_PRIME  16777619u

static inline uint32_t fnv1a_bytes(uint32_t h, const void *data, size_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= b[i];
        h *= FNV1A_PRIME;
    }
    return h;
}

/* Signature of the sequencer view (everything priv_u8g2_seq_draw_frame reads). */
static uint32_t seq_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &seq_state.active_layer_idx, sizeof(seq_state.active_layer_idx));
    h = fnv1a_bytes(h, &seq_state.bpm, sizeof(seq_state.bpm));
    h = fnv1a_bytes(h, &seq_state.playing, sizeof(seq_state.playing));
    h = fnv1a_bytes(h, &seq_state.current_step, sizeof(seq_state.current_step));
    h = fnv1a_bytes(h, &seq_state.edit_mode, sizeof(seq_state.edit_mode));
    h = fnv1a_bytes(h, &seq_state.selected_track, sizeof(seq_state.selected_track));
    h = fnv1a_bytes(h, &seq_state.selected_step, sizeof(seq_state.selected_step));
    h = fnv1a_bytes(h, &seq_state.drum_select_mode, sizeof(seq_state.drum_select_mode));
    h = fnv1a_bytes(h, &seq_state.patch_select_mode, sizeof(seq_state.patch_select_mode));
    if (seq_state.num_layers > 0) {
        const seq_layer_t *L = &seq_state.layers[seq_state.active_layer_idx];
        h = fnv1a_bytes(h, &L->type, sizeof(L->type));
        h = fnv1a_bytes(h, &L->patch, sizeof(L->patch));
        h = fnv1a_bytes(h, &L->num_steps, sizeof(L->num_steps));
        h = fnv1a_bytes(h, &L->step_page, sizeof(L->step_page));
        h = fnv1a_bytes(h, L->track_base_note, sizeof(L->track_base_note));
        h = fnv1a_bytes(h, L->grid, sizeof(L->grid));
    }
    return h;
}

/* Signature of the graph editor (everything graph_popup_draw + the top bar read). */
static uint32_t graph_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &s_graph_popup.cursor, sizeof(s_graph_popup.cursor));
    h = fnv1a_bytes(h, &s_graph_popup.editing_value, sizeof(s_graph_popup.editing_value));
    h = fnv1a_bytes(h, &s_graph_popup.adjust_axis_y, sizeof(s_graph_popup.adjust_axis_y));
    h = fnv1a_bytes(h, &s_graph_long_range, sizeof(s_graph_long_range));
    h = fnv1a_bytes(h, &s_graph_layer, sizeof(s_graph_layer));
    h = fnv1a_bytes(h, &s_graph_track, sizeof(s_graph_track));
    h = fnv1a_bytes(h, s_graph_popup.points,
                    s_graph_popup.num_points * sizeof(gpopup_point_t));
    return h;
}

/* Draw the yellow context top bar (rows 0..15) for the envelope editor. */
static void graph_draw_topbar(u8g2_t *u8g2)
{
    char buf[24];

    /* Left: which row is being edited. */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    snprintf(buf, sizeof(buf), "L%u T%u ENV", s_graph_layer, s_graph_track);
    u8g2_DrawStr(u8g2, 2, 8, buf);

    /* Right: SHORT/LONG range flag. */
    const char *rng = s_graph_long_range ? "L" : "S";
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    uint8_t rw = (uint8_t)u8g2_GetStrWidth(u8g2, rng);
    u8g2_DrawStr(u8g2, (uint8_t)(128 - rw - 2), 8, rng);

    /* Middle: live readout of the selected point's real value (ms / %). */
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    uint8_t c = s_graph_popup.cursor;
    if (n >= 4 && c >= 1 && c <= 3) {
        uint32_t cum_a = graph_x_to_ms(pts[1].x);
        uint32_t cum_d = graph_x_to_ms(pts[2].x);
        uint32_t cum_r = graph_x_to_ms(pts[3].x);
        if (c == 1) {
            snprintf(buf, sizeof(buf), "A %lums", (unsigned long)cum_a);
        } else if (c == 2) {
            uint32_t d = (cum_d > cum_a) ? (cum_d - cum_a) : 0;
            snprintf(buf, sizeof(buf), "D %lums S %u%%",
                     (unsigned long)d, (unsigned)(pts[2].y * 100.0f + 0.5f));
        } else {
            uint32_t r = (cum_r > cum_d) ? (cum_r - cum_d) : 0;
            snprintf(buf, sizeof(buf), "R %lums", (unsigned long)r);
        }
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, buf);
        /* Centre-ish, between the left label (~x=56) and the range flag. */
        int mx = 60 + (int)((128 - 60 - rw - 4 - tw) / 2);
        if (mx < 60) mx = 60;
        u8g2_DrawStr(u8g2, (uint8_t)mx, 8, buf);
    }

    /* Divider at the yellow/blue boundary. */
    u8g2_DrawHLine(u8g2, 0, GRAPH_TOPBAR_H - 1, 128);
}

static void sequencer_ui_task(void *pvParameters)
{
    (void)pvParameters;
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t delay = pdMS_TO_TICKS(50); /* 20 Hz */
    uint32_t last_sig = 0;
    bool     last_was_graph = false;
    for (;;) {
        seq_state.current_step =
            sequencer_core_get_current_step(seq_state.active_layer_idx);
        if (s_u8g2) {
            bool graph = graph_popup_is_active(&s_graph_popup);
            uint32_t sig = graph ? graph_view_signature() : seq_view_signature();
            bool force = s_force_redraw || (graph != last_was_graph);

            if (force || sig != last_sig) {
                if (graph) {
                    /* Full-screen envelope editor: yellow context bar (rows
                     * 0..15) + the ADSR plot below. The widget owns the plot;
                     * the bar and buffer flush are owned here. */
                    u8g2_ClearBuffer(s_u8g2);
                    u8g2_SetDrawColor(s_u8g2, 1);
                    graph_draw_topbar(s_u8g2);
                    graph_popup_draw(s_u8g2, &s_graph_popup);
                    u8g2_SendBuffer(s_u8g2);
                } else {
                    priv_u8g2_seq_draw_frame(s_u8g2, &seq_state);
                }
                last_sig = sig;
                last_was_graph = graph;
                s_force_redraw = false;
            }
        }
        vTaskDelayUntil(&last_wake_time, delay);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void sequencer_ui_init(u8g2_t *u8g2)
{
    s_u8g2 = u8g2;
    seq_state.playing = true;

    sequencer_core_init();

    /* Add drum layer (index 0). */
    sequencer_ui_add_layer(SEQ_LAYER_DRUM, SEQ_STEPS);

    /* Default pattern: 4-on-the-floor hi-hat + backbeat kick. */
    seq_layer_t *drum = &seq_state.layers[0];
    drum->grid[0][0] = drum->grid[0][4] =
    drum->grid[0][8] = drum->grid[0][12] = true;
    drum->grid[1][4] = drum->grid[1][12]  = true;

    sync_layer_to_core(0);
    sequencer_core_set_playing(true);

    /* Pin to Core 0: the OLED refresh does blocking I2C and is not latency
     * critical, so keep it off Core 1 where the AMY DSP now runs. */
    xTaskCreatePinnedToCore(sequencer_ui_task, "seq_ui", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "Sequencer UI + Core initialized");
}

uint8_t sequencer_ui_add_layer(seq_layer_type_t type, uint8_t num_steps)
{
    uint8_t li = sequencer_core_add_layer(type, num_steps);
    if (li == 0xFF) return 0xFF;

    seq_layer_t *layer = &seq_state.layers[li];
    memset(layer, 0, sizeof(seq_layer_t));
    layer->type       = type;
    layer->num_steps  = (num_steps == SEQ_MAX_STEPS) ? SEQ_MAX_STEPS : SEQ_STEPS;
    layer->num_tracks = SEQ_TRACKS;
    layer->step_page  = 0;

    if (type == SEQ_LAYER_MELODIC) {
        layer->patch = sequencer_core_get_melodic_patch();
        /* Default: Cmaj7 voicing — C4 E4 G4 B4 */
        static const uint8_t mel_notes[SEQ_TRACKS] = {60, 64, 67, 71};
        for (int t = 0; t < SEQ_TRACKS; t++) {
            layer->track_base_note[t] = mel_notes[t];
            for (int s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = mel_notes[t];
            }
        }
    } else {
        static const uint8_t drum_notes[SEQ_TRACKS] = {42, 35, 38, 56};
        for (int t = 0; t < SEQ_TRACKS; t++) {
            layer->track_base_note[t] = drum_notes[t];
            for (int s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = drum_notes[t];
            }
        }
    }

    seq_state.num_layers = li + 1;
    ESP_LOGI(TAG, "UI layer %d added (type=%d steps=%d)",
             li, type, layer->num_steps);
    return li;
}

void sequencer_ui_cycle_active_layer(void)
{
    if (seq_state.num_layers <= 1) return;
    seq_state.active_layer_idx =
        (uint8_t)((seq_state.active_layer_idx + 1) % seq_state.num_layers);
    seq_state.selected_track = 0;
    seq_state.selected_step  = 0;
    seq_state.edit_mode      = true;
    ESP_LOGI(TAG, "Active layer -> %d (%s)",
             seq_state.active_layer_idx,
             seq_state.layers[seq_state.active_layer_idx].type == SEQ_LAYER_DRUM
             ? "drum" : "melodic");
}

/* Moves the step cursor by `delta` while in edit mode, or nudges BPM otherwise.
 * The cursor walks the current track's steps; running off either end wraps to
 * the adjacent track (and wraps track index too), so a long turn scans the
 * whole grid track-by-track. */
void sequencer_ui_handle_encoder(long delta)
{
    if (delta == 0) return;

    if (seq_state.edit_mode) {
        uint8_t li        = seq_state.active_layer_idx;
        uint8_t num_steps = seq_state.layers[li].num_steps;
        int new_step      = (int)seq_state.selected_step + (int)delta;

        if (new_step < 0) {
            /* Walked off the start: jump to the last step of the previous track. */
            new_step = (int)num_steps - 1;
            seq_state.selected_track =
                (uint8_t)((seq_state.selected_track + SEQ_TRACKS - 1) % SEQ_TRACKS);
        } else if (new_step >= (int)num_steps) {
            /* Walked off the end: jump to the first step of the next track. */
            new_step = 0;
            seq_state.selected_track =
                (uint8_t)((seq_state.selected_track + 1) % SEQ_TRACKS);
        }
        seq_state.selected_step = (uint8_t)new_step;

        /* 32-step layers display 16 steps per page; keep the cursor visible by
         * selecting the page (0 or 1) that contains the new step. */
        if (num_steps == SEQ_MAX_STEPS) {
            seq_state.layers[li].step_page = (uint8_t)(new_step / 16);
        }
    } else {
        sequencer_ui_set_bpm((uint16_t)((int)seq_state.bpm + (int)delta));
    }
}

/* Encoder push: in edit mode toggles the step under the cursor on/off (and
 * mirrors that to the core); otherwise it acts as a play/pause toggle. */
void sequencer_ui_handle_button(void)
{
    if (seq_state.edit_mode) {
        uint8_t li = seq_state.active_layer_idx;
        uint8_t t  = seq_state.selected_track;
        uint8_t s  = seq_state.selected_step;
        seq_state.layers[li].grid[t][s] = !seq_state.layers[li].grid[t][s];
        sequencer_core_set_step(li, t, s, seq_state.layers[li].grid[t][s]);
    } else {
        seq_state.playing = !seq_state.playing;
        sequencer_core_set_playing(seq_state.playing);
    }
}

void sequencer_ui_toggle_playing(void)
{
    seq_state.playing = !seq_state.playing;
    sequencer_core_set_playing(seq_state.playing);
    ESP_LOGI(TAG, "Playback %s", seq_state.playing ? "started" : "stopped");
}

void sequencer_ui_set_bpm(uint16_t bpm)
{
    bpm = SEQ_CLAMP_U16(bpm, 40, 300);
    seq_state.bpm = bpm;
    sequencer_core_set_bpm(bpm);
}

/* Transposes the selected track's note by `delta` semitones. We read/write the
 * *source* note (the user's raw choice) so repeated nudges accumulate cleanly;
 * the core may quantize it, so we read back the resolved note for display. */
void sequencer_ui_adjust_track_note(int delta)
{
    uint8_t li    = seq_state.active_layer_idx;
    uint8_t track = seq_state.selected_track;
    uint8_t note  = sequencer_core_get_track_source_note(li, track);
    uint8_t new_note = SEQ_CLAMP_U8(note + delta, 0, 127);
    sequencer_core_set_track_midi_note(li, track, new_note);
    /* Keep display in sync with resolved note after core clamp/quantize. */
    seq_state.layers[li].track_base_note[track] =
        sequencer_core_get_track_midi_note(li, track);
}

void sequencer_ui_set_drum_select_mode(bool held)
{
    seq_state.drum_select_mode = held;
}

void sequencer_ui_set_patch_select_mode(bool held)
{
    seq_state.patch_select_mode = held;
}

void sequencer_ui_cycle_melodic_patch(int delta)
{
    if (delta == 0) return;

    uint8_t li = seq_state.active_layer_idx;
    if (li >= seq_state.num_layers) return;
    if (seq_state.layers[li].type != SEQ_LAYER_MELODIC) return;

    /* One patch per detent regardless of how many sub-steps the encoder
     * reported: patch select is a discrete chooser, not a value knob. Multi-tick
     * deltas (jitter, fast turns) otherwise skipped entries and looked erratic. */
    int dir = (delta > 0) ? 1 : -1;

    uint16_t current = sequencer_core_get_melodic_patch();
    uint16_t next;

#if CONFIG_SEQ_PATCH_BROWSE_FULL_RANGE
    /* Walk the full 0..SEQ_PATCH_FULL_MAX range, wrapping at the ends. */
    int n = SEQ_PATCH_FULL_MAX + 1;          /* inclusive count */
    int cur = (int)current;
    if (cur < 0 || cur > SEQ_PATCH_FULL_MAX) cur = 0;  /* off-range -> start */
    int idx = (cur + dir + n) % n;
    next = (uint16_t)idx;
#else
    /* Walk the curated shortlist in order, wrapping at the ends. */
    int n = SEQ_RUNTIME_PATCH_COUNT;
    int idx = sequencer_patch_cycle_index_for(current);
    int ni = (idx + dir + n) % n;
    next = s_melodic_patch_cycle[ni];
#endif

    sequencer_core_set_melodic_patch(next);
    sequencer_ui_sync_melodic_patch_cache();

    uint16_t applied = sequencer_core_get_melodic_patch();
    const char *name = patch_name_for(applied);
    if (name) {
        ESP_LOGI(TAG, "melodic patch cycle -> %u (%s)", (unsigned)applied, name);
    } else {
        ESP_LOGI(TAG, "melodic patch cycle -> %u", (unsigned)applied);
    }
}


