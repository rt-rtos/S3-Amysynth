#include "synth_ui/synth_ui_internal.h"
#include "amy_fx.h"
#include "fx_bus.h"
#include "display_dist.h"   /* DIST_DRIVE/BITS/RATE range + mix step */
#include "seq_clamp.h"
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════════
 *  FX HUB + PER-BUS FX PAGE
 * ════════════════════════════════════════════════════════════════════════
 * Two item models for the FX side of the menu overlay; page state and input
 * routing live in ui_screen_menu.c.
 *
 * The HUB is one row per AMY bus (dive rows, showing whether the bus carries
 * anything) plus the one setting that is not per-bus: the Preset FX guard.
 *
 * The BUS page is the old global-FX model, now bound to one bus: three-band
 * EQ, echo/chorus/reverb with their extended params, distortion (fx_state_t),
 * plus the group's Split toggle and the bus trim. fx_menu_set_bus() picks
 * which cache it reads and writes.
 *
 * Extended params use the FX_PARAM_UNSET sentinel: while unset, fx_push_*
 * never writes the matching amy_event field, so AMY keeps its factory value.
 * Rows therefore display the AMY default while unset, and the first encoder
 * step seeds the field from that default - shown and audible never disagree. */

/* ── Hub ─────────────────────────────────────────────────────────────────── */

typedef enum {
    FXH_BUS0 = 0,
    FXH_BUS1,
    FXH_BUS2,
    FXH_BUS3,
    FXH_PRESET_GLOBAL_FX,
    FXH_BACK,
    FXH_COUNT
} fxhub_item_id_t;

_Static_assert(FXH_BUS3 - FXH_BUS0 + 1 == FX_BUS_COUNT,
               "hub bus rows must cover every bus");

static menu_item_view_t s_hub_items[FXH_COUNT];

uint8_t fxhub_item_count(void)
{
    return FXH_COUNT;
}

const menu_item_view_t *fxhub_build_items(void)
{
    for (int i = 0; i < FXH_COUNT; i++) {
        s_hub_items[i].value[0] = '\0';
    }

    for (uint8_t bus = 0; bus < FX_BUS_COUNT; bus++) {
        menu_item_view_t *it = &s_hub_items[FXH_BUS0 + bus];
        snprintf(it->label, MENU_LABEL_LEN, "Bus %u %s",
                 (unsigned)(bus + 1u), fx_bus_label(bus));
        /* Bus 0 always carries the melodic mix, so it has no on/off to show. */
        if (bus == FX_BUS_HOME) {
            snprintf(it->value, MENU_VALUE_LEN, ">");
        } else {
            snprintf(it->value, MENU_VALUE_LEN, "%s >",
                     fx_bus_is_active(bus) ? "ON" : "OFF");
        }
    }

    /* "Presets alter bus FX? y/n" - OFF makes Juno presets timbre-only. */
    snprintf(s_hub_items[FXH_PRESET_GLOBAL_FX].label, MENU_LABEL_LEN, "Preset FX");
    snprintf(s_hub_items[FXH_PRESET_GLOBAL_FX].value, MENU_VALUE_LEN, "%s",
             s_fx_presets_alter_global ? "ON" : "OFF");

    snprintf(s_hub_items[FXH_BACK].label, MENU_LABEL_LEN, "< Back");

    return s_hub_items;
}

bool fxhub_item_is_bus(uint8_t idx, uint8_t *bus_out)
{
    if (idx > FXH_BUS3) return false;
    if (bus_out) *bus_out = (uint8_t)(idx - FXH_BUS0);
    return true;
}

bool fxhub_item_is_value(uint8_t idx)
{
    return idx == FXH_PRESET_GLOBAL_FX;
}

bool fxhub_item_is_back(uint8_t idx)
{
    return idx == FXH_BACK;
}

void fxhub_edit_value(uint8_t idx, int delta)
{
    if (delta == 0) return;
    if (idx != FXH_PRESET_GLOBAL_FX) return;
    s_fx_presets_alter_global = !s_fx_presets_alter_global;
    /* Re-arming the guard re-imposes the cached FX at once, on every bus,
     * undoing whatever the last preset left behind. */
    if (!s_fx_presets_alter_global) synth_ui_fx_reassert_all();
}

/* ── Per-bus page ────────────────────────────────────────────────────────── */

