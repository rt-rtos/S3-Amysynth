/* Serial test harness transport: the UART0 command task. It reads lines and
 * hands them to harness_exec() (harness_exec.c); protocol and contracts are in
 * include/harness.h. Everything below compiles out when
 * CONFIG_DEV_SERIAL_HARNESS is off. */

#include "sdkconfig.h"

#if CONFIG_DEV_SERIAL_HARNESS

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "harness.h"

static const char *TAG = "harness";

#define HARNESS_UART        UART_NUM_0
#define HARNESS_RX_BUF      1024
#define HARNESS_TASK_STACK  6144
#define HARNESS_TASK_PRIO   3

static const harness_hooks_t *s_hooks = NULL;

/* Responses go through printf like the seq dump does: raw lines on the
 * console UART, no ESP_LOG prefix, so the host parser keys on "H<" columns
 * regardless of interleaved log output. */
static void reply_console(const char *line)
{
    printf("%s\n", line);
}

static void harness_task(void *arg)
{
    (void)arg;
    static char line[HARNESS_LINE_MAX];
    size_t len = 0;

    reply_console("H< ok harness ready");
    for (;;) {
        uint8_t ch;
        int n = uart_read_bytes(HARNESS_UART, &ch, 1, pdMS_TO_TICKS(100));
        if (n != 1) continue;
        if (ch == '\r') continue;
        if (ch != '\n') {
            if (len < sizeof(line) - 1) {
                line[len++] = (char)ch;
            } else {
                len = 0;                    /* oversize: drop the whole line */
                reply_console("H< err line too long");
            }
            continue;
        }
        line[len] = '\0';
        len = 0;
        harness_exec(s_hooks, line, reply_console);
    }
}

esp_err_t harness_init(const harness_hooks_t *hooks)
{
    if (hooks == NULL) return ESP_ERR_INVALID_ARG;
    if (s_hooks != NULL) return ESP_ERR_INVALID_STATE;

    /* RX driver only: console TX (logs, replies via printf) keeps using the
     * default non-driver path; both coexist on UART0. */
    esp_err_t err = uart_driver_install(HARNESS_UART, HARNESS_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_hooks = hooks;
    if (xTaskCreatePinnedToCore(harness_task, "harness_cmd",
                                HARNESS_TASK_STACK, NULL,
                                HARNESS_TASK_PRIO, NULL, 0) != pdPASS) {
        uart_driver_delete(HARNESS_UART);
        s_hooks = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "serial harness listening on UART0");
    return ESP_OK;
}

#endif /* CONFIG_DEV_SERIAL_HARNESS */
