/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "priv_i2c_u8g2.h"
#include "u8g2.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_task.h"
#include "amy.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "rotary_encoder.h"
#include "sequencer_ui.h"
#include "usb_audio.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <sys/_intsup.h>
#include "esp_log.h"

static const char *TAG = "main"; // For ESP_LOG and related logs in this file

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "esp_adc/adc_oneshot.h"
#include "soc/gpio_num.h"
#include "my_buttons.h"
/*----------------------------------------------GPIO/MACROS-----------------------------------------------------------*/
//i2s pins
#define CONFIG_I2S_BCLK 11 // 25
#define CONFIG_I2S_LRCLK 9
#define CONFIG_I2S_DIN 10
// This can be 32 bit, int32_t -- helpful for digital output to a i2s->USB teensy3 board


// Potentiometer ADC channels
#define CONFIG_POT_ADC_CHANNEL ADC_CHANNEL_5 // GPIO6 on ESP32-S3 (ADC1_CH5)
// Physical GPIO for the pot (matches ADC_CHANNEL_3 on ESP32-S3)
#define POT_GPIO_NUM GPIO_NUM_6
// Rotary encoder pins
#define ENCODER_PIN_A GPIO_NUM_40
#define ENCODER_PIN_B GPIO_NUM_41
#define ENCODER_PIN_BTN GPIO_NUM_16

#define u8g2_task_stack_size (8 * 1024) // 8KB stack for the u8g2 task

#define CHANGE_DELTA_HZ 5.0f // Minimum frequency change in Hz to update the synth parameters, to avoid excessive event scheduling



typedef int16_t i2s_sample_type;



adc_oneshot_unit_handle_t pot_adc_handle;
static i2c_u8g2_handle_t s_display;
static u8g2_t *s_u8g2 = NULL;
static volatile int s_last_pot_raw = 0;
static volatile float s_last_pot_freq_hz = 0.0f;
// Last frequency that was sent to AMY (used for thresholding)
static volatile float s_last_sent_pot_freq_hz = 0.0f;
static volatile long last_count = 0; // For encoder count
static volatile long count = 0; // For encoder count
static volatile uint32_t s_last_seq_tick = 0;
static volatile uint32_t s_seq_tick_hook_count = 0;
static volatile uint32_t s_render_block_count = 0;
static volatile uint32_t s_last_render_sysclock_ms = 0;

static QueueHandle_t s_button_queue = NULL;

typedef struct {
    my_button_id_t id;
} button_msg_t;

static void main_sequencer_tick_hook(uint32_t tick_count)
{
    s_last_seq_tick = tick_count;
    s_seq_tick_hook_count++;
}

static void pot_log_task(void *pvParameters)
{
    (void)pvParameters;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        ESP_LOGI(TAG, "ADC: %d, Freq: %.1f Hz", s_last_pot_raw, s_last_pot_freq_hz);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(500));
    }
}
static void amy_usb_render_task(void *arg) {
    (void)arg;
    const uint64_t block_us = ((uint64_t)AMY_BLOCK_SIZE * 1000000ULL) / (uint64_t)AMY_SAMPLE_RATE;
    uint64_t next_deadline_us = (uint64_t)esp_timer_get_time();
    while (1) {
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (now_us < next_deadline_us) {
            uint64_t wait_us = next_deadline_us - now_us;
            TickType_t wait_ticks = pdMS_TO_TICKS((uint32_t)(wait_us / 1000ULL));
            if (wait_ticks > 0) {
                vTaskDelay(wait_ticks);
            } else {
                taskYIELD();
            }
            continue;
        }

        int16_t *block = amy_update();           // synthesizes everything / advances AMY sample clock
        if (block) {
            s_render_block_count++;
            s_last_render_sysclock_ms = amy_sysclock();
            esp_err_t write_err = usb_audio_write_stereo(block, AMY_BLOCK_SIZE);
            if (write_err == ESP_ERR_NO_MEM) {
                // USB ring buffer is full; briefly back off and try again.
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        next_deadline_us += block_us;
    }
}
// Button event callback: routes my_buttons events to sequencer UI actions
static void button_handler_task(void *pvParameters)
{
    (void)pvParameters;
    button_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_button_queue, &msg, portMAX_DELAY) == pdTRUE) {
            switch (msg.id) {
                case MY_BUTTON_0:
                    sequencer_ui_toggle_playing();
                    break;
                case MY_BUTTON_1:
                case MY_BUTTON_ENC:
                    sequencer_ui_handle_button();
                    break;
                default:
                    break;
            }
        }
    }
}

