#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "display_flush.h"
#include "priv_display_flush.h"

static const char *TAG = "display_flush";

#define DISPLAY_FLUSH_SHADOW_BYTES 1024
#define DISPLAY_FLUSH_MAX_TILES    16

static uint8_t *s_shadow;
static bool s_valid;
static uint16_t s_len;

void display_flush_init(void)
{
    if (s_shadow != NULL) return;
    s_shadow = heap_caps_malloc(DISPLAY_FLUSH_SHADOW_BYTES, MALLOC_CAP_SPIRAM);
    if (s_shadow == NULL) {
        ESP_LOGW(TAG, "no shadow buffer; flushing full frames");
        s_len = 0;
    } else {
        s_len = DISPLAY_FLUSH_SHADOW_BYTES;
    }
    s_valid = false;
}

void display_flush(u8g2_t *u8g2)
{
    uint8_t tiles = u8g2_GetBufferTileWidth(u8g2);
    uint8_t rows = u8g2_GetBufferTileHeight(u8g2);
    uint16_t rowbytes = (uint16_t)(tiles * 8u);
    uint8_t *shadow = s_shadow;
    if (tiles > DISPLAY_FLUSH_MAX_TILES || (uint32_t)rows * rowbytes > s_len) {
        shadow = NULL;      /* geometry exceeds the shadow: full sends only */
    }
    uint8_t *buf = u8g2_GetBufferPtr(u8g2);

    bool full = (shadow == NULL) || !s_valid;
    /* Set before sending: a failed transfer clears it from the byte callback. */
    s_valid = (shadow != NULL);
    if (full) {
        u8g2_SendBuffer(u8g2);
    } else {
        display_flush_run_t runs[DISPLAY_FLUSH_MAX_RUNS];
        for (uint8_t r = 0; r < rows; r++) {
            uint8_t n = display_flush_row_runs(buf + r * rowbytes, shadow + r * rowbytes,
                                               tiles, runs);
            for (uint8_t i = 0; i < n; i++) {
                u8g2_UpdateDisplayArea(u8g2, runs[i].tx, r, runs[i].tw, 1);
            }
        }
    }
    if (s_valid) memcpy(shadow, buf, (size_t)rows * rowbytes);
}

void display_flush_invalidate(void)
{
    s_valid = false;
}
