#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROTARY_ENCODER_DEFAULT_HIGH_LIMIT  1000
#define ROTARY_ENCODER_DEFAULT_LOW_LIMIT  -1000
#define ROTARY_ENCODER_DEFAULT_QUEUE_SIZE  10
#define ROTARY_ENCODER_DEFAULT_GLITCH_NS   5000 // 5 microseconds, suitable for typical mechanical encoders

typedef struct rotary_encoder_s *rotary_encoder_handle_t;

/**
 * Configuration for rotary_encoder_new_with_config().
 */
typedef struct {
    gpio_num_t pin_a;                 /**< GPIO connected to phase A */
    gpio_num_t pin_b;                 /**< GPIO connected to phase B */
    int32_t high_limit;               /**< PCNT high limit */
    int32_t low_limit;                /**< PCNT low limit */
    const int32_t *watch_points;      /**< Array of watch-point values */
    size_t watch_point_count;         /**< Number of watch-point entries */
    uint32_t glitch_filter_ns;        /**< Glitch filter (ns), 0 to disable */
    size_t event_queue_size;          /**< Length of the watch-point queue */
} rotary_encoder_config_t;

/**
 * Return a config pre-filled with sensible defaults for the given encoder pins.
 */
rotary_encoder_config_t rotary_encoder_default_config(gpio_num_t pin_a, gpio_num_t pin_b);

/**
 * Create and start a quadrature rotary encoder (X4 mode) using hardware PCNT.
 * Uses the provided config structure for limits, queue size, glitch filter, etc.
 */
esp_err_t rotary_encoder_new_with_config(const rotary_encoder_config_t *config, rotary_encoder_handle_t *out_handle);

/**
 * Convenience helper for the most common configuration.
 */
esp_err_t rotary_encoder_new(gpio_num_t pin_a, gpio_num_t pin_b, rotary_encoder_handle_t *out_handle);

/**
 * Get current signed count (position).
 */
int32_t rotary_encoder_get_count(rotary_encoder_handle_t handle);

/**
 * Reset counter to zero.
 */
esp_err_t rotary_encoder_reset(rotary_encoder_handle_t handle);

/**
 * Get the internal event queue (watch-point events are sent here).
 */
QueueHandle_t rotary_encoder_get_event_queue(rotary_encoder_handle_t handle);

/**
 * Delete encoder and free resources.
 */
esp_err_t rotary_encoder_delete(rotary_encoder_handle_t handle);

#ifdef __cplusplus
}
#endif
