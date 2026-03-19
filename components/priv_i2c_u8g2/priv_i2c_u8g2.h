#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2C_U8G2_TX_BUFFER_SIZE 132

typedef void (*i2c_u8g2_setup_fn_t)(u8g2_t *u8g2,
                                    const u8g2_cb_t *rotation,
                                    u8x8_msg_cb byte_cb,
                                    u8x8_msg_cb gpio_and_delay_cb);

typedef struct {
    i2c_port_num_t i2c_port;
    gpio_num_t sda_io_num;
    gpio_num_t scl_io_num;
    uint32_t scl_speed_hz;
    uint16_t timeout_ms;
    uint8_t device_address;
    bool enable_internal_pullup;
    const u8g2_cb_t *rotation;
    i2c_u8g2_setup_fn_t setup_fn;
} i2c_u8g2_config_t;

typedef struct {
    u8g2_t u8g2;
    i2c_master_bus_handle_t i2c_bus_handle;
    i2c_master_dev_handle_t display_dev_handle;
    uint16_t timeout_ms;
    uint32_t scl_speed_hz;
    bool initialized;
    uint8_t device_address;
    uint8_t tx_buffer[I2C_U8G2_TX_BUFFER_SIZE];
    size_t tx_buffer_len;
} i2c_u8g2_handle_t;

/**
 * @brief Fill a config with menuconfig-backed defaults.
 */
i2c_u8g2_config_t i2c_u8g2_config_default(void);

/**
 * @brief Initialize I2C bus and U8G2 display.
 *
 * If config->setup_fn is NULL, defaults to SSD1306 128x64 noname full buffer setup.
 */
esp_err_t i2c_u8g2_init(i2c_u8g2_handle_t *handle, const i2c_u8g2_config_t *config);

/**
 * @brief Deinitialize display device and I2C bus.
 */
esp_err_t i2c_u8g2_deinit(i2c_u8g2_handle_t *handle);

/**
 * @brief Get the U8G2 object for draw operations.
 */
u8g2_t *i2c_u8g2_get_u8g2(i2c_u8g2_handle_t *handle);

/**
 * @brief Set display power save mode.
 */
esp_err_t i2c_u8g2_set_power_save(i2c_u8g2_handle_t *handle, bool enable);

void demo_text_display(u8g2_t *u8g2);


#ifdef __cplusplus
}
#endif
