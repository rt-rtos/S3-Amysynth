// Status LED updater (contract in status_led.h). A low-priority Core-0 task
// ticks at 100 ms for blink animation; the core-load sample and any LED
// rewrite happen only once per CONFIG_AMYSYNTH_STATUS_LED_PERIOD_MS window,
// and the WS2812 latches its color, so the steady-state cost is one
// core_load_sample() read of the idle-hook counters per window (no kernel
// critical section) and zero RMT traffic while the band is stable.
#include "status_led.h"

#if CONFIG_AMYSYNTH_STATUS_LED

#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "led_strip.h"
#include "core_load.h"

static const char *TAG = "status_led";

#define STATUS_LED_TICK_MS      100
#define STATUS_LED_RENDER_CORE  1
// Blink burst: 3 on/off cycles of STATUS_LED_TICK_MS halves per alloc failure.
#define STATUS_LED_BLINK_TICKS  6

_Static_assert(CORE_LOAD_NUM_CORES > STATUS_LED_RENDER_CORE,
               "render core index out of range");

typedef struct {
    uint8_t r, g, b;
} rgb_t;

// Band colors at full scale, scaled by BRIGHTNESS at write time. Hand-balanced
// for similar perceived brightness (green reads far brighter than red on the
// WS2812); orange keeps little green so it stays distinct from yellow.
static const rgb_t s_band_color[] = {
    {0, 130, 0},    // green:  < 50 %
    {86, 86, 0},    // yellow: >= 50 %
    {171, 43, 0},   // orange: >= 75 %
    {255, 0, 0},    // red:    >= 90 %
};
// Band i is entered upward at load >= s_band_enter[i], left downward at
// load < s_band_enter[i] - STATUS_LED_HYST.
static const uint8_t s_band_enter[] = {0, 50, 75, 90};
#define STATUS_LED_NUM_BANDS (sizeof(s_band_enter) / sizeof(s_band_enter[0]))
#define STATUS_LED_HYST 5

static const rgb_t s_blink_color = {186, 0, 186}; // magenta (luma-matched): alloc failure
static const rgb_t s_off_color = {0, 0, 0};

static led_strip_handle_t s_strip;
static atomic_bool s_alloc_failed;

static void write_color(rgb_t c)
{
    static rgb_t s_current = {0, 0, 0};
    static bool s_written;
    if (s_written && c.r == s_current.r && c.g == s_current.g && c.b == s_current.b) {
        return;
    }
    const uint32_t scale = CONFIG_AMYSYNTH_STATUS_LED_BRIGHTNESS;
    esp_err_t err = led_strip_set_pixel(s_strip, 0,
                                        (c.r * scale) / 255,
                                        (c.g * scale) / 255,
                                        (c.b * scale) / 255);
    if (err == ESP_OK) {
        err = led_strip_refresh(s_strip);
    }
    if (err == ESP_OK) {
        s_current = c;
        s_written = true;
    }
}

static unsigned band_update(unsigned band, uint8_t load)
{
    while (band + 1 < STATUS_LED_NUM_BANDS && load >= s_band_enter[band + 1]) {
        band++;
    }
    while (band > 0 && load + STATUS_LED_HYST < s_band_enter[band]) {
        band--;
    }
    return band;
}

static void status_led_task(void *arg)
{
    (void)arg;
    const uint32_t ticks_per_sample =
        CONFIG_AMYSYNTH_STATUS_LED_PERIOD_MS / STATUS_LED_TICK_MS;
    unsigned band = 0;
    unsigned blink_ticks = 0;
    uint32_t tick = 0;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(STATUS_LED_TICK_MS));

        if (atomic_exchange(&s_alloc_failed, false)) {
            blink_ticks = STATUS_LED_BLINK_TICKS;
        }

        if (blink_ticks > 0) {
            write_color((blink_ticks & 1) ? s_blink_color : s_off_color);
            blink_ticks--;
            continue;
        }

        if (++tick >= ticks_per_sample) {
            tick = 0;
            uint8_t busy[CORE_LOAD_NUM_CORES];
            if (core_load_sample(busy)) {
                band = band_update(band, busy[STATUS_LED_RENDER_CORE]);
            }
        }
        write_color(s_band_color[band]);
    }
}

#if CONFIG_AMYSYNTH_STATUS_LED_ALLOC_BLINK
// Runs in the context of whatever task's allocation failed - flag only.
static void alloc_failed_hook(size_t size, uint32_t caps, const char *function_name)
{
    (void)size;
    (void)caps;
    (void)function_name;
    status_led_notify_alloc_failure();
}
#endif

void status_led_notify_alloc_failure(void)
{
    atomic_store(&s_alloc_failed, true);
}

void status_led_start(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_AMYSYNTH_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false, // 1 LED: a 24-bit frame fits the RMT FIFO
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED driver init failed (%s); status LED disabled",
                 esp_err_to_name(err));
        s_strip = NULL;
        return;
    }
    led_strip_clear(s_strip);

    BaseType_t ok = xTaskCreatePinnedToCore(status_led_task, "status_led",
                                            3072, NULL, 2, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "task create failed; status LED disabled");
        led_strip_del(s_strip);
        s_strip = NULL;
        return;
    }

#if CONFIG_AMYSYNTH_STATUS_LED_ALLOC_BLINK
    heap_caps_register_failed_alloc_callback(alloc_failed_hook);
#endif
}

#endif // CONFIG_AMYSYNTH_STATUS_LED