static void main_button_event_cb(my_button_id_t button_id, const char *event_str, void *user_data)
{
    (void)user_data;
    if (strcmp(event_str, "BUTTON_PRESS_DOWN") != 0) return;

    if (s_button_queue != NULL) {
        button_msg_t msg = { .id = button_id };
        (void)xQueueSend(s_button_queue, &msg, 0);
    } else {
        switch (button_id) {
            case MY_BUTTON_0:
                sequencer_ui_toggle_playing();
                break;
            case MY_BUTTON_1:
            case MY_BUTTON_ENC:
                sequencer_ui_handle_button();
                break;
            default:
                break;
        }
    }
}

//encoder
static void encoder_task(void *pvParameters)
{
    rotary_encoder_handle_t enc = (rotary_encoder_handle_t)pvParameters;
    long prev = last_count;
    for (;;) {
        long cur = rotary_encoder_get_count(enc);
        
        if (cur != prev) {
            long delta = cur - prev;
            ESP_LOGI(TAG,
                 "encoder count=%ld (delta=%ld)",
                  (long)cur, (long)(delta));
            last_count = prev;  // Store the previous count for display
            count = cur;        // Update current count
            prev = cur;
            sequencer_ui_handle_encoder(delta);
        }

        vTaskDelay(pdMS_TO_TICKS(20));  // Poll at 50Hz
    }
}

static void encoder_init_task(void *pvParameters)
{
    (void)pvParameters;
    // Delay init to avoid early-boot conflicts (PSRAM, console pins, etc.)
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "[encoder_init] starting after delay");
    // Button GPIO (GPIO16) is now managed by my_buttons/iot_button.

    rotary_encoder_config_t enc_cfg = rotary_encoder_default_config(ENCODER_PIN_A, ENCODER_PIN_B);
    rotary_encoder_handle_t enc = NULL;
    esp_err_t err = rotary_encoder_new_with_config(&enc_cfg, &enc);
    ESP_LOGI(TAG, "[encoder_init] rotary_encoder_new_with_config returned %d", err);
    if (err == ESP_OK && enc) {
        // Increase encoder task stack to avoid stack overflow when handling
        // amy_event-heavy operations (sequencer toggles create several
        // amy_event/delta conversions on the stack).
        xTaskCreate(encoder_task, "encoder_task", 8192, enc, 5, NULL);
    }

    vTaskDelete(NULL);
}




// I2C recover sequence will be performed inline in app_main.


// AMY synth states
extern struct state amy_global;



static amy_err_t setup_pot_adc(void) {
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &pot_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(pot_adc_handle, CONFIG_POT_ADC_CHANNEL, &chan_cfg));
    // Configure the GPIO pull to avoid floating inputs when the pot is
    // physically disconnected. This biases the input to a known state
    // and prevents unsafe sudden jumps in readings.
    gpio_set_direction(POT_GPIO_NUM, GPIO_MODE_INPUT);
    gpio_set_pull_mode(POT_GPIO_NUM, GPIO_PULLDOWN_ONLY);
    return AMY_OK;
}



/* 
static void update_tone_effect_from_pot(uint32_t now) {
    // Deprecated: function kept for compatibility but not used when pot_reader_task is active.
    (void)now;
}
*/

