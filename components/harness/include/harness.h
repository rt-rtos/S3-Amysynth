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
 * iot_button event enum (the hook provider owns the mapping). */
typedef enum {
    HARNESS_BTN_DOWN = 0,   /* press down */
    HARNESS_BTN_UP,         /* release */
    HARNESS_BTN_CLICK,      /* single click (down+up gesture result) */
    HARNESS_BTN_LONG,       /* long-press start */
} harness_btn_act_t;

/* Injection hooks, provided by the application (main.c on the device) so the
 * harness never reaches into another component's statics.
 *
 * Execution context: both hooks are called from inside harness_exec(), on
 * its caller's task: on the device the harness command task (Core 0, low
 * priority). inject_button must post to the same queue the
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

/* Longest command line harness_exec() takes, terminator included. */
#define HARNESS_LINE_MAX    160

/* Receives one whole response line ("H< ok ..." or "H< err ..."), no line
 * ending. `line` is only valid during the call. */
typedef void (*harness_reply_fn)(const char *line);

/* Run one command line: the interpreter behind the UART task, with no
 * transport of its own, so another front end can drive the same grammar.
 *
 * `cmdline` is one line without its line ending, with or without the "H>"
 * sentinel; it is not modified. `hooks` may be NULL (in.btn and in.enc then
 * answer err). `reply` must not be NULL.
 *
 * Guarantees: `reply` is called exactly once per command, last; a blank line
 * is not a command and gets no reply. A line of HARNESS_LINE_MAX characters
 * or more is refused with "err line too long" and nothing runs. Dumps that
 * commands trigger (st.seqdump, st.render, st.dropts) go to stdout from the
 * modules that print them, before the reply, not through `reply`.
 *
 * Execution context: task context, the context the commands' own calls need
 * (sequencer transport setters, the hooks above). Keeps no state between
 * calls; not ISR or render-path safe. Uses about 700 bytes of stack. Only
 * built under CONFIG_DEV_SERIAL_HARNESS. */
#if CONFIG_DEV_SERIAL_HARNESS
void harness_exec(const harness_hooks_t *hooks, const char *cmdline,
                  harness_reply_fn reply);
#endif

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
