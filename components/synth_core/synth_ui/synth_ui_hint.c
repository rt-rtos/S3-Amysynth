#include "synth_ui_hint.h"
#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include <stdio.h>

/* The button-hint strip reads its labels from the view descriptor table,
 * indexed by the single precedence resolver (synth_ui_active_view()), so it can
 * never disagree with the draw switch about which view is active. A NULL static
 * label means the cell is dynamic - it depends on ui_mode, which the view id
 * does not carry - and is filled by the row's b*_fn. */
static const char *hint_cell(const char *label, const char *(*fn)(void))
{
    return label ? label : fn();
}

const char *synth_ui_hint_text(void)
{
    const ui_view_desc_t *d = &ui_view_table[synth_ui_active_view()];
    static char buf[32];
    const char *bs = d->bs_fn ? d->bs_fn() : NULL;
    snprintf(buf, sizeof(buf), "1:%s 2:%s 3:%s%s%s",
             hint_cell(d->b1, d->b1_fn),
             hint_cell(d->b2, d->b2_fn),
             d->b3,  /* b3 is always a static label */
             bs ? " LB:" : "", bs ? bs : "");
    return buf;
}

bool synth_ui_hint_visible(void)
{
    if (seq_state.ui_mode == UI_MODE_PROG) {
        return false;
    }
    if (seq_state.ui_mode == UI_MODE_DRONE && s_drone_vis_open) {
        return false;
    }
    return true;
}
