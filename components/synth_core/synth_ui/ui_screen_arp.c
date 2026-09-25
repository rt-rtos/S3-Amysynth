#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "arp_core.h"
#include "patch_names.h"
#include "seq_clamp.h"
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  ARP SCREEN
 * ════════════════════════════════════════════════════════════════════════ */

/* View-resolved, not ui_mode: main.c's input isolation guard keys off this, so
 * answering true under the filter/LFO overlays (which leave ui_mode alone) would
 * swallow those editors' own MY_BUTTON_0 taps. */
bool synth_ui_arp_is_active(void)
{
    return synth_ui_active_view() == UI_VIEW_ARP;
}

/* Cursor + editing flags are the arp screen's own, independent of the menu's;
 * file-static because the screen is a singleton. */
static uint8_t s_arp_cursor  = ARP_CUR_ENABLE;
static bool    s_arp_editing = false;

/* Build the flat arp view from arp_core for the renderer. */
void arp_build_view(arp_view_t *out)
{
    static const char *s_dir_names[ARP_DIR_COUNT] = { "UP", "DOWN", "SLOT" };
    out->enabled  = arp_get_enabled();
    out->mode_str = s_dir_names[arp_get_direction()];
    out->octaves  = arp_get_octaves();
    out->rate_str = arp_rate_name(arp_get_rate());
    out->gate_pct = arp_get_gate_pct();
    for (uint8_t i = 0; i < ARP_VIEW_SLOTS; i++) {
        int16_t raw     = arp_get_slot(i);
        int16_t snapped = arp_get_slot_snapped(i);
        out->slot_rest[i] = (raw == ARP_REST);
        if (snapped >= 0) {
            out->slot_active[i] = true;
            ui_note_name((uint8_t)snapped, out->slot_name[i]);
        } else {
            out->slot_active[i] = false;
            out->slot_name[i][0] = '\0';
        }
    }
    out->cursor  = s_arp_cursor;
    out->editing = s_arp_editing;
    out->portamento_ms = arp_get_portamento_ms();
    out->quant_mode    = (uint8_t)arp_get_quant_mode();

    /* Patch indicator mirrors the sequencer view: number always, name banner
     * only while the patch hold+turn gesture is active. */
    out->patch        = arp_get_patch();
    out->patch_select = seq_state.patch_select_mode;
    out->patch_name   = patch_name_for(out->patch);
}

/* Builds the view into *out so the caller can draw from it without rebuilding. */
uint32_t arp_view_signature(arp_view_t *out)
{
    uint32_t h = FNV1A_OFFSET;
    arp_build_view(out);
    h = fnv1a_bytes(h, &out->enabled, sizeof(out->enabled));
    h = fnv1a_bytes(h, &out->octaves, sizeof(out->octaves));
    h = fnv1a_bytes(h, &out->gate_pct, sizeof(out->gate_pct));
    h = fnv1a_bytes(h, &out->cursor, sizeof(out->cursor));
    h = fnv1a_bytes(h, &out->editing, sizeof(out->editing));
    h = fnv1a_bytes(h, &out->patch, sizeof(out->patch));
    h = fnv1a_bytes(h, &out->patch_select, sizeof(out->patch_select));
    h = fnv1a_bytes(h, &out->portamento_ms, sizeof(out->portamento_ms));
    h = fnv1a_bytes(h, &out->quant_mode, sizeof(out->quant_mode));
    h = fnv1a_bytes(h, out->rate_str, 4);
    h = fnv1a_bytes(h, out->mode_str, 4);
    for (uint8_t i = 0; i < ARP_VIEW_SLOTS; i++) {
        h = fnv1a_bytes(h, &out->slot_active[i], sizeof(out->slot_active[i]));
        h = fnv1a_bytes(h, &out->slot_rest[i],   sizeof(out->slot_rest[i]));
        h = fnv1a_bytes(h, out->slot_name[i], sizeof(out->slot_name[i]));
    }
    return h;
}

static void arp_edit_value(uint8_t cursor, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    switch (cursor) {
        case ARP_CUR_ENABLE:
            if (dir != 0) arp_set_enabled(!arp_get_enabled());
            break;
        case ARP_CUR_MODE:
            if (dir != 0) {
                int nd = ((int)arp_get_direction() + dir + ARP_DIR_COUNT) % ARP_DIR_COUNT;
                arp_set_direction((arp_dir_t)nd);
            }
            break;
        case ARP_CUR_OCT:
            arp_set_octaves((uint8_t)SEQ_CLAMP_INT(
                (int)arp_get_octaves() + dir, 1, ARP_OCT_MAX));
            break;
        case ARP_CUR_RATE: {
            int r = (int)arp_get_rate() + dir;
            r = SEQ_CLAMP_INT(r, 0, ARP_RATE_COUNT - 1);
            arp_set_rate((arp_rate_t)r);
            break;
        }
        case ARP_CUR_GATE:
            arp_set_gate_pct((uint8_t)SEQ_CLAMP_INT(
                (int)arp_get_gate_pct() + dir * 5, 10, 100));
            break;
        case ARP_CUR_PORTA:
            /* 1ms/detent: fine control for short, snappy glides. */
            arp_set_portamento_ms((uint16_t)SEQ_CLAMP_INT(
                (int)arp_get_portamento_ms() + dir * 1, 0, ARP_PORTAMENTO_MAX_MS));
            break;
        case ARP_CUR_QUANT:
            /* Cycles OWN -> GLOB -> CHRD with the detent direction, wrapping. */
            if (dir != 0) {
                int nq = ((int)arp_get_quant_mode() + dir + ARP_QUANT_COUNT) % ARP_QUANT_COUNT;
                arp_set_quant_mode((arp_quant_mode_t)nq);
            }
            break;
        default: {
            /* Slot edit: chromatic note, or clear below the floor. */
            uint8_t slot = (uint8_t)(cursor - ARP_CUR_SLOT0);
            if (slot >= ARP_VIEW_SLOTS) break;
            int16_t cur = arp_get_slot(slot);
            if (cur == -1) {
                /* Unused: up seeds a note, down sets REST (one step above floor). */
                if (dir > 0) arp_set_slot(slot, (int16_t)arp_get_root_note());
                else         arp_set_slot(slot, ARP_REST);
            } else if (cur == ARP_REST) {
                /* REST is the floor: up goes to empty, not root note, so recovery
                 * is two turns rather than scrolling through octaves. */
                if (dir > 0) arp_set_slot(slot, -1);
                /* dir < 0: stay at REST, no bounce back to empty */
            } else {
                int nv = (int)cur + dir;
                if (nv < 24) {
                    arp_set_slot(slot, -1);   /* below floor → clear to empty */
                } else if (nv > 127) {
                    arp_set_slot(slot, 127);  /* ceiling clamp, no wrap */
                } else {
                    arp_set_slot(slot, (int16_t)nv);
                }
            }
            break;
        }
    }
}

void synth_ui_arp_handle_encoder(long delta)
{
    if (delta == 0) return;
    if (s_arp_editing) {
        arp_edit_value(s_arp_cursor, (int)delta);
    } else {
        int c = (int)s_arp_cursor + (int)delta;
        c = SEQ_CLAMP_INT(c, 0, ARP_CUR_COUNT - 1);
        s_arp_cursor = (uint8_t)c;
    }
    s_force_redraw = true;
}

void synth_ui_arp_handle_button(void)
{
    s_arp_editing = !s_arp_editing;
    s_force_redraw = true;
}
