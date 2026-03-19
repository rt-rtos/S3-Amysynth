#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"
#include "priv_i2c_u8g2.h"

static const char *TAG = "i2c_u8g2";

// Use component-level Kconfig values.
#define I2C_MASTER_NUM CONFIG_I2C_U8G2_I2C_PORT
#define I2C_MASTER_SDA_IO CONFIG_I2C_U8G2_SDA_GPIO
#define I2C_MASTER_SCL_IO CONFIG_I2C_U8G2_SCL_GPIO
#define I2C_FREQ_HZ CONFIG_I2C_U8G2_FREQ_HZ
#define I2C_DISPLAY_ADDRESS CONFIG_I2C_U8G2_DISPLAY_ADDRESS

static i2c_u8g2_handle_t *s_active_handle = NULL;
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;
static i2c_master_dev_handle_t s_display_dev_handle = NULL;
static uint32_t s_scl_speed_hz = 0;
static uint16_t s_timeout_ms = 1000;
static uint8_t s_device_address = 0x3C;

static inline i2c_u8g2_handle_t *i2c_u8g2_handle_from_u8x8(u8x8_t *u8x8)
{
    (void)u8x8;
    return s_active_handle;
}

static uint8_t i2c_u8g2_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    static uint8_t buffer[I2C_U8G2_TX_BUFFER_SIZE];
    static uint8_t buf_idx;

    switch (msg) {
    case U8X8_MSG_BYTE_INIT: {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = s_device_address,
            .scl_speed_hz = s_scl_speed_hz,
            .scl_wait_us = 0,
            .flags.disable_ack_check = false,
        };

        esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus_handle, &dev_config,
                                                  &s_display_dev_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add display device to I2C bus (%s)", esp_err_to_name(ret));
            return 0;
        }

        i2c_u8g2_handle_t *handle = i2c_u8g2_handle_from_u8x8(u8x8);
        if (handle != NULL) {
            handle->display_dev_handle = s_display_dev_handle;
        }

        ESP_LOGI(TAG, "I2C master driver initialized successfully");
        break;
    }

    case U8X8_MSG_BYTE_START_TRANSFER:
        buf_idx = 0;
        break;

    case U8X8_MSG_BYTE_SET_DC:
        break;

    case U8X8_MSG_BYTE_SEND:
        for (size_t i = 0; i < arg_int; ++i) {
            if (buf_idx >= sizeof(buffer)) {
                ESP_LOGE(TAG, "U8G2 transfer buffer overflow (%u >= %u)",
                         (unsigned int)buf_idx,
                         (unsigned int)sizeof(buffer));
                return 0;
            }
            buffer[buf_idx++] = *((uint8_t *)arg_ptr + i);
        }
        break;

    case U8X8_MSG_BYTE_END_TRANSFER:
        if (buf_idx > 0 && s_display_dev_handle != NULL) {
            esp_err_t ret = i2c_master_transmit(s_display_dev_handle, buffer, buf_idx, s_timeout_ms);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "I2C transfer failed (%s)", esp_err_to_name(ret));
                return 0;
            }
        }
        break;

    default:
        return 0;
    }

    return 1;
}

static uint8_t i2c_u8g2_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg,
                                          uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)arg_ptr;

    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        ESP_LOGI(TAG, "GPIO and delay initialization completed");
        break;

    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        break;

    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us(arg_int * 10);
        break;

    case U8X8_MSG_DELAY_100NANO:
        __asm__ __volatile__("nop");
        break;

    case U8X8_MSG_DELAY_I2C:
        if (arg_int == 0) {
            return 1;
        }
        esp_rom_delay_us(5 / arg_int);
        break;

    case U8X8_MSG_GPIO_RESET:
        break;

    default:
        return 0;
    }

    return 1;
}

i2c_u8g2_config_t i2c_u8g2_config_default(void)
{
    i2c_u8g2_config_t config = {
        .i2c_port = (i2c_port_num_t)I2C_MASTER_NUM,
        .sda_io_num = (gpio_num_t)I2C_MASTER_SDA_IO,
        .scl_io_num = (gpio_num_t)I2C_MASTER_SCL_IO,
        .scl_speed_hz = I2C_FREQ_HZ,
        .timeout_ms = CONFIG_I2C_U8G2_TIMEOUT_MS,
        .device_address = I2C_DISPLAY_ADDRESS,
        .enable_internal_pullup = CONFIG_I2C_U8G2_INTERNAL_PULLUP,
        .rotation = U8G2_R0,
        .setup_fn = NULL,
    };
    return config;
}

