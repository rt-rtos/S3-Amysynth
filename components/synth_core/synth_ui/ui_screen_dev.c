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
 * (Controls whose backing state persists, like the PCM mode, own that in
 * their real module.) Everything here runs on the UI task. */

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

/* PCM playback mode per drum track, targeting L1 (test surface; the core
 * setter no-ops gracefully when L1 is not a drum layer). */
static const char *PCM_MODE_NAMES[] = { "DFLT", "PLAY", "LOOP", "LOOPST", "FRVR" };
#define PCM_MODE_COUNT 5

static void pcm_mode_fmt(char *buf, size_t n, int arg)
{
    uint8_t m = sequencer_core_get_drum_pcm_mode(0, (uint8_t)arg);
    if (m >= PCM_MODE_COUNT) m = 0;
    snprintf(buf, n, "%s", PCM_MODE_NAMES[m]);
}

static void pcm_mode_adjust(int delta, int arg)
{
    int m = (int)sequencer_core_get_drum_pcm_mode(0, (uint8_t)arg) + delta;
    m %= PCM_MODE_COUNT;
    if (m < 0) m += PCM_MODE_COUNT;
    sequencer_core_set_drum_pcm_mode(0, (uint8_t)arg, (uint8_t)m);
}

/* Melodic unison prototype (sequencer_core_set_unison): pick a melodic
 * layer (L2-L4), then edit its spec. The field id rides `arg`; LAYER is
 * dev-local navigation state, the spec itself lives in the core. COUNT and
 * LAYOUT changes rebuild the layer's voices (notes stop); the rest push
 * live. */
enum { UNI_LAYER, UNI_COUNT, UNI_LAYOUT, UNI_DETUNE, UNI_SPREAD, UNI_BLEND };

static uint8_t s_uni_layer = 1;   /* layer INDEX 1..3 = UI L2..L4 */

static void uni_fmt(char *buf, size_t n, int arg)
{
    voice_unison_t u = sequencer_core_get_unison(s_uni_layer);
    switch (arg) {
        case UNI_LAYER:  snprintf(buf, n, "L%u", (unsigned)s_uni_layer + 1u); break;
        case UNI_COUNT:  snprintf(buf, n, "%u",   (unsigned)u.count);         break;
        case UNI_LAYOUT: {
            static const char *const names[VOICE_UNISON_LAYOUT_COUNT] =
                { "fan", "head", "eng" };
            uint8_t l = (u.layout < VOICE_UNISON_LAYOUT_COUNT) ? u.layout : 0u;
            snprintf(buf, n, "%s", names[l]);
            break;
        }
        case UNI_DETUNE: snprintf(buf, n, "%uc",  (unsigned)u.detune_cents);  break;
        case UNI_SPREAD: snprintf(buf, n, "%u%%", (unsigned)u.spread_pct);    break;
        case UNI_BLEND:  snprintf(buf, n, "%u%%", (unsigned)u.blend_pct);     break;
        default:         buf[0] = '\0';                                       break;
    }
}

static void uni_adjust(int delta, int arg)
{
    if (arg == UNI_LAYER) {
        int l = (int)s_uni_layer - 1 + delta;   /* wrap over indices 1..3 */
        l %= 3;
        if (l < 0) l += 3;
        s_uni_layer = (uint8_t)(l + 1);
        return;
    }
    voice_unison_t u = sequencer_core_get_unison(s_uni_layer);
    switch (arg) {
        case UNI_COUNT:
            u.count = (uint8_t)SEQ_CLAMP_INT((int)u.count + delta,
                                             1, (int)VOICE_UNISON_MAX_COPIES);
            break;
        case UNI_LAYOUT: {
            int l = ((int)u.layout + delta) % (int)VOICE_UNISON_LAYOUT_COUNT;
            if (l < 0) l += (int)VOICE_UNISON_LAYOUT_COUNT;
            u.layout = (uint8_t)l;           /* fan -> head -> eng, wrapping */
            break;
        }
        case UNI_DETUNE:
            u.detune_cents = (uint8_t)SEQ_CLAMP_INT((int)u.detune_cents + delta,
                                                    0, (int)VOICE_UNISON_MAX_DETUNE);
            break;
        case UNI_SPREAD:
            u.spread_pct = (uint8_t)SEQ_CLAMP_INT((int)u.spread_pct + delta, 0, 100);
            break;
        case UNI_BLEND:
            u.blend_pct = (uint8_t)SEQ_CLAMP_INT((int)u.blend_pct + delta, 0, 100);
            break;
        default:
            return;
    }
    sequencer_core_set_unison(s_uni_layer, &u);
}

/* Karplus-Strong loop allpass (ks_loop_set/ks_loop_get in oscillators.c):
 * Tune runs the stage that completes the fractional period, so a KS note
 * plays in tune instead of up to a semitone sharp; Stages/Coef add fixed
 * allpasses that push the upper modes off the harmonic series. Volatile
 * like everything else here - boot leaves Tune on and Stages 0. */
