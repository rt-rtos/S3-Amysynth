#include "synth_ui/synth_ui_internal.h"
#include "sequencer_core.h"
#include "prog_gen.h"
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════════
 *  PROG GEN PAGE (Main Menu -> "Prog Gen")
 * ════════════════════════════════════════════════════════════════════════
 * Item model for the chord-progression generator; page state and input
 * routing live in ui_screen_menu.c (same split as the Bounce/Chords pages).
 *
 * The top block is the generator's parameters, which live in sequencer_core
 * beside the progression they produce - this page keeps no copy, so a project
 * load shows the settings that made the chords on screen. Generate overwrites
 * the progression and enables it; one level of Undo puts back whatever the
 * last Generate replaced.
 *
 * Key and scale are the global quantizer's, not the page's: the generated
 * roots are diatonic to what the rest of the instrument is already playing. */

enum {
    PGEN_ROW_STYLE = 0,
    PGEN_ROW_LEN,
    PGEN_ROW_BARS,
    PGEN_ROW_EXT,
    PGEN_ROW_VAR,
    PGEN_ROW_SEED,
    PGEN_ROW_GENERATE,
    PGEN_ROW_UNDO,
    PGEN_ROW_BACK,
    PGEN_ROW_COUNT
};

static const uint8_t PGEN_LEN_STEPS[]  = { 2, 4, 8 };
static const uint8_t PGEN_BARS_STEPS[] = { 1, 2, 4 };
#define PGEN_LEN_STEP_COUNT  (sizeof(PGEN_LEN_STEPS) / sizeof(PGEN_LEN_STEPS[0]))
#define PGEN_BARS_STEP_COUNT (sizeof(PGEN_BARS_STEPS) / sizeof(PGEN_BARS_STEPS[0]))

#define PGEN_SEED_COUNT 1000
#define PGEN_VAR_COUNT  4

static menu_item_view_t s_items[PGEN_ROW_COUNT];

/* One level of undo, runtime only: what the last Generate overwrote. Not
 * persisted - a project holds the progression, not the step before it. */
static struct {
    bool             valid;
    bool             enabled;
    uint8_t          count;
    prog_gen_entry_t entries[PROG_GEN_MAX_ENTRIES];
} s_undo;

/* Seed of the last Generate, -1 before the first one. An edited seed is used
 * as it stands; pressing Generate again on the same seed rolls to the next. */
static int s_last_gen_seed = -1;

static uint8_t wrap_index(int value, int count)
{
    if (count <= 0) return 0;
    value %= count;
    if (value < 0) value += count;
    return (uint8_t)value;
}

/* Index of a value in a step list; 0 for a value that is not on it. */
static uint8_t step_index(const uint8_t *steps, size_t n, uint8_t value)
{
    for (size_t i = 0; i < n; i++) {
        if (steps[i] == value) return (uint8_t)i;
    }
    return 0;
}

static uint8_t clamped_style(prog_gen_family_t family, uint8_t style)
{
    uint8_t count = prog_gen_style_count(family);
    return (style >= count) ? (uint8_t)(count - 1) : style;
}

static const char *ext_word(uint8_t ext)
{
    switch (ext) {
        case PROG_GEN_EXT_7TH: return "7th";
        case PROG_GEN_EXT_9TH: return "9th";
        default:               return "Tri";
    }
}

static void proggen_generate(void)
{
    prog_gen_params_t p;
    sequencer_core_progression_gen_params_get(&p);

    if ((int)p.seed == s_last_gen_seed) {
        p.seed = (uint16_t)((p.seed + 1u) % PGEN_SEED_COUNT);
        sequencer_core_progression_gen_params_set(&p);
    }

    s_undo.valid   = true;
    s_undo.enabled = sequencer_core_progression_get_enabled();
    s_undo.count   = sequencer_core_progression_get_count();
    if (s_undo.count > PROG_GEN_MAX_ENTRIES) s_undo.count = PROG_GEN_MAX_ENTRIES;
    for (uint8_t i = 0; i < s_undo.count; i++) {
        sequencer_core_progression_get_entry(i, &s_undo.entries[i].root,
                                             &s_undo.entries[i].chord_type,
                                             &s_undo.entries[i].duration_bars);
    }

    uint8_t root  = (uint8_t)(sequencer_core_get_quantizer_root_note() % 12);
    uint8_t scale = sequencer_core_get_quantizer_scale();

    prog_gen_entry_t out[PROG_GEN_MAX_ENTRIES];
    uint8_t n = prog_gen_generate(&p, root, scale, out);
    /* Ascending order: the setter appends only at idx == count. */
    for (uint8_t i = 0; i < n; i++) {
        sequencer_core_progression_set_entry(i, out[i].root, out[i].chord_type,
                                             out[i].duration_bars);
    }
    sequencer_core_progression_set_count(n);
    sequencer_core_progression_set_enabled(true);

    s_last_gen_seed = (int)p.seed;
}

static void proggen_undo(void)
{
    if (!s_undo.valid) return;
    for (uint8_t i = 0; i < s_undo.count; i++) {
        sequencer_core_progression_set_entry(i, s_undo.entries[i].root,
                                             s_undo.entries[i].chord_type,
                                             s_undo.entries[i].duration_bars);
    }
    sequencer_core_progression_set_count(s_undo.count);
    sequencer_core_progression_set_enabled(s_undo.enabled);
    s_undo.valid = false;
}

