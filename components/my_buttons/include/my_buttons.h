/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include "iot_button.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Button identifiers
 *
 * Each name is a ROLE; the pin it lands on comes from this list's order,
 * because s_button_gpios[] (my_buttons.c) is positional. Reordering members
 * here reassigns roles to physical buttons without touching the wiring list.
 * Keep each comment's GPIO in step with the matching row of that array.
 */
typedef enum {
    MY_BUTTON_SHOULDER, // GPIO15, left shoulder (LB) → per-view: step toggle on the grid (second one,
                        //          alongside the encoder press), EG1 sweep
                        //          polarity in the envelope editor
    MY_BUTTON_1,        // GPIO18 → patch-select hold (hold + encoder); in editors:
                        //          filter enable, EG curve-type cycle
    MY_BUTTON_2,        // GPIO8
    MY_BUTTON_3,        // GPIO42 (GPIO3 is strapping pin, avoided)
    MY_BUTTON_ENC,      // GPIO16 → encoder push button (step toggle)
    MY_BUTTON_0,        // GPIO17 → layer cycle (tap) / play-stop (long); commit
                        //          or cancel an open editor
    MY_BUTTON_SHIFT,    // GPIO47, right shoulder (RB) → hold modifier for the SHIFT+1/2/3 chords
    MY_BUTTON_MAX
} my_button_id_t;

/**
 * @brief Button event callback type
 *
 * @param button_id  Which button triggered the event
 * @param event      The button_event_t enum value (e.g. BUTTON_PRESS_DOWN)
 * @param user_data  User data passed during registration
 */
typedef void (*my_button_event_cb_t)(my_button_id_t button_id, button_event_t event, void *user_data);

/**
 * @brief Initialize all buttons
 * 
 * Creates GPIO buttons on pins 15, 18, 8, 42, 16, 17, 47 with active low
 * configuration and internal pull-ups enabled.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t my_buttons_init(void);

/**
 * @brief Register a callback for button events
 * 
 * @param cb Callback function to invoke on button events
 * @param user_data User data passed to callback
 * @return ESP_OK on success
 */
esp_err_t my_buttons_register_cb(my_button_event_cb_t cb, void *user_data);

/**
 * @brief Deinitialize and free button resources
 * 
 * @return ESP_OK on success
 */
esp_err_t my_buttons_deinit(void);

#ifdef __cplusplus
}
#endif
