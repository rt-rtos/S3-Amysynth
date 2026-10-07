/* Project transfer device transport: the UART0 RX driver and the reader task
 * that splits input into "P>" lines for project_xfer_line() and everything
 * else for the caller's line handler (contract in include/project_xfer.h). */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "project_xfer.h"

static const char *TAG = "project_xfer";

#define XFER_UART        UART_NUM_0
#define XFER_RX_BUF      1024
#define XFER_TASK_STACK  6144
#define XFER_TASK_PRIO   3

static project_xfer_line_fn s_other = NULL;
static bool                 s_started = false;

/* Replies go through printf: raw lines on the console UART, no ESP_LOG
 * prefix, so the host keys on "P<" lines regardless of interleaved logs.
 * Console TX busy-waits on the FIFO (no UART driver on the TX side), so a
 * get's data lines, ~17 ms each at 115200, would keep this task runnable
 * for seconds and starve IDLE0 into the task watchdog; one tick after each
 * lets it run. */
static void reply_console(const char *line)
{
    printf("%s\n", line);
    if (strncmp(line, "P< d ", 5) == 0) vTaskDelay(1);
}

static bool is_xfer_line(const char *line)
{
    return line[0] == 'P' && line[1] == '>';
}

static void reader_task(void *arg)
{
    (void)arg;
    static char line[PROJECT_XFER_LINE_MAX];
    size_t len = 0;
    bool overflow = false;

    for (;;) {
        uint8_t ch;
        int n = uart_read_bytes(XFER_UART, &ch, 1, pdMS_TO_TICKS(100));
        if (n != 1) continue;
        if (ch == '\r') continue;
        if (ch != '\n') {
            if (len < sizeof(line) - 1) {
                line[len++] = (char)ch;
            } else {
                overflow = true;   /* keep line[0..1] for the "P>" test */
            }
            continue;
        }
        line[len] = '\0';
        if (overflow) {
            if (is_xfer_line(line)) reply_console("P< err line too long");
        } else if (is_xfer_line(line)) {
            project_xfer_line(line, reply_console);
        } else if (len > 0 && s_other != NULL) {
            s_other(line);
        }
        len = 0;
        overflow = false;
    }
}

esp_err_t project_xfer_init(project_xfer_line_fn other)
{
    if (s_started) return ESP_ERR_INVALID_STATE;

    /* RX driver only: console TX (logs, replies via printf) keeps using the
     * default non-driver path; both coexist on UART0. */
    esp_err_t err = uart_driver_install(XFER_UART, XFER_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_other = other;
    if (xTaskCreatePinnedToCore(reader_task, "uart0_rx",
                                XFER_TASK_STACK, NULL,
                                XFER_TASK_PRIO, NULL, 0) != pdPASS) {
        uart_driver_delete(XFER_UART);
        s_other = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "listening on UART0");
    return ESP_OK;
}
