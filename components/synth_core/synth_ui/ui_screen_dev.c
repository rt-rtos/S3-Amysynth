#include "sdkconfig.h"
#if CONFIG_SYNTH_DEV_MENU

#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "seq_clamp.h"
#include "amy.h"
#include "esp_heap_caps.h"
#include "core_load.h"
#include "dropout_stats.h"
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  DEV menu — quarantine zone for temporary controls, diagnostics, readouts
 * ════════════════════════════════════════════════════════════════════════
 * Deliberately free of the UX constraints real screens carry: the ONLY design
 * goals are cheap authoring and trivial navigation (turn = move, click = act,
 * "< back" row = up). Controls graduate to real screens if they prove out.
 *
 *  Adding a control = one table row + (usually) a fmt/adjust pair:
 *    readout   { .label="X", .fmt=... }                    click: nothing
 *    toggle    { .label="X", .fmt=..., .fire=... }         click: fire
 *    value     { .label="X", .fmt=..., .adjust=... }       click: edit mode,
 *                                                          turn adjusts
 *    action    { .label="X", .fire=... }                   click: fire
 *    submenu   { .label="X", .sub=&page }                  click: enter
 *              (+ .fmt previews state on the row: "VALUE >")
 *  `arg` is passed to every callback so one handler serves N rows.
 *  Adding a submenu = one dev_item_t[] + one dev_page_t + one submenu row.
 *
 * DEV state is VOLATILE by design - no snapshot fields, no persistence.
 * (A control whose backing state persists owns that in its real module and
 * belongs on a real screen, e.g. the Layer menu's Unison page beside the
 * global backend knob kept here.) Everything here runs on the UI task. */

typedef struct dev_page dev_page_t;

typedef struct {
    const char *label;
    void (*fmt)(char *buf, size_t n, int arg);   /* value text; NULL = none  */
    void (*adjust)(int delta, int arg);          /* encoder while editing    */
    void (*fire)(int arg);                       /* one-shot on click        */
    const dev_page_t *sub;                       /* submenu target           */
    int arg;
} dev_item_t;

struct dev_page {
    const char       *title;
    const dev_item_t *items;
    uint8_t           count;
};

/* ── Controls ──────────────────────────────────────────────────────────── */

/* Unison backend (sequencer_core_set_unison_layout; layouts in seq_model.h):
 * one global knob, eng -> fan -> head, wrapping. A change rebuilds the
 * melodic layers whose rows it reshapes (notes stop). */
static void uni_layout_fmt(char *buf, size_t n, int arg)
{
    (void)arg;
    static const char *const names[VOICE_UNISON_LAYOUT_COUNT] = { "fan", "head", "eng" };
    uint8_t l = sequencer_core_get_unison_layout();
    snprintf(buf, n, "%s", names[(l < VOICE_UNISON_LAYOUT_COUNT) ? l : 0u]);
}

static void uni_layout_adjust(int delta, int arg)
{
    (void)arg;
    int l = ((int)sequencer_core_get_unison_layout() + delta) % (int)VOICE_UNISON_LAYOUT_COUNT;
    if (l < 0) l += (int)VOICE_UNISON_LAYOUT_COUNT;
    sequencer_core_set_unison_layout((uint8_t)l);
}

/* Karplus-Strong loop controls: Tune (fractional-period allpass, ks_loop_set),
 * Stages/Stiff (dispersion allpasses; Stiff steps a 1-2-5 ladder, 0 = none),
 * Release (ks_release_set: note-off starts the amp release on KS voices) and
 * Voices (sequencer_core_set_ks_voices, up to CONFIG_SEQ_KS_VOICES_MAX; a
 * change rebuilds the layers holding KS rows). Volatile like everything here. */
enum { KSL_TUNE, KSL_STAGES, KSL_STIFF, KSL_RELEASE, KSL_VOICES };

static const float s_ksl_stiff[] = {
    0.0f, 1e-5f, 2e-5f, 5e-5f, 1e-4f, 2e-4f, 5e-4f, 1e-3f, 2e-3f, 5e-3f,
};
static const char *const s_ksl_stiff_name[] = {
    "0", "1e-5", "2e-5", "5e-5", "1e-4", "2e-4", "5e-4", "1e-3", "2e-3", "5e-3",
};
#define KSL_STIFF_COUNT (int)(sizeof s_ksl_stiff / sizeof *s_ksl_stiff)