// Task: read the potentiometer, compute frequency and only trigger AMY update
// when the frequency change exceeds CHANGE_DELTA_HZ. This acts as the
// FreeRTOS "trigger" the user requested.
static void pot_reader_task(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        int raw = 0;
        if (adc_oneshot_read(pot_adc_handle, CONFIG_POT_ADC_CHANNEL, &raw) != ESP_OK) {
            // ADC failed, keep previous readings
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Quick stability check: take a few fast samples and ensure the
        // value isn't wildly fluctuating (which indicates a floating/unconnected pot).
       bool unstable = false;
        for (int i = 0; i < 2; ++i) {
            int tmp = 0;
            if (adc_oneshot_read(pot_adc_handle, CONFIG_POT_ADC_CHANNEL, &tmp) != ESP_OK) {
                unstable = true;
                break;
            }
            if (abs(tmp - raw) > 50) { // if samples vary more than ~50 counts, treat as unstable
                unstable = true;
                break;
            }
            raw = tmp;
        }
        if (unstable) {
            // Skip this cycle; keep last known good values to avoid sudden jumps
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        float normalized = (float)raw / 4095.0f;
        uint16_t bpm = 80 + (uint16_t)(normalized * 60.0f);

        // Update last-seen values for logging/UI
        s_last_pot_raw = raw;
        s_last_pot_freq_hz = bpm; // Reusing this variable for logging

        // Compare against last sent frequency and only trigger AMY when change
        // exceeds the configured threshold (CHANGE_DELTA_HZ)
        if (abs(bpm - (uint16_t)s_last_sent_pot_freq_hz) > 0) {
            sequencer_ui_set_bpm(bpm);

            // Remember the last value we sent
            s_last_sent_pot_freq_hz = bpm;
        }

        // Poll at a modest rate
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


    void app_main(void)
{
   
    ESP_LOGI(TAG, "Hello world!");
/*
    // I2C recover: Flush Sequence ensure SDA released and toggle SCL 9 times
    printf("[startup] before i2c_recover\n");
    gpio_set_direction(CONFIG_I2C_U8G2_SDA_GPIO, GPIO_MODE_INPUT);

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_I2C_U8G2_SCL_GPIO,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    for (int i = 0; i < 9; i++) {
        gpio_set_level(CONFIG_I2C_U8G2_SCL_GPIO, 0);
        ets_delay_us(5);
        gpio_set_level(CONFIG_I2C_U8G2_SCL_GPIO, 1);
        ets_delay_us(5);
    }
    printf("[startup] after i2c_recover\n");
*/
    ESP_LOGI(TAG, "[startup] before i2c_u8g2_init");
    // Display configuration is now managed through menuconfig (Kconfig)
    i2c_u8g2_config_t display_cfg = i2c_u8g2_config_default();
    ESP_LOGI(TAG, "[startup] display i2c cfg: SDA=%d SCL=%d Freq=%d Addr=0x%02X Timeout=%dms",
             (int)display_cfg.sda_io_num,
             (int)display_cfg.scl_io_num,
             (int)display_cfg.scl_speed_hz,
             (unsigned int)display_cfg.device_address,
             (int)display_cfg.timeout_ms);

    esp_err_t display_err = i2c_u8g2_init(&s_display, &display_cfg);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "[startup] i2c_u8g2_init failed: %s", esp_err_to_name(display_err));
        return;
    }
    s_u8g2 = i2c_u8g2_get_u8g2(&s_display);
    if (s_u8g2 == NULL) {
        ESP_LOGE(TAG, "[startup] i2c_u8g2_get_u8g2 returned NULL");
        return;
    }
    ESP_LOGI(TAG, "[startup] after i2c_u8g2_init");

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    ESP_LOGI(TAG, "silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Get flash size failed");
        return;
    }

    ESP_LOGI(TAG, "%" PRIu32 "MB %s flash", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    ESP_LOGI(TAG, "Minimum free heap size: %" PRIu32 " bytes", esp_get_minimum_free_heap_size());

    // Configure and start AMY
    amy_config_t amy_cfg = amy_default_config();
    amy_cfg.audio = AMY_AUDIO_IS_NONE; //changed from audio is none
    amy_cfg.amy_external_sequencer_hook = main_sequencer_tick_hook;
    ESP_LOGI(TAG, "Starting AMY synth engine... (audio=%d, Fs=%d)", amy_cfg.audio, AMY_SAMPLE_RATE);
    ESP_LOGI(TAG, "[startup] before amy_start");
    amy_start(amy_cfg);
    
    // Our USB Audio (must be after TinyUSB init)
    ESP_ERROR_CHECK(usb_audio_init());

    sequencer_ui_init(s_u8g2);
    ESP_LOGI(TAG, "[startup] after amy_start");
    
    TaskHandle_t amy_render_task_handle = NULL;
    BaseType_t render_task_ok;
#if CONFIG_FREERTOS_UNICORE
    render_task_ok = xTaskCreatePinnedToCore(amy_usb_render_task, "amy_render", 8192, NULL, 7, &amy_render_task_handle, 0);
#else
    render_task_ok = xTaskCreatePinnedToCore(amy_usb_render_task, "amy_render", 8192, NULL, 7, &amy_render_task_handle, 1);
#endif
    if (render_task_ok != pdPASS) {
        ESP_LOGW(TAG, "amy_render pinned task create failed (%ld), retrying unpinned", (long)render_task_ok);
        render_task_ok = xTaskCreate(amy_usb_render_task, "amy_render", 8192, NULL, 7, &amy_render_task_handle);
    }
    if (render_task_ok != pdPASS) {
        ESP_LOGE(TAG, "amy_render task create failed (%ld)", (long)render_task_ok);
    }

    ESP_LOGI(TAG, "AMY + USB Audio ready (48 kHz stereo to PC)");

    // Initialize push buttons (GPIO17, GPIO18, GPIO8, GPIO42)
    ESP_LOGI(TAG, "[startup] before my_buttons_init");
    s_button_queue = xQueueCreate(8, sizeof(button_msg_t));
    if (s_button_queue == NULL) {
        ESP_LOGW(TAG, "Button queue creation failed; callbacks will run inline");
    } else {
        if (xTaskCreate(button_handler_task, "button_task", 8192, NULL, 5, NULL) != pdPASS) {
            ESP_LOGW(TAG, "Button handler task creation failed");
            vQueueDelete(s_button_queue);
            s_button_queue = NULL;
        }
    }
    esp_err_t btn_err = my_buttons_init();
    if (btn_err != ESP_OK) {
        ESP_LOGE(TAG, "my_buttons_init failed: %s", esp_err_to_name(btn_err));
    } else {
        ESP_LOGI(TAG, "[startup] after my_buttons_init");
        my_buttons_register_cb(main_button_event_cb, NULL);
    }

        // Setup rotary encoder
    // Defer rotary encoder initialization to a task to avoid early-boot conflicts
    xTaskCreate(encoder_init_task, "encoder_init_task", 2048, NULL, 5, NULL);


    // Setup ADC for frequency pot
    ESP_LOGI(TAG, "[startup] before setup_pot_adc");
    setup_pot_adc();
    ESP_LOGI(TAG, "[startup] after setup_pot_adc");
    
   
    ESP_LOGI(TAG, "Scheduling test tone on OSC 0...");
   // start_test_tone(start_time + 200);

    

    xTaskCreate(
        pot_log_task,
         "pot_log_task",
         2048,
         NULL,
         4
         ,
         NULL);

    // Start pot reader task which will trigger AMY updates only when the
    // frequency change exceeds CHANGE_DELTA_HZ.
    xTaskCreate(
        pot_reader_task,
        "pot_reader_task",
        6144,
        NULL,
        4,
        NULL);

    ESP_LOGI(TAG, "Main loop started: pot reader task running");
    // Idle loop; pot_reader_task handles all pot->synth updates.
    while (1) {
        ESP_LOGI(TAG,
                 "Main loop idle... seq_tick=%" PRIu32 " tick_hook_calls=%" PRIu32 " render_blocks=%" PRIu32 " render_sysclock_ms=%" PRIu32,
                 s_last_seq_tick, s_seq_tick_hook_count, s_render_block_count, s_last_render_sysclock_ms);
        vTaskDelay(pdMS_TO_TICKS(5000));        }
    }