const char *proggen_menu_title(void)
{
    return "PROG GEN";
}

const menu_item_view_t *proggen_menu_build_items(void)
{
    prog_gen_params_t p;
    sequencer_core_progression_gen_params_get(&p);

    prog_gen_family_t family =
        prog_gen_family_for_scale(sequencer_core_get_quantizer_scale());

    snprintf(s_items[PGEN_ROW_STYLE].label, MENU_LABEL_LEN, "Style");
    snprintf(s_items[PGEN_ROW_STYLE].value, MENU_VALUE_LEN, "%s",
             prog_gen_style_name(family, clamped_style(family, p.style)));

    snprintf(s_items[PGEN_ROW_LEN].label, MENU_LABEL_LEN, "Len");
    snprintf(s_items[PGEN_ROW_LEN].value, MENU_VALUE_LEN, "%u", (unsigned)p.len);

    snprintf(s_items[PGEN_ROW_BARS].label, MENU_LABEL_LEN, "Bars");
    snprintf(s_items[PGEN_ROW_BARS].value, MENU_VALUE_LEN, "%u", (unsigned)p.bars);

    snprintf(s_items[PGEN_ROW_EXT].label, MENU_LABEL_LEN, "Ext");
    snprintf(s_items[PGEN_ROW_EXT].value, MENU_VALUE_LEN, "%s", ext_word(p.ext));

    snprintf(s_items[PGEN_ROW_VAR].label, MENU_LABEL_LEN, "Var");
    snprintf(s_items[PGEN_ROW_VAR].value, MENU_VALUE_LEN, "%u", (unsigned)p.var);

    snprintf(s_items[PGEN_ROW_SEED].label, MENU_LABEL_LEN, "Seed");
    snprintf(s_items[PGEN_ROW_SEED].value, MENU_VALUE_LEN, "%u", (unsigned)p.seed);

    snprintf(s_items[PGEN_ROW_GENERATE].label, MENU_LABEL_LEN, "Generate");
    s_items[PGEN_ROW_GENERATE].value[0] = '\0';

    snprintf(s_items[PGEN_ROW_UNDO].label, MENU_LABEL_LEN, "Undo");
    s_items[PGEN_ROW_UNDO].value[0] = '\0';

    snprintf(s_items[PGEN_ROW_BACK].label, MENU_LABEL_LEN, "< Back");
    s_items[PGEN_ROW_BACK].value[0] = '\0';
    return s_items;
}

uint8_t proggen_menu_item_count(void)
{
    return (uint8_t)PGEN_ROW_COUNT;
}

bool proggen_menu_item_is_back(uint8_t idx)
{
    return idx == PGEN_ROW_BACK;
}

bool proggen_menu_item_is_value(uint8_t idx)
{
    return idx <= PGEN_ROW_SEED;
}

/* Returns the new menu_editing state (mirrors bounce_menu_handle_click). */
bool proggen_menu_handle_click(uint8_t idx)
{
    if (proggen_menu_item_is_value(idx)) {
        return !seq_state.menu_editing;   /* toggle value editing */
    }

    if (idx == PGEN_ROW_GENERATE) {
        proggen_generate();
        return false;
    }

    if (idx == PGEN_ROW_UNDO) {
        proggen_undo();
        return false;
    }

    return false;
}

void proggen_menu_edit_value(uint8_t idx, int delta)
{
    if (delta == 0) return;
    int move = delta;

    prog_gen_params_t p;
    sequencer_core_progression_gen_params_get(&p);

    if (idx == PGEN_ROW_STYLE) {
        prog_gen_family_t family =
            prog_gen_family_for_scale(sequencer_core_get_quantizer_scale());
        int count = (int)prog_gen_style_count(family);
        p.style = wrap_index((int)clamped_style(family, p.style) + move, count);
    } else if (idx == PGEN_ROW_LEN) {
        uint8_t i = step_index(PGEN_LEN_STEPS, PGEN_LEN_STEP_COUNT, p.len);
        p.len = PGEN_LEN_STEPS[wrap_index((int)i + move, (int)PGEN_LEN_STEP_COUNT)];
    } else if (idx == PGEN_ROW_BARS) {
        uint8_t i = step_index(PGEN_BARS_STEPS, PGEN_BARS_STEP_COUNT, p.bars);
        p.bars = PGEN_BARS_STEPS[wrap_index((int)i + move, (int)PGEN_BARS_STEP_COUNT)];
    } else if (idx == PGEN_ROW_EXT) {
        p.ext = wrap_index((int)p.ext + move, (int)PROG_GEN_EXT_COUNT);
    } else if (idx == PGEN_ROW_VAR) {
        p.var = wrap_index((int)p.var + move, PGEN_VAR_COUNT);
    } else if (idx == PGEN_ROW_SEED) {
        int seed = (int)p.seed + move;
        seed %= PGEN_SEED_COUNT;
        if (seed < 0) seed += PGEN_SEED_COUNT;
        p.seed = (uint16_t)seed;
    } else {
        return;
    }

    sequencer_core_progression_gen_params_set(&p);
}

/* Nothing on this page is per-visit: the parameters live in sequencer_core and
 * the undo buffer outlives the page deliberately, so a visit starts on what the
 * last one left. */
void proggen_menu_reset(void)
{
}