typedef enum {
    FXI_SPLIT = 0,  /* route this bus's group off bus 0 (no-op row on bus 0) */
    /* Rows follow the per-bus chain order in amy_fill_buffer():
     * dist -> EQ -> chorus -> echo -> reverb, then the bus gain. */
    FXI_DIST_TYPE,
    FXI_DIST_DRIVE,
    FXI_DIST_BITS,
    FXI_DIST_RATE,
    FXI_DIST_MIX,
    FXI_EQ_LOW,
    FXI_EQ_MID,
    FXI_EQ_HIGH,
    FXI_CHORUS_LEVEL,
    FXI_CHORUS_RATE,
    FXI_CHORUS_DEPTH,
    FXI_CHORUS_DELAY,
    FXI_ECHO_LEVEL,
    FXI_ECHO_FEEDBACK,
    FXI_ECHO_MODE,  /* Sync: Echo Note sets the time; Free: Echo Time does */
    FXI_ECHO_NOTE,
    FXI_ECHO_TIME,
    FXI_ECHO_TONE,
    FXI_REVERB_LEVEL,
    FXI_REVERB_LIVENESS,
    FXI_REVERB_DAMPING,
    FXI_REVERB_XOVER,
    FXI_LEVEL,
    FXI_BACK,
    FXI_COUNT
} fx_menu_item_id_t;

static menu_item_view_t s_fx_items[FXI_COUNT];

/* Which bus the page is bound to; the hub sets it on the way in. */
static uint8_t s_page_bus = FX_BUS_HOME;

void fx_menu_set_bus(uint8_t bus)
{
    s_page_bus = (bus < FX_BUS_COUNT) ? bus : FX_BUS_HOME;
}

const char *fx_menu_title(void)
{
    static char s_title[24];
    snprintf(s_title, sizeof s_title, "BUS %u FX  %s",
             (unsigned)(s_page_bus + 1u), fx_bus_label(s_page_bus));
    return s_title;
}

/* Effective value of a sentinel-gated param: while unset, the AMY factory
 * default (what the bus is actually running); the stored value once set. */
static int fx_eff(int16_t v, int def)
{
    return (v == FX_PARAM_UNSET) ? def : (int)v;
}

uint8_t fx_menu_item_count(void)
{
    return FXI_COUNT;
}

