#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Arp screen renderer ──────────────────────────────────────────────────
 * Arp logic lives in synth_core (arp_core); synth_ui populates this flat view
 * each frame and the renderer just draws it.
 *
 * Layout (everything visible at once, no multiplexed slots):
 *   ARP:ON | MODE | OCT:2 |      R:1/16   <- macro row 1 (cursor 0..3, L->R)
 *   GT:75% | P12 | Q:CHRD |      GL:200   <- macro row 2 (cursor 4..6, L->R)
 *   [C3]  E3   G3   B3   --   --  ...      <- 8 note slots
 *
 * Cursor index space:
 *   0=ARP enable, 1=MODE, 2=OCT, 3=RATE, 4=GATE, 5=GLIDE, 6=QUANT source,
 *   7..14 = slots 0..7.
 *
 * The row-2 centre indicator shows the patch number (full catalog, as on the
 * sequencer view); the patch changes via the hold+turn gesture, not a cursor. */

#define ARP_VIEW_SLOTS 8

#define ARP_CUR_ENABLE  0
#define ARP_CUR_MODE    1
#define ARP_CUR_OCT     2
#define ARP_CUR_RATE    3
#define ARP_CUR_GATE    4
#define ARP_CUR_PORTA   5   /* portamento/glide time, ms             */
#define ARP_CUR_QUANT   6   /* scale source: own / global / chord     */
#define ARP_CUR_SLOT0   7
#define ARP_CUR_COUNT   (ARP_CUR_SLOT0 + ARP_VIEW_SLOTS)

typedef struct {
    bool        enabled;
    const char *mode_str;     /* "UP" / "DOWN"        */
    uint8_t     octaves;
    const char *rate_str;     /* "1/16" etc           */
    uint8_t     gate_pct;
    /* Per-slot display: note name (snapped), "--" (empty), or " R " (rest).
     * slot_active: true = note present. slot_rest: true = ARP_REST sentinel.
     * Check slot_rest before slot_active to disambiguate the inactive case. */
    char        slot_name[ARP_VIEW_SLOTS][4];
    bool        slot_active[ARP_VIEW_SLOTS];
    bool        slot_rest[ARP_VIEW_SLOTS];
    uint8_t     cursor;       /* 0..ARP_CUR_COUNT-1   */
    bool        editing;      /* value being adjusted */
    uint16_t    portamento_ms;/* glide time, ms (0 = off)                   */
    uint8_t     quant_mode;   /* scale source, arp_quant_mode_t order:
                                 0 = own scale, 1 = global quantizer,
                                 2 = progression chord                      */
    /* Patch indicator (mirrors the sequencer view): the number shows top-right
     * in PATCH mode; while the select button is held, a non-NULL patch_name is
     * drawn as a centred banner. */
    uint16_t    patch;
    bool        patch_select; /* true while the patch hold+turn gesture is active */
    const char *patch_name;   /* human-readable name or NULL (table excluded)     */
} arp_view_t;

void display_arp_draw_frame(u8g2_t *u8g2, const arp_view_t *view);

#ifdef __cplusplus
}
#endif
