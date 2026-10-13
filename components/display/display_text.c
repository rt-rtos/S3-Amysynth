#include "display_text.h"
#include <stdio.h>

void display_hz_label(char out[6], float hz)
{
    if (hz < 999.5f)        snprintf(out, 6, "%u", (unsigned)(hz + 0.5f));
    else if (hz < 9950.0f)  snprintf(out, 6, "%.1fk", (double)(hz / 1000.0f));
    else                    snprintf(out, 6, "%uk", (unsigned)(hz / 1000.0f + 0.5f));
}