const menu_item_view_t *fx_menu_build_items(void)
{
    const fx_state_t *f = &s_fx[s_page_bus];

    for (int i = 0; i < FXI_COUNT; i++) {
        s_fx_items[i].value[0] = '\0';
    }

    if (s_page_bus == FX_BUS_HOME) {
        /* Bus 0 is where everything renders by default; nothing to split. */
        snprintf(s_fx_items[FXI_SPLIT].label, MENU_LABEL_LEN, "Bus");
        snprintf(s_fx_items[FXI_SPLIT].value, MENU_VALUE_LEN, "home");
    } else if (s_page_bus == FX_BUS_CLIPS) {
        /* The clip bus is owned outright: clips never render on bus 0. */
        snprintf(s_fx_items[FXI_SPLIT].label, MENU_LABEL_LEN, "Bus");
        snprintf(s_fx_items[FXI_SPLIT].value, MENU_VALUE_LEN, "clips");
    } else {
        snprintf(s_fx_items[FXI_SPLIT].label, MENU_LABEL_LEN, "Split");
        snprintf(s_fx_items[FXI_SPLIT].value, MENU_VALUE_LEN, "%s",
                 fx_group_is_split(fx_group_of_bus(s_page_bus)) ? "ON" : "OFF");
    }

    /* Bus distortion; same field model as the per-track dist editor
     * (type = stage mask, drive 1..16, bits/rate for CRUSH). */
    snprintf(s_fx_items[FXI_DIST_TYPE].label, MENU_LABEL_LEN, "Dist");
    snprintf(s_fx_items[FXI_DIST_TYPE].value, MENU_VALUE_LEN, "%s",
             seq_dist_stage_label(f->bus_dist_type));
    snprintf(s_fx_items[FXI_DIST_DRIVE].label, MENU_LABEL_LEN, "Dst Drive");
    snprintf(s_fx_items[FXI_DIST_DRIVE].value, MENU_VALUE_LEN, "%u",
             (unsigned)f->bus_dist_drive);
    snprintf(s_fx_items[FXI_DIST_BITS].label, MENU_LABEL_LEN, "Dst Bits");
    snprintf(s_fx_items[FXI_DIST_BITS].value, MENU_VALUE_LEN, "%u",
             (unsigned)f->bus_dist_bits);
    snprintf(s_fx_items[FXI_DIST_RATE].label, MENU_LABEL_LEN, "Dst Rate");
    snprintf(s_fx_items[FXI_DIST_RATE].value, MENU_VALUE_LEN, "%u",
             (unsigned)f->bus_dist_rate);
    snprintf(s_fx_items[FXI_DIST_MIX].label, MENU_LABEL_LEN, "Dst Mix");
    snprintf(s_fx_items[FXI_DIST_MIX].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)f->bus_dist_mix);

    snprintf(s_fx_items[FXI_EQ_LOW].label, MENU_LABEL_LEN, "EQ Low");
    snprintf(s_fx_items[FXI_EQ_LOW].value, MENU_VALUE_LEN, "%+ddB",
             (int)f->eq_low_db);
    snprintf(s_fx_items[FXI_EQ_MID].label, MENU_LABEL_LEN, "EQ Mid");
    snprintf(s_fx_items[FXI_EQ_MID].value, MENU_VALUE_LEN, "%+ddB",
             (int)f->eq_mid_db);
    snprintf(s_fx_items[FXI_EQ_HIGH].label, MENU_LABEL_LEN, "EQ High");
    snprintf(s_fx_items[FXI_EQ_HIGH].value, MENU_VALUE_LEN, "%+ddB",
             (int)f->eq_high_db);

    snprintf(s_fx_items[FXI_CHORUS_LEVEL].label, MENU_LABEL_LEN, "Chorus");
    snprintf(s_fx_items[FXI_CHORUS_LEVEL].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)f->chorus_level);
    {
        int r = fx_eff(f->chorus_rate, 50);   /* centi-Hz */
        snprintf(s_fx_items[FXI_CHORUS_RATE].label, MENU_LABEL_LEN, "Cho Rate");
        snprintf(s_fx_items[FXI_CHORUS_RATE].value, MENU_VALUE_LEN, "%d.%02dHz",
                 r / 100, r % 100);
    }
    snprintf(s_fx_items[FXI_CHORUS_DEPTH].label, MENU_LABEL_LEN, "Cho Depth");
    snprintf(s_fx_items[FXI_CHORUS_DEPTH].value, MENU_VALUE_LEN, "%d%%",
             fx_eff(f->chorus_depth, 50));
    snprintf(s_fx_items[FXI_CHORUS_DELAY].label, MENU_LABEL_LEN, "Cho Delay");
    snprintf(s_fx_items[FXI_CHORUS_DELAY].value, MENU_VALUE_LEN, "%dsmp",
             fx_eff(f->chorus_delay, 512));

    snprintf(s_fx_items[FXI_ECHO_LEVEL].label, MENU_LABEL_LEN, "Echo");
    snprintf(s_fx_items[FXI_ECHO_LEVEL].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)f->echo_level);
    snprintf(s_fx_items[FXI_ECHO_FEEDBACK].label, MENU_LABEL_LEN, "Echo Fbk");
    snprintf(s_fx_items[FXI_ECHO_FEEDBACK].value, MENU_VALUE_LEN, "%d%%",
             fx_eff(f->echo_feedback, 0));
    snprintf(s_fx_items[FXI_ECHO_MODE].label, MENU_LABEL_LEN, "Echo Mode");
    snprintf(s_fx_items[FXI_ECHO_MODE].value, MENU_VALUE_LEN, "%s",
             f->echo_sync ? "Sync" : "Free");
    /* The row the mode does not use stays visible in parentheses, read-only:
     * in Sync, Echo Time shows the ms the note works out to. */
    snprintf(s_fx_items[FXI_ECHO_NOTE].label, MENU_LABEL_LEN, "Echo Note");
    snprintf(s_fx_items[FXI_ECHO_NOTE].value, MENU_VALUE_LEN,
             f->echo_sync ? "%s" : "(%s)", amy_fx_echo_div_label(f->echo_div));
    snprintf(s_fx_items[FXI_ECHO_TIME].label, MENU_LABEL_LEN, "Echo Time");
    snprintf(s_fx_items[FXI_ECHO_TIME].value, MENU_VALUE_LEN,
             f->echo_sync ? "(%dms)" : "%dms",
             (int)(amy_fx_echo_time_ms(s_page_bus) + 0.5f));
    snprintf(s_fx_items[FXI_ECHO_TONE].label, MENU_LABEL_LEN, "Echo Tone");
    snprintf(s_fx_items[FXI_ECHO_TONE].value, MENU_VALUE_LEN, "%+d",
             fx_eff(f->echo_tone, 0));

    snprintf(s_fx_items[FXI_REVERB_LEVEL].label, MENU_LABEL_LEN, "Reverb");
    snprintf(s_fx_items[FXI_REVERB_LEVEL].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)f->reverb_level);
    snprintf(s_fx_items[FXI_REVERB_LIVENESS].label, MENU_LABEL_LEN, "Rev Live");
    snprintf(s_fx_items[FXI_REVERB_LIVENESS].value, MENU_VALUE_LEN, "%d%%",
             fx_eff(f->reverb_liveness, 85));
    snprintf(s_fx_items[FXI_REVERB_DAMPING].label, MENU_LABEL_LEN, "Rev Damp");
    snprintf(s_fx_items[FXI_REVERB_DAMPING].value, MENU_VALUE_LEN, "%d%%",
             fx_eff(f->reverb_damping, 50));
    snprintf(s_fx_items[FXI_REVERB_XOVER].label, MENU_LABEL_LEN, "Rev Xover");
    snprintf(s_fx_items[FXI_REVERB_XOVER].value, MENU_VALUE_LEN, "%dHz",
             fx_eff(f->reverb_xover_hz, 3000));

    snprintf(s_fx_items[FXI_LEVEL].label, MENU_LABEL_LEN, "Level");
    snprintf(s_fx_items[FXI_LEVEL].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)f->level);

    snprintf(s_fx_items[FXI_BACK].label, MENU_LABEL_LEN, "< Back");

    return s_fx_items;
}