enum { KSL_TUNE, KSL_STAGES, KSL_COEF };

static void ksl_fmt(char *buf, size_t n, int arg)
{
    bool tune; uint8_t stages; float coef;
    ks_loop_get(&tune, &stages, &coef);
    switch (arg) {
        case KSL_TUNE:   snprintf(buf, n, "%s", tune ? "on" : "off"); break;
        case KSL_STAGES: snprintf(buf, n, "%u", (unsigned)stages);    break;
        case KSL_COEF:   snprintf(buf, n, "%+.2f", (double)coef);     break;
        default:         buf[0] = '\0';                               break;
    }
}

static void ksl_fire(int arg)
{
    bool tune; uint8_t stages; float coef;
    ks_loop_get(&tune, &stages, &coef);
    if (arg == KSL_TUNE) ks_loop_set(!tune, stages, coef);
}

static void ksl_adjust(int delta, int arg)
{
    bool tune; uint8_t stages; float coef;
    ks_loop_get(&tune, &stages, &coef);
    switch (arg) {
        case KSL_STAGES:
            stages = (uint8_t)SEQ_CLAMP_INT((int)stages + delta,
                                            0, KS_DISPERSION_MAX_STAGES);
            break;
        case KSL_COEF: {
            float c = coef + (float)delta * 0.05f;
            if (c < -0.9f) c = -0.9f;
            if (c >  0.9f) c =  0.9f;
            coef = c;
            break;
        }
        default:
            return;
    }
    ks_loop_set(tune, stages, coef);
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
 * screen (including ones that normally hide the hint) so internal-heap and
 * CPU headroom can be watched live under real load. Sampled from the UI
 * task's pre-gate service pass at HEAPBAR_PERIOD frames (20 Hz loop ->
 * 500 ms); the sampled text feeds the render gate so the bar refreshes on
 * change. Dev-facing abbreviations (deliberately terse, not end-user text):
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

/* Dropout status bar: same mechanism as the heap bar (claims the bottom hint
 * strip on every screen, sampled at the same throttle, text feeds the render
 * gate), showing the cumulative dropout counters so an audible gap can be
 * attributed live to its pipeline layer (see dropout_stats.h):
 *   Z = wire ZLPs (TinyUSB EP-IN FIFO dry - clock-beat fingerprint)
 *   U = ring underruns (render behind realtime, chunk zero-padded)
 *   O = render-clock overruns (blocks of realtime lost)
 *   D = ring-full drops (host stalled draining) */

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

#define PCM_TRACK_ROW(n) \
    { .label = "T" #n, .fmt = pcm_mode_fmt, .adjust = pcm_mode_adjust, .arg = (n) - 1 }

static const dev_item_t s_pcm_items[] = {
    PCM_TRACK_ROW(1),
    PCM_TRACK_ROW(2),
    PCM_TRACK_ROW(3),
    PCM_TRACK_ROW(4),
};
static const dev_page_t s_page_pcm = { "PCM MODE L1", s_pcm_items,
                                       sizeof s_pcm_items / sizeof *s_pcm_items };

static const dev_item_t s_uni_items[] = {
    { .label = "Layer",  .fmt = uni_fmt, .adjust = uni_adjust, .arg = UNI_LAYER  },
    { .label = "Count",  .fmt = uni_fmt, .adjust = uni_adjust, .arg = UNI_COUNT  },
    { .label = "Layout", .fmt = uni_fmt, .adjust = uni_adjust, .arg = UNI_LAYOUT },
    { .label = "Detune", .fmt = uni_fmt, .adjust = uni_adjust, .arg = UNI_DETUNE },
    { .label = "Spread", .fmt = uni_fmt, .adjust = uni_adjust, .arg = UNI_SPREAD },
    { .label = "Blend",  .fmt = uni_fmt, .adjust = uni_adjust, .arg = UNI_BLEND  },
};
static const dev_page_t s_page_uni = { "UNISON", s_uni_items,
                                       sizeof s_uni_items / sizeof *s_uni_items };

static const dev_item_t s_ksloop_items[] = {
    { .label = "Tune",   .fmt = ksl_fmt, .fire   = ksl_fire,   .arg = KSL_TUNE   },
    { .label = "Stages", .fmt = ksl_fmt, .adjust = ksl_adjust, .arg = KSL_STAGES },
    { .label = "Coef",   .fmt = ksl_fmt, .adjust = ksl_adjust, .arg = KSL_COEF   },
};
static const dev_page_t s_page_ksloop = { "KS LOOP", s_ksloop_items,
                                          sizeof s_ksloop_items / sizeof *s_ksloop_items };

static const dev_item_t s_root_items[] = {
    { .label = "PCM Mode L1", .sub = &s_page_pcm },
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