esp_err_t i2c_u8g2_init(i2c_u8g2_handle_t *handle, const i2c_u8g2_config_t *config)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "timeout_ms must be > 0");
    ESP_RETURN_ON_FALSE(config->scl_speed_hz > 0, ESP_ERR_INVALID_ARG, TAG, "scl_speed_hz must be > 0");
    ESP_RETURN_ON_FALSE(config->device_address <= 0x7F, ESP_ERR_INVALID_ARG, TAG,
                        "device_address must be 7-bit");

    memset(handle, 0, sizeof(*handle));
    handle->timeout_ms = config->timeout_ms;
    handle->scl_speed_hz = config->scl_speed_hz;
    handle->device_address = config->device_address;

    s_timeout_ms = config->timeout_ms;
    s_scl_speed_hz = config->scl_speed_hz;
    s_device_address = config->device_address;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = config->i2c_port,
        .sda_io_num = config->sda_io_num,
        .scl_io_num = config->scl_io_num,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = config->enable_internal_pullup,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus_handle),
                        TAG, "Failed to create I2C master bus");

    /* Brief delay to allow display to stabilize after bus init */
    vTaskDelay(pdMS_TO_TICKS(10));

    handle->i2c_bus_handle = s_i2c_bus_handle;

    i2c_u8g2_setup_fn_t setup_fn = config->setup_fn;
    if (setup_fn == NULL) {
        setup_fn = u8g2_Setup_ssd1315_i2c_128x64_noname_f;
    }

    setup_fn(&handle->u8g2,
             config->rotation != NULL ? config->rotation : U8G2_R0,
             i2c_u8g2_byte_cb,
             i2c_u8g2_gpio_and_delay_cb);

    s_active_handle = handle;

    u8g2_InitDisplay(&handle->u8g2);
    u8g2_SetPowerSave(&handle->u8g2, 0);

    /* Test draw to verify I2C communication works */
    u8g2_ClearBuffer(&handle->u8g2);
    u8g2_SetFont(&handle->u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(&handle->u8g2, 0, 12, "Init OK");
    u8g2_SendBuffer(&handle->u8g2);
    ESP_LOGI(TAG, "Test draw completed");

    handle->initialized = true;
    return ESP_OK;
}

esp_err_t i2c_u8g2_deinit(i2c_u8g2_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    if (handle->display_dev_handle != NULL) {
        ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(handle->display_dev_handle),
                            TAG, "Failed to remove display device");
        handle->display_dev_handle = NULL;
        s_display_dev_handle = NULL;
    }

    if (handle->i2c_bus_handle != NULL) {
        ESP_RETURN_ON_ERROR(i2c_del_master_bus(handle->i2c_bus_handle),
                            TAG, "Failed to delete I2C master bus");
        handle->i2c_bus_handle = NULL;
        s_i2c_bus_handle = NULL;
    }

    if (s_active_handle == handle) {
        s_active_handle = NULL;
    }

    handle->initialized = false;
    return ESP_OK;
}

u8g2_t *i2c_u8g2_get_u8g2(i2c_u8g2_handle_t *handle)
{
    if (handle == NULL || !handle->initialized) {
        return NULL;
    }
    return &handle->u8g2;
}

esp_err_t i2c_u8g2_set_power_save(i2c_u8g2_handle_t *handle, bool enable)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(handle->initialized, ESP_ERR_INVALID_STATE, TAG, "display not initialized");

    u8g2_SetPowerSave(&handle->u8g2, enable ? 1 : 0);
    return ESP_OK;
}

void demo_shapes(u8g2_t *u8g2)
{
    ESP_LOGI(TAG, "Geometric Shapes");

    u8g2_ClearBuffer(u8g2);

    /* Title */
    u8g2_SetFont(u8g2, u8g2_font_ncenB08_tr);
    u8g2_DrawStr(u8g2, 35, 12, "Shapes");

    /* Filled rectangle */
    u8g2_DrawBox(u8g2, 10, 20, 20, 15);

    /* Outlined rectangle */
    u8g2_DrawFrame(u8g2, 35, 20, 20, 15);

    /* Circle outline */
    u8g2_DrawCircle(u8g2, 70, 27, 8, U8G2_DRAW_ALL);

    /* Filled circle */
    u8g2_DrawDisc(u8g2, 95, 27, 6, U8G2_DRAW_ALL);

    /* Lines */
    u8g2_DrawLine(u8g2, 10, 45, 50, 45);
    u8g2_DrawLine(u8g2, 10, 50, 30, 60);
    u8g2_DrawLine(u8g2, 30, 50, 50, 60);

    /* Triangle */
    u8g2_DrawTriangle(u8g2, 70, 45, 85, 60, 55, 60);

    u8g2_SendBuffer(u8g2);
}