bool fx_menu_item_is_value(uint8_t idx)
{
    /* Every row except Back holds an editable value - bar Split on bus 0 and
     * the clip bus, which state where the bus sits rather than offering a
     * choice, and whichever of Echo Note / Echo Time the mode is not using. */
    if (idx >= FXI_BACK) return false;
    if (idx == FXI_ECHO_NOTE) return s_fx[s_page_bus].echo_sync;
    if (idx == FXI_ECHO_TIME) return !s_fx[s_page_bus].echo_sync;
    if (idx == FXI_SPLIT && (s_page_bus == FX_BUS_HOME || s_page_bus == FX_BUS_CLIPS))
        return false;
    return true;
}

bool fx_menu_item_is_back(uint8_t idx)
{
    return idx == FXI_BACK;
}

/* Step a sentinel-gated param: the first edit seeds the field from the AMY
 * default it was displaying, then steps and pushes normally. */
static void fx_step(int16_t *field, int def, int step, int lo, int hi,
                    int dir, void (*push)(uint8_t), uint8_t bus)
{
    int cur = (*field == FX_PARAM_UNSET) ? def : (int)*field;
    *field = (int16_t)SEQ_CLAMP_INT(cur + dir * step, lo, hi);
    push(bus);
}

void fx_menu_edit_value(uint8_t idx, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    if (dir == 0) return;

    uint8_t bus = s_page_bus;
    fx_state_t *f = &s_fx[bus];

    switch ((fx_menu_item_id_t)idx) {
        case FXI_SPLIT: {
            /* A toggle, not a range: either encoder direction flips it. */
            fx_group_t g = fx_group_of_bus(bus);
            amy_fx_set_bus_split(g, !fx_group_is_split(g));
            break;
        }
        case FXI_DIST_TYPE:
            /* 8-state stage-set cycle like the per-track dist editor: OFF is
             * a value in the cycle, not a separate toggle. */
            f->bus_dist_type = seq_dist_stage_step(f->bus_dist_type, dir);
            fx_push_dist(bus);
            break;
        case FXI_DIST_DRIVE:
            f->bus_dist_drive = (uint8_t)SEQ_CLAMP_INT(
                (int)f->bus_dist_drive + dir,
                (int)DIST_DRIVE_MIN, (int)DIST_DRIVE_MAX);
            fx_push_dist(bus);
            break;
        case FXI_DIST_BITS:
            f->bus_dist_bits = (uint8_t)SEQ_CLAMP_INT(
                (int)f->bus_dist_bits + dir,
                (int)DIST_BITS_MIN, (int)DIST_BITS_MAX);
            fx_push_dist(bus);
            break;
        case FXI_DIST_RATE:
            f->bus_dist_rate = (uint8_t)SEQ_CLAMP_INT(
                (int)f->bus_dist_rate + dir,
                (int)DIST_RATE_MIN, (int)DIST_RATE_MAX);
            fx_push_dist(bus);
            break;
        case FXI_DIST_MIX:
            f->bus_dist_mix = (uint8_t)SEQ_CLAMP_INT(
                (int)f->bus_dist_mix + dir * (int)DIST_MIX_STEP, 0, 100);
            fx_push_dist(bus);
            break;
        case FXI_EQ_LOW: {
            int v = SEQ_CLAMP_INT((int)f->eq_low_db + dir, -15, 15);
            f->eq_low_db = (int8_t)v; fx_push_eq(bus);
            break;
        }
        case FXI_EQ_MID: {
            int v = SEQ_CLAMP_INT((int)f->eq_mid_db + dir, -15, 15);
            f->eq_mid_db = (int8_t)v; fx_push_eq(bus);
            break;
        }
        case FXI_EQ_HIGH: {
            int v = SEQ_CLAMP_INT((int)f->eq_high_db + dir, -15, 15);
            f->eq_high_db = (int8_t)v; fx_push_eq(bus);
            break;
        }
        case FXI_CHORUS_LEVEL: {
            int v = SEQ_CLAMP_INT((int)f->chorus_level + dir * 5, 0, 100);
            f->chorus_level = (uint8_t)v; fx_push_chorus(bus);
            break;
        }
        case FXI_CHORUS_RATE:
            /* centi-Hz: 5..1000 = 0.05..10 Hz in 0.05 Hz steps. */
            fx_step(&f->chorus_rate, 50, 5, 5, 1000, dir, fx_push_chorus, bus);
            break;
        case FXI_CHORUS_DEPTH:
            fx_step(&f->chorus_depth, 50, 5, 0, 100, dir, fx_push_chorus, bus);
            break;
        case FXI_CHORUS_DELAY:
            /* samples: the sweep runs 0..N, centred on N/2; 512 is the line. */
            fx_step(&f->chorus_delay, 512, 16, 16, 512, dir, fx_push_chorus, bus);
            break;
        case FXI_ECHO_LEVEL: {
            int v = SEQ_CLAMP_INT((int)f->echo_level + dir * 5, 0, 100);
            f->echo_level = (uint8_t)v; fx_push_echo(bus);
            break;
        }
        case FXI_ECHO_FEEDBACK:
            fx_step(&f->echo_feedback, 0, 5, 0, 99, dir, fx_push_echo, bus);
            break;
        case FXI_ECHO_MODE:
            f->echo_sync = !f->echo_sync;
            fx_push_echo(bus);
            break;
        case FXI_ECHO_NOTE:
            f->echo_div = (uint8_t)SEQ_CLAMP_INT((int)f->echo_div + dir,
                                                 0, FX_ECHO_DIV_COUNT - 1);
            fx_push_echo(bus);
            break;
        case FXI_ECHO_TIME:
            fx_step(&f->echo_delay_ms, 500, 10, 0, FX_ECHO_MAX_MS, dir,
                    fx_push_echo, bus);
            break;
        case FXI_ECHO_TONE:
            fx_step(&f->echo_tone, 0, 5, -99, 99, dir, fx_push_echo, bus);
            break;
        case FXI_REVERB_LEVEL: {
            int v = SEQ_CLAMP_INT((int)f->reverb_level + dir * 5, 0, 100);
            f->reverb_level = (uint8_t)v; fx_push_reverb(bus);
            break;
        }
        case FXI_REVERB_LIVENESS:
            fx_step(&f->reverb_liveness, 85, 5, 0, 100, dir, fx_push_reverb, bus);
            break;
        case FXI_REVERB_DAMPING:
            fx_step(&f->reverb_damping, 50, 5, 0, 100, dir, fx_push_reverb, bus);
            break;
        case FXI_REVERB_XOVER:
            fx_step(&f->reverb_xover_hz, 3000, 250, 500, 8000, dir,
                    fx_push_reverb, bus);
            break;
        case FXI_LEVEL: {
            int v = SEQ_CLAMP_INT((int)f->level + dir * 5, 0, 200);
            amy_fx_set_bus_level(bus, (uint8_t)v);
            break;
        }
        default:
            break;
    }
}
