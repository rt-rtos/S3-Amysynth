#pragma once
#include "u8g2.h"
#include "graph_popup.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Envelope editor screen on the dual-colour panel: a context bar in the
 * yellow rows 0..ENV_TOPBAR_H-1, the graph_popup plot below it (the host
 * places the widget at y = ENV_TOPBAR_H), and the persistent curve-type code
 * in the plot's top-right corner. */
#define ENV_TOPBAR_H 16

/* View state: preformatted text, so the renderer owns layout only. Empty
 * strings are not drawn. The right readout and the middle readout share the
 * 60..126 px band; the host fills at most one of them. */
typedef struct {
    char        label[24];    /* left: edit target and EG ("L1 T2 EG0*")    */
    char        mid[24];      /* selected point's value ("A 120ms")         */
    char        right[16];    /* right readout (target depth, SWG, AMP)     */
    bool        right_flash;  /* draw right inverted: type-cycle flash      */
    char        active[8];    /* one letter per target the EG drives,
                                 dropped when it would hit the right text   */
    const char *type_code;    /* curve-type code in the plot corner, or NULL */
} env_view_t;

/* Draws over the current buffer contents; the caller clears first. */
void env_view_draw(u8g2_t *u8g2, const env_view_t *v, const gpopup_t *plot);

#ifdef __cplusplus
}
#endif
