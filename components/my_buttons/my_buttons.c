/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "my_buttons.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "esp_log.h"

static const char *TAG = "my_buttons";

// Button GPIO assignments (active low with internal pull-up)
// GPIO3 is ESP32-S3 strapping pin, using GPIO42 instead
static const int32_t s_button_gpios[MY_BUTTON_MAX] = {
    17,  // MY_BUTTON_0 → play/pause
    18,  // MY_BUTTON_1 → step toggle
    8,   // MY_BUTTON_2
    42,  // MY_BUTTON_3 (avoiding GPIO3 strapping pin)
    16,  // MY_BUTTON_ENC → encoder push button
};

static button_handle_t s_button_handles[MY_BUTTON_MAX] = {NULL};
static my_button_event_cb_t s_user_cb = NULL;
static void *s_user_data = NULL;

// Internal callback that routes events to user callback
static void button_event_cb(void *button_handle, void *usr_data)
{
    if (s_user_cb == NULL) {
        return;
    }

    my_button_id_t btn_id = (my_button_id_t)(uintptr_t)usr_data;
    button_event_t event = iot_button_get_event(button_handle);
    const char *event_str = iot_button_get_event_str(event);

    ESP_LOGI(TAG, "Button %d: %s", btn_id, event_str);
    s_user_cb(btn_id, event_str, s_user_data);
}

esp_err_t my_buttons_init(void)
{
    esp_err_t ret = ESP_OK;

    // Common button config (use defaults for timing)
    button_config_t btn_cfg = {
        .long_press_time = 0,   // Use default (1500ms)
        .short_press_time = 0,  // Use default (180ms)
    };

    for (int i = 0; i < MY_BUTTON_MAX; i++) {
        button_gpio_config_t gpio_cfg = {
            .gpio_num = s_button_gpios[i],
            .active_level = 0,           // Active low
            .enable_power_save = false,
            .disable_pull = false,       // Enable internal pull-up (for active_level=0)
        };

        ret = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &s_button_handles[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create button %d on GPIO%ld: %s",
                     i, gpio_cfg.gpio_num, esp_err_to_name(ret));
            // Clean up already created buttons
            my_buttons_deinit();
            return ret;
        }

        // Register callbacks for common events
        iot_button_register_cb(s_button_handles[i], BUTTON_PRESS_DOWN, NULL,
                               button_event_cb, (void *)(uintptr_t)i);
        iot_button_register_cb(s_button_handles[i], BUTTON_PRESS_UP, NULL,
                               button_event_cb, (void *)(uintptr_t)i);
        iot_button_register_cb(s_button_handles[i], BUTTON_SINGLE_CLICK, NULL,
                               button_event_cb, (void *)(uintptr_t)i);
        iot_button_register_cb(s_button_handles[i], BUTTON_DOUBLE_CLICK, NULL,
                               button_event_cb, (void *)(uintptr_t)i);
        iot_button_register_cb(s_button_handles[i], BUTTON_LONG_PRESS_START, NULL,
                               button_event_cb, (void *)(uintptr_t)i);

        ESP_LOGI(TAG, "Button %d initialized on GPIO%ld (active low, pull-up)",
                 i, gpio_cfg.gpio_num);
    }

    ESP_LOGI(TAG, "All %d buttons initialized", MY_BUTTON_MAX);
    return ESP_OK;
}

esp_err_t my_buttons_register_cb(my_button_event_cb_t cb, void *user_data)
{
    s_user_cb = cb;
    s_user_data = user_data;
    return ESP_OK;
}

esp_err_t my_buttons_deinit(void)
{
    for (int i = 0; i < MY_BUTTON_MAX; i++) {
        if (s_button_handles[i] != NULL) {
            iot_button_delete(s_button_handles[i]);
            s_button_handles[i] = NULL;
        }
    }
    s_user_cb = NULL;
    s_user_data = NULL;
    return ESP_OK;
}