/* Ladder index nearest to B. */
static int ksl_stiff_index(float b)
{
    int best = 0;
    for (int i = 1; i < KSL_STIFF_COUNT; i++)
        if (fabsf(s_ksl_stiff[i] - b) < fabsf(s_ksl_stiff[best] - b)) best = i;
    return best;
}

static void ksl_fmt(char *buf, size_t n, int arg)
{
    bool tune; uint8_t stages; float stiff;
    ks_loop_get(&tune, &stages, &stiff);
    switch (arg) {
        case KSL_TUNE:   snprintf(buf, n, "%s", tune ? "on" : "off"); break;
        case KSL_STAGES: snprintf(buf, n, "%u", (unsigned)stages);    break;
        case KSL_STIFF:  snprintf(buf, n, "%s",
                                  s_ksl_stiff_name[ksl_stiff_index(stiff)]); break;
        case KSL_RELEASE: snprintf(buf, n, "%s", ks_release_get() ? "on" : "off"); break;
        case KSL_VOICES: snprintf(buf, n, "%u/%u",
                                  (unsigned)sequencer_core_get_ks_voices(),
                                  (unsigned)CONFIG_SEQ_KS_VOICES_MAX);  break;
        default:         buf[0] = '\0';                               break;
    }
}

static void ksl_fire(int arg)
{
    bool tune; uint8_t stages; float stiff;
    ks_loop_get(&tune, &stages, &stiff);
    if (arg == KSL_TUNE) ks_loop_set(!tune, stages, stiff);
    if (arg == KSL_RELEASE) ks_release_set(!ks_release_get());
}

static void ksl_adjust(int delta, int arg)
{
    if (arg == KSL_VOICES) {
        int v = (int)sequencer_core_get_ks_voices() + delta;
        sequencer_core_set_ks_voices((uint8_t)SEQ_CLAMP_INT(v, 1, CONFIG_SEQ_KS_VOICES_MAX));
        return;
    }
    bool tune; uint8_t stages; float stiff;
    ks_loop_get(&tune, &stages, &stiff);
    switch (arg) {
        case KSL_STAGES:
            stages = (uint8_t)SEQ_CLAMP_INT((int)stages + delta,
                                            0, KS_DISPERSION_MAX_STAGES);
            break;
        case KSL_STIFF:
            stiff = s_ksl_stiff[SEQ_CLAMP_INT(ksl_stiff_index(stiff) + delta,
                                              0, KSL_STIFF_COUNT - 1)];
            break;
        default:
            return;
    }
    ks_loop_set(tune, stages, stiff);
}

/* Row count of the active layer, melodic or drum
 * (sequencer_core_set_layer_tracks): a grow seeds the new row, a shrink drops
 * it from playback and keeps its data. Structural, so the change is queued to
 * synth_ui_task, which applies it and re-exports the layer for the grid. */
static void rows_fmt(char *buf, size_t n, int arg)
{
    (void)arg;
    uint8_t li = seq_state.active_layer_idx;
    snprintf(buf, n, "L%u %u", (unsigned)li + 1u,
             (unsigned)sequencer_core_get_layer_tracks(li));
}

static void rows_adjust(int delta, int arg)
{
    (void)arg;
    uint8_t li = seq_state.active_layer_idx;
    int r = (int)sequencer_core_get_layer_tracks(li) + delta;
    synth_ui_request_layer_tracks(li, (uint8_t)SEQ_CLAMP_INT(r, SEQ_TRACKS_DEFAULT,
                                                             SEQ_TRACKS));
}

/* KS riff A/B (ui_dev_riff.c): A plays six bare KS strings at the
 * "A duty" row's duty (0.50 or 0.37), B the same riff on the first melodic
 * layer's rows. */
static void riff_fmt(char *buf, size_t n, int arg)
{
    if (arg == 2) { snprintf(buf, n, "%.2f", (double)synth_ui_dev_riff_duty()); return; }
    if (arg == 3) { snprintf(buf, n, "log"); return; }
    snprintf(buf, n, "%s", synth_ui_dev_riff_busy() ? "..." : "play");
}

static void riff_adjust(int delta, int arg)
{
    (void)arg;
    if (delta) synth_ui_dev_riff_toggle_duty();
}

