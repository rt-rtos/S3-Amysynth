/* Serial test harness front end: lines arrive from project_xfer's UART0
 * reader (it owns UART0 input) and run through harness_exec()
 * (harness_exec.c); protocol and contracts are in include/harness.h.
 * Everything below compiles out when CONFIG_DEV_SERIAL_HARNESS is off. */

#include "sdkconfig.h"

#if CONFIG_DEV_SERIAL_HARNESS

#include <stdio.h>

#include "esp_log.h"

#include "harness.h"

static const char *TAG = "harness";

/* Written once by harness_init(); until then harness_exec() runs with NULL
 * hooks, which it accepts (in.btn and in.enc answer err). */
static const harness_hooks_t *s_hooks = NULL;

/* Responses go through printf like the seq dump does: raw lines on the
 * console UART, no ESP_LOG prefix, so the host parser keys on "H<" columns
 * regardless of interleaved log output. */
static void reply_console(const char *line)
{
    printf("%s\n", line);
}

void harness_handle_line(const char *line)
{
    harness_exec(s_hooks, line, reply_console);
}

esp_err_t harness_init(const harness_hooks_t *hooks)
{
    if (hooks == NULL) return ESP_ERR_INVALID_ARG;
    if (s_hooks != NULL) return ESP_ERR_INVALID_STATE;

    s_hooks = hooks;
    reply_console("H< ok harness ready");
    ESP_LOGI(TAG, "serial harness listening on UART0");
    return ESP_OK;
}

#endif /* CONFIG_DEV_SERIAL_HARNESS */
