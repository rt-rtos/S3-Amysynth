#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Project file transfer: whole .amp project files (project_store.h layout)
 * sent to the device over UART0 and stored in a project slot. The core
 * (project_xfer.c) is the line protocol below and has no driver calls, so it
 * also builds on a host; project_xfer_uart.c is the device transport, the
 * UART0 reader. amp_xfer.py in this component is the host client.
 *
 * Protocol: line-based ASCII on UART0, beside console log output. Requests
 * start with "P>", replies with "P<"; a client keys on "P<" lines and ignores
 * the rest. Every request line gets exactly one reply line. The reply to the
 * last data line of a put comes from the UI task when the write has finished.
 *
 *   P> put <slot> <len> [<name>]  -> P< ok put ready <len>
 *        Starts a transfer, dropping any unfinished one. Pre-flight first
 *        (below).
 *   P> d <base64>                 -> P< ok d <received>
 *        Until len bytes have arrived; the last line's reply is the final
 *        result below.
 *   P> abort                      -> P< ok abort
 *        Also when nothing is in progress.
 *   P> ls                         -> P< slot <n> <size> <ver> <name>
 *                                    per used slot (ver = fmt_version, name
 *                                    last), then P< ok ls <count>
 *
 * - put pre-flight, on the calling task with project_store_slot_info(): an
 *   explicit slot that is used and has no '!' -> "P< err slot <n> in use";
 *   '-' with no free slot -> "P< err no free slot"; no transfer starts. The
 *   UI task's check at write time stays authoritative, with the same replies.
 * - abort after the last data line does not cancel the queued import: its
 *   "P< ok put ..." or err line still arrives.
 * - "err busy" means exactly "the import queue is full" (a zero-wait queue
 *   send). Once the UI task has taken a request, the next one is accepted
 *   and runs on the following frame.
 * - On any "P< err" during a put the client stops sending and sends abort
 *   (otherwise a dropped d line leaves a short transfer waiting).
 * - The per-line ack is load-bearing: the final write is a LittleFS write
 *   and erase with the flash cache off, and the UART ISR is not in IRAM
 *   (CONFIG_UART_ISR_IN_IRAM unset), so bytes arriving during it are lost.
 *   The client is waiting for the final reply then, hence its longer timeout.
 *
 * Fields:
 * - <slot>: '-' = first free slot; N (0..CONFIG_SYNTH_PROJECT_MAX_SLOTS-1) =
 *   that slot, refused if used; N! = that slot, replacing what is there.
 * - <len>: the whole .amp file (32-byte header + payload), 32..65536.
 * - <name>: the rest of the line after <len>, spaces included: the client
 *   sends the file name. Mapped by project_store_name_from(); missing or
 *   mapping to nothing: IMPORT.
 * - d lines: standard base64 alphabet, each line decodes on its own (length
 *   a multiple of 4, '=' padding only at the end of the line), at most 192
 *   characters (144 bytes). The client cuts the file into 144-byte pieces.
 *
 * Final results: "P< ok put <slot> <NAME>", "P< err no free slot",
 * "P< err slot <n> in use", "P< err write failed".
 * Other errors (the transfer is dropped where it says so):
 *   "P< err usage"                 bad put arguments
 *   "P< err too large"
 *   "P< err no memory"
 *   "P< err no transfer"           d without put
 *   "P< err bad base64"            dropped
 *   "P< err too much data"         dropped
 *   "P< err invalid project file"  project_store_check_image() refused the
 *                                  bytes; dropped
 *   "P< err busy"                  an import is already queued; dropped
 *   "P< err no project store"      CONFIG_SYNTH_PROJECT_STORE off
 *   "P< err unknown command"
 *   "P< err line too long"         the reader (project_xfer_init) */

/* Receives one whole reply line, no line ending; only valid during the call. */
typedef void (*project_xfer_reply_fn)(const char *line);
/* Receives one whole input line that is not a "P>" line, no line ending. */
typedef void (*project_xfer_line_fn)(const char *line);

#define PROJECT_XFER_LINE_MAX 256   /* reader line buffer, terminator included */
#define PROJECT_XFER_MAX_LEN  65536 /* largest <len> a put accepts */

/* Handle one "P>" request line.
 *
 * Obligations: line starts with "P>", has no line ending and is
 * NUL-terminated. reply is non-NULL and safe to call from the UI task (it
 * travels with the import request as its ctx). Called from one task only (the
 * transport's; a host build's main thread). Not ISR or render-path safe.
 *
 * Guarantees: one reply per request line, as in the protocol above. A
 * dropped transfer frees its buffer. On the last data line the buffer passes
 * to the UI task only if synth_ui_projects_request_import() accepted it;
 * otherwise it is freed here. Either way the module is idle again when the
 * call returns: the done callback, on the UI task, only formats the final
 * line and calls reply with it, touching no module state. Transfer state is
 * the module's only state, used by the calling task alone; at most one
 * transfer is in progress.
 *
 * Allocation: the file buffer, len bytes, heap_caps_malloc(MALLOC_CAP_SPIRAM)
 * at put, NULL-checked ("err no memory"). Nothing else allocates. */
void project_xfer_line(const char *line, project_xfer_reply_fn reply);

/* Device only (project_xfer_uart.c). Installs the UART0 RX driver (1024-byte
 * ring; TX untouched, console output keeps its default path) and starts the
 * reader task (Core 0, priority 3, 6144-byte stack).
 *
 * Reader: collects bytes up to '\n' and drops '\r'. A line starting with
 * "P>" goes to project_xfer_line() with a printf reply; any other non-empty
 * line goes to other (NULL: ignored), on the reader task. A line of
 * PROJECT_XFER_LINE_MAX or more characters is dropped whole; if it began
 * with "P>" the reply is "P< err line too long".
 *
 * Obligations: call once, from init, after synth_ui_init().
 * Guarantees: ESP_OK with the reader running; on failure an error is returned
 * with no driver installed and no task running. */
esp_err_t project_xfer_init(project_xfer_line_fn other);

#ifdef __cplusplus
}
#endif