static void riff_fire(int arg)
{
    if (arg == 0)      synth_ui_dev_riff_play_a();
    else if (arg == 1) synth_ui_dev_riff_play_b();
    else               synth_ui_dev_riff_dump();
}

/* One-shot sequencer state dump to the console (seq_core_dump.c). */
static void seqdump_fire(int arg)
{
    (void)arg;
    sequencer_core_dump_state();
}

/* Readouts. */
static void oom_fmt(char *buf, size_t n, int arg)
{
    (void)arg;
    snprintf(buf, n, "%lu", (unsigned long)amy_get_oom_count());
}

static void heap_fmt(char *buf, size_t n, int arg)
{
    (void)arg;
    snprintf(buf, n, "%uK", (unsigned)(heap_caps_get_free_size(
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024u));
}

/* Per-core busy % as "c0/c1", read from the status-LED sampler's last result
 * (core_load_last(); "--" until it has published one, e.g. LED task absent). */
static void cpu_fmt(char *buf, size_t n, int arg)
{
    (void)arg;
    uint8_t busy[CORE_LOAD_NUM_CORES];
    if (core_load_last(busy))
        snprintf(buf, n, "%u/%u%%", (unsigned)busy[0], (unsigned)busy[1]);
    else
        snprintf(buf, n, "--");
}

/* Heap+CPU status bar: while enabled, replaces the bottom hint strip on EVERY
 * screen (including ones that normally hide the hint). Sampled from the UI
 * task's pre-gate service pass at HEAPBAR_PERIOD frames (20 Hz loop ->
 * 500 ms); the sampled text feeds the render gate so the bar refreshes on
 * change. The two DEV bars are mutually exclusive. Abbreviations:
 *   TF  = total free internal heap (MALLOC_CAP_INTERNAL|8BIT), K
 *   LB  = largest free block of the same heap, K
 *   CPU = core0/core1 busy %, from the status-LED sampler via
 *         core_load_last() (1 s cadence; omitted until first publish) */
#define HEAPBAR_PERIOD 10

static bool     s_heapbar_on = false;
static char     s_heapbar_text[48];
static uint32_t s_heapbar_sig;
static uint8_t  s_heapbar_tick;

static bool     s_dropbar_on = false;
static char     s_dropbar_text[48];
static uint32_t s_dropbar_sig;
static uint8_t  s_dropbar_tick;

static void heapbar_fmt(char *buf, size_t n, int arg)
{
    (void)arg;
    snprintf(buf, n, "%s", s_heapbar_on ? "ON" : "OFF");
}

static void heapbar_fire(int arg)
{
    (void)arg;
    s_heapbar_on   = !s_heapbar_on;
    s_heapbar_tick = 0;   /* sample on the next service pass */
    s_dropbar_on   = false;   /* the two bars share the hint strip */
}

void synth_ui_dev_heapbar_poll(void)
{
    if (!s_heapbar_on) return;
    if (s_heapbar_tick == 0) {
        size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t b = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                    MALLOC_CAP_8BIT);
        uint8_t busy[CORE_LOAD_NUM_CORES];
        char cpu[12];
        if (core_load_last(busy))
            snprintf(cpu, sizeof cpu, "%u/%u",
                     (unsigned)busy[0], (unsigned)busy[1]);
        else
            snprintf(cpu, sizeof cpu, "--");
        snprintf(s_heapbar_text, sizeof s_heapbar_text,
                 "TF:%u.%uK LB:%u.%uK CPU:%s",
                 (unsigned)(f / 1024u), (unsigned)((f % 1024u) * 10u / 1024u),
                 (unsigned)(b / 1024u), (unsigned)((b % 1024u) * 10u / 1024u),
                 cpu);
        /* Hash the text, not the raw sizes: sub-0.1K jitter then never forces
         * a physically identical redraw through the gate. */
        s_heapbar_sig = fnv1a_bytes(FNV1A_OFFSET, s_heapbar_text,
                                    strlen(s_heapbar_text));
    }
    s_heapbar_tick = (uint8_t)((s_heapbar_tick + 1) % HEAPBAR_PERIOD);
}

bool synth_ui_dev_heapbar_active(void)
{
    return s_heapbar_on;
}

const char *synth_ui_dev_heapbar_text(void)
{
    return s_heapbar_text;
}

uint32_t synth_ui_dev_heapbar_sig(void)
{
    return s_heapbar_on ? s_heapbar_sig : 0;
}

