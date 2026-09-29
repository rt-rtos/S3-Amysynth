#pragma once

#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Serial test harness: a line-command control channel on UART0 for driving a
 * headless instance (input injection, transport, state dumps). Compiled in
 * only under CONFIG_DEV_SERIAL_HARNESS; every symbol here is a no-op stub
 * otherwise.
 *
 * Protocol (line-oriented ASCII on the console UART, coexists with log
 * output): commands arrive as "H> <cmd> [args]", every command gets exactly
 * one "H< ok ..." or "H< err <reason>" response line. Bulk dumps triggered by
 * commands (seq dump, render stats) print their own existing formats between
 * command and response; hosts key on the H< line for completion. */

/* Button actions in protocol terms, decoupled from the underlying
 * iot_button event enum (main.c owns the mapping). */
typedef enum {
    HARNESS_BTN_DOWN = 0,   /* press down */
    HARNESS_BTN_UP,         /* release */
    HARNESS_BTN_CLICK,      /* single click (down+up gesture result) */
    HARNESS_BTN_LONG,       /* long-press start */
} harness_btn_act_t;

/* Injection hooks, provided by main.c so the harness never reaches into
 * another component's statics.
 *
 * Execution context: both hooks are called on the harness command task
 * (Core 0, low priority). inject_button must post to the same queue the
 * physical button callback posts to (same drop-on-full policy); it must not
 * block. inject_encoder_steps runs the encoder step-routing chain directly
 * and therefore assumes NO concurrent physical encoder activity - the
 * harness targets a bare board (contract: harness builds run without
 * encoders attached; on a populated board, injected and physical encoder
 * processing may interleave mid-gesture). */
typedef struct {
    void (*inject_button)(int button_id, harness_btn_act_t act);
    void (*inject_encoder_steps)(long steps);
} harness_hooks_t;

/* Start the harness: installs the UART0 RX driver and spawns the command
 * task (Core 0, priority 3, below input/UI dispatch).
 *
 * Obligations: call ONCE from init context after the button queue and
 * sequencer are up (hooks must be valid for the lifetime of the system;
 * pass a pointer to static storage). Not render-path or ISR safe.
 * Guarantees: on failure returns an error with nothing running and no UART
 * driver installed - the rest of the firmware is unaffected (degrade =
 * harness absent, never fatal). With CONFIG_DEV_SERIAL_HARNESS off, returns
 * ESP_OK having done nothing: "harness absent" is the success case there, so
 * callers need no #if of their own and the hooks they pass fold away. */
#if CONFIG_DEV_SERIAL_HARNESS
esp_err_t harness_init(const harness_hooks_t *hooks);
#else
static inline esp_err_t harness_init(const harness_hooks_t *hooks)
{
    (void)hooks;
    return ESP_OK;
}
#endif

#ifdef __cplusplus
}
#endif
