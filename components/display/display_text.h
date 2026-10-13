#pragma once

/* Small value-to-text formatters shared by renderers and the screens that
 * fill their view structs. Pure; any task. */

/* A frequency in at most four characters: "550", "2.2k", "11k". */
void display_hz_label(char out[6], float hz);