/* Dropout bar: the heap bar's mechanism over dropout_stats counters
 * (Z wire_zlp, U ring_underrun, O render_overrun, D ring_overrun). */

static void dropbar_fmt(char *buf, size_t n, int arg)
{
    (void)arg;
    snprintf(buf, n, "%s", s_dropbar_on ? "ON" : "OFF");
}

static void dropbar_fire(int arg)
{
    (void)arg;
    s_dropbar_on   = !s_dropbar_on;
    s_dropbar_tick = 0;   /* sample on the next service pass */
    s_heapbar_on   = false;   /* the two bars share the hint strip */
}

void synth_ui_dev_dropbar_poll(void)
{
    if (!s_dropbar_on) return;
    if (s_dropbar_tick == 0) {
        dropout_stats_t d;
        dropout_stats_get(&d);
        snprintf(s_dropbar_text, sizeof s_dropbar_text,
                 "Z:%lu U:%lu O:%lu D:%lu",
                 (unsigned long)d.wire_zlp,
                 (unsigned long)d.ring_underrun,
                 (unsigned long)d.render_overrun,
                 (unsigned long)d.ring_overrun);
        s_dropbar_sig = fnv1a_bytes(FNV1A_OFFSET, s_dropbar_text,
                                    strlen(s_dropbar_text));
    }
    s_dropbar_tick = (uint8_t)((s_dropbar_tick + 1) % HEAPBAR_PERIOD);
}

bool synth_ui_dev_dropbar_active(void)
{
    return s_dropbar_on;
}

const char *synth_ui_dev_dropbar_text(void)
{
    return s_dropbar_text;
}

uint32_t synth_ui_dev_dropbar_sig(void)
{
    return s_dropbar_on ? s_dropbar_sig : 0;
}

/* ── Pages (leaf pages first - root last, so rows can reference them) ──── */

static const dev_item_t s_uni_items[] = {
    { .label = "Backend", .fmt = uni_layout_fmt, .adjust = uni_layout_adjust },
};
static const dev_page_t s_page_uni = { "UNISON", s_uni_items,
                                       sizeof s_uni_items / sizeof *s_uni_items };

static const dev_item_t s_ksloop_items[] = {
    { .label = "Tune",   .fmt = ksl_fmt, .fire   = ksl_fire,   .arg = KSL_TUNE   },
    { .label = "Stages", .fmt = ksl_fmt, .adjust = ksl_adjust, .arg = KSL_STAGES },
    { .label = "Stiff",  .fmt = ksl_fmt, .adjust = ksl_adjust, .arg = KSL_STIFF  },
    { .label = "Release", .fmt = ksl_fmt, .fire  = ksl_fire,   .arg = KSL_RELEASE },
    { .label = "Voices", .fmt = ksl_fmt, .adjust = ksl_adjust, .arg = KSL_VOICES },
    { .label = "Rows",   .fmt = rows_fmt, .adjust = rows_adjust },
    { .label = "A duty", .fmt = riff_fmt, .adjust = riff_adjust, .arg = 2 },
    { .label = "Riff A", .fmt = riff_fmt, .fire = riff_fire,     .arg = 0 },
    { .label = "Riff B", .fmt = riff_fmt, .fire = riff_fire,     .arg = 1 },
    { .label = "Dump",   .fmt = riff_fmt, .fire = riff_fire,     .arg = 3 },
};
static const dev_page_t s_page_ksloop = { "KS LOOP", s_ksloop_items,
                                          sizeof s_ksloop_items / sizeof *s_ksloop_items };

static const dev_item_t s_root_items[] = {
    { .label = "Unison",      .sub = &s_page_uni },
    { .label = "KS loop",     .sub = &s_page_ksloop },
    { .label = "AMY OOM",     .fmt = oom_fmt },
    { .label = "Heap int",    .fmt = heap_fmt },
    { .label = "CPU c0/c1",   .fmt = cpu_fmt },
    { .label = "Status bar",  .fmt = heapbar_fmt, .fire = heapbar_fire },
    { .label = "Drop bar",    .fmt = dropbar_fmt, .fire = dropbar_fire },
    { .label = "Dump seq",    .fire = seqdump_fire },
};
static const dev_page_t s_page_root = { "", s_root_items,
                                        sizeof s_root_items / sizeof *s_root_items };

/* ── Navigation (row 0 of every page is the synthetic "< back" row) ────── */

#define DEV_STACK_MAX 4

typedef struct { const dev_page_t *pg; uint8_t cur; } dev_lvl_t;

static dev_lvl_t s_stack[DEV_STACK_MAX] = { { &s_page_root, 0 } };
static uint8_t   s_depth   = 0;
static bool      s_editing = false;

static uint8_t dev_row_count(const dev_page_t *pg)
{
    return (uint8_t)(pg->count + 1);   /* +1 for "< back" */
}

bool synth_ui_dev_is_active(void)
{
    return seq_state.ui_mode == UI_MODE_DEV && !seq_state.menu_open;
}

bool synth_ui_dev_handle_encoder(int delta)
{
    if (!synth_ui_dev_is_active()) return false;
    dev_lvl_t *lv = &s_stack[s_depth];
    if (s_editing && lv->cur > 0) {
        const dev_item_t *it = &lv->pg->items[lv->cur - 1];
        if (it->adjust) it->adjust(delta, it->arg);
    } else {
        int n = (int)dev_row_count(lv->pg);
        int c = (int)lv->cur + delta;
        c %= n;
        if (c < 0) c += n;
        lv->cur = (uint8_t)c;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_dev_handle_button(void)
{
    if (!synth_ui_dev_is_active()) return false;
    dev_lvl_t *lv = &s_stack[s_depth];
    if (s_editing) {
        s_editing = false;
    } else if (lv->cur == 0) {
        /* "< back": up one page, or from the root back to the main menu. */
        if (s_depth > 0) s_depth--;
        else             seq_state.menu_open = true;
    } else {
        const dev_item_t *it = &lv->pg->items[lv->cur - 1];
        if (it->sub && s_depth + 1 < DEV_STACK_MAX) {
            s_depth++;
            s_stack[s_depth] = (dev_lvl_t){ it->sub, 0 };
        } else if (it->fire) {
            it->fire(it->arg);
        } else if (it->adjust) {
            s_editing = true;
        }
        /* plain readout: click is a no-op */
    }
    s_force_redraw = true;
    return true;
}

/* ── View build + render-gate signature ────────────────────────────────── */

uint32_t dev_view_signature(dev_view_t *out)
{
    const dev_lvl_t  *lv = &s_stack[s_depth];
    const dev_page_t *pg = lv->pg;

    /* Zero first: rows are hashed full-width below, so bytes past each NUL
     * must be deterministic or the render gate never settles. */
    memset(out, 0, sizeof *out);
    out->title   = pg->title;
    out->cursor  = lv->cur;
    out->editing = s_editing;

    uint8_t n = dev_row_count(pg);
    if (n > DEV_VIEW_MAX_ROWS) n = DEV_VIEW_MAX_ROWS;
    out->count = n;

    snprintf(out->rows[0].label, DEV_LABEL_LEN, "%s",
             s_depth > 0 ? "< back" : "< menu");
    out->rows[0].value[0] = '\0';

    for (uint8_t i = 1; i < n; i++) {
        const dev_item_t *it = &pg->items[i - 1];
        snprintf(out->rows[i].label, DEV_LABEL_LEN, "%s", it->label);
        if (it->sub && it->fmt) {
            /* Submenu that also previews state: "<value> >". */
            char v[DEV_VALUE_LEN];
            it->fmt(v, sizeof v, it->arg);
            snprintf(out->rows[i].value, DEV_VALUE_LEN, "%.*s >",
                     DEV_VALUE_LEN - 3, v);
        }
        else if (it->sub) snprintf(out->rows[i].value, DEV_VALUE_LEN, ">");
        else if (it->fmt) it->fmt(out->rows[i].value, DEV_VALUE_LEN, it->arg);
        else              out->rows[i].value[0] = '\0';
    }

    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &s_depth,      sizeof s_depth);
    h = fnv1a_bytes(h, &out->cursor,  sizeof out->cursor);
    h = fnv1a_bytes(h, &out->editing, sizeof out->editing);
    h = fnv1a_bytes(h, &out->count,   sizeof out->count);
    for (uint8_t i = 0; i < n; i++) {
        h = fnv1a_bytes(h, out->rows[i].label, sizeof out->rows[i].label);
        h = fnv1a_bytes(h, out->rows[i].value, sizeof out->rows[i].value);
    }
    return h;
}

#endif /* CONFIG_SYNTH_DEV_MENU */
