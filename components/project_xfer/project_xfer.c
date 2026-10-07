/* Project transfer core: the "P>" line protocol (protocol and contracts in
 * include/project_xfer.h). No driver or RTOS calls, so it builds on a host;
 * the device transport is project_xfer_uart.c. */

#include "sdkconfig.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "project_store.h"
#include "synth_ui.h"
#include "project_xfer.h"

#if CONFIG_SYNTH_PROJECT_STORE

#define B64_LINE_MAX   192                      /* chars per d line */
#define B64_BYTES_MAX  (B64_LINE_MAX / 4 * 3)   /* 144 */
#define REPLY_MAX      96

/* The one transfer in progress (buf != NULL), owned by the calling task. */
static struct {
    uint8_t *buf;
    size_t   len;
    size_t   received;
    int      slot;                      /* -1 = first free */
    bool     force;
    char     name[PROJECT_NAME_LEN];
} s_xfer;

static void xfer_drop(void)
{
    heap_caps_free(s_xfer.buf);
    memset(&s_xfer, 0, sizeof(s_xfer));
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

static const char *skip_spaces(const char *p)
{
    while (is_space(*p)) p++;
    return p;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode one d line on its own: n a multiple of 4, '=' only as the last one
 * or two characters. Returns the byte count, or -1 if the text is invalid. */
static int b64_decode(const char *in, size_t n, uint8_t out[B64_BYTES_MAX])
{
    if (n == 0 || n % 4 != 0 || n > B64_LINE_MAX) return -1;
    size_t pad = 0;
    if (in[n - 1] == '=') pad++;
    if (in[n - 2] == '=') {
        if (pad == 0) return -1;
        pad++;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        bool last = (i + 4 == n);
        uint32_t v = 0;
        for (size_t k = 0; k < 4; k++) {
            int d;
            if (last && k >= 4 - pad) {
                d = 0;
            } else {
                d = b64_value(in[i + k]);
                if (d < 0) return -1;
            }
            v = (v << 6) | (uint32_t)d;
        }
        out[o++] = (uint8_t)(v >> 16);
        if (!last || pad < 2) out[o++] = (uint8_t)(v >> 8);
        if (!last || pad < 1) out[o++] = (uint8_t)v;
    }
    return (int)o;
}

/* Runs on the UI task (synth_ui_import_done_fn): ctx is the reply function
 * project_xfer_line() was given. Touches no module state. */
static void import_done(synth_ui_import_result_t r, uint8_t slot,
                        const char *name, void *ctx)
{
    char out[REPLY_MAX];
    switch (r) {
        case SYNTH_UI_IMPORT_OK:
            snprintf(out, sizeof(out), "P< ok put %u %s", (unsigned)slot, name);
            break;
        case SYNTH_UI_IMPORT_NO_FREE_SLOT:
            snprintf(out, sizeof(out), "P< err no free slot");
            break;
        case SYNTH_UI_IMPORT_SLOT_USED:
            snprintf(out, sizeof(out), "P< err slot %u in use", (unsigned)slot);
            break;
        case SYNTH_UI_IMPORT_WRITE_FAILED:
        default:
            snprintf(out, sizeof(out), "P< err write failed");
            break;
    }
    ((project_xfer_reply_fn)ctx)(out);
}

/* "<slot> <len> [<name>]" */
static void cmd_put(const char *args, project_xfer_reply_fn reply)
{
    char out[REPLY_MAX];
    xfer_drop();

    const char *p = skip_spaces(args);
    int  slot  = -1;
    bool force = false;
    if (*p == '-') {
        p++;
    } else if (*p >= '0' && *p <= '9') {
        unsigned n = 0;
        while (*p >= '0' && *p <= '9' && n < CONFIG_SYNTH_PROJECT_MAX_SLOTS) {
            n = n * 10u + (unsigned)(*p++ - '0');
        }
        if (n >= CONFIG_SYNTH_PROJECT_MAX_SLOTS) { reply("P< err usage"); return; }
        slot = (int)n;
        if (*p == '!') { force = true; p++; }
    } else {
        reply("P< err usage");
        return;
    }
    if (!is_space(*p)) { reply("P< err usage"); return; }

    p = skip_spaces(p);
    if (!(*p >= '0' && *p <= '9')) { reply("P< err usage"); return; }
    size_t len = 0;
    bool too_large = false;
    while (*p >= '0' && *p <= '9') {
        if (len <= PROJECT_XFER_MAX_LEN) len = len * 10u + (size_t)(*p - '0');
        if (len > PROJECT_XFER_MAX_LEN) too_large = true;
        p++;
    }
    if (*p != '\0' && !is_space(*p)) { reply("P< err usage"); return; }
    if (too_large) { reply("P< err too large"); return; }
    if (len < 32) { reply("P< err usage"); return; }

    project_slot_info_t info;
    if (slot >= 0 && !force) {
        if (project_store_slot_info((uint8_t)slot, &info) && info.used) {
            snprintf(out, sizeof(out), "P< err slot %d in use", slot);
            reply(out);
            return;
        }
    } else if (slot < 0) {
        bool any_free = false;
        for (uint8_t i = 0; i < CONFIG_SYNTH_PROJECT_MAX_SLOTS && !any_free; i++) {
            any_free = project_store_slot_info(i, &info) && !info.used;
        }
        if (!any_free) { reply("P< err no free slot"); return; }
    }

    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (buf == NULL) { reply("P< err no memory"); return; }

    s_xfer.buf      = buf;
    s_xfer.len      = len;
    s_xfer.received = 0;
    s_xfer.slot     = slot;
    s_xfer.force    = force;
    project_store_name_from(*p ? p : NULL, s_xfer.name);

    snprintf(out, sizeof(out), "P< ok put ready %u", (unsigned)len);
    reply(out);
}

static void cmd_d(const char *args, project_xfer_reply_fn reply)
{
    char out[REPLY_MAX];
    if (s_xfer.buf == NULL) { reply("P< err no transfer"); return; }

    const char *p = skip_spaces(args);
    size_t n = strlen(p);
    uint8_t chunk[B64_BYTES_MAX];
    int got = b64_decode(p, n, chunk);
    if (got < 0) { xfer_drop(); reply("P< err bad base64"); return; }
    if ((size_t)got > s_xfer.len - s_xfer.received) {
        xfer_drop();
        reply("P< err too much data");
        return;
    }
    memcpy(s_xfer.buf + s_xfer.received, chunk, (size_t)got);
    s_xfer.received += (size_t)got;

    if (s_xfer.received < s_xfer.len) {
        snprintf(out, sizeof(out), "P< ok d %u", (unsigned)s_xfer.received);
        reply(out);
        return;
    }

    const uint8_t *payload = NULL;
    size_t plen = 0;
    if (!project_store_check_image(s_xfer.buf, s_xfer.len, &payload, &plen, NULL)) {
        xfer_drop();
        reply("P< err invalid project file");
        return;
    }
    /* Accepted: the buffer now belongs to synth_ui and the final reply comes
     * from import_done on the UI task. */
    if (synth_ui_projects_request_import(s_xfer.buf, s_xfer.len, s_xfer.slot,
                                         s_xfer.force, s_xfer.name,
                                         import_done, (void *)reply)) {
        memset(&s_xfer, 0, sizeof(s_xfer));
        return;
    }
    xfer_drop();
    reply("P< err busy");
}

static void cmd_ls(project_xfer_reply_fn reply)
{
    char out[REPLY_MAX];
    unsigned count = 0;
    for (uint8_t i = 0; i < CONFIG_SYNTH_PROJECT_MAX_SLOTS; i++) {
        project_slot_info_t info;
        if (!project_store_slot_info(i, &info) || !info.used) continue;
        snprintf(out, sizeof(out), "P< slot %u %lu %u %s", (unsigned)i,
                 (unsigned long)info.size_bytes, (unsigned)info.fmt_version,
                 info.name);
        reply(out);
        count++;
    }
    snprintf(out, sizeof(out), "P< ok ls %u", count);
    reply(out);
}

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encode n (1..B64_BYTES_MAX) bytes; out takes 4 * ceil(n / 3) + 1 chars. */
static void b64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64_CHARS[(v >> 18) & 63];
        out[o++] = B64_CHARS[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64_CHARS[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64_CHARS[v & 63] : '=';
    }
    out[o] = '\0';
}

/* "<slot>": the slot file as d lines, then the ok line. */
static void cmd_get(const char *args, project_xfer_reply_fn reply)
{
    char out[REPLY_MAX];
    const char *p = skip_spaces(args);
    unsigned slot = 0;
    if (!(*p >= '0' && *p <= '9')) { reply("P< err usage"); return; }
    while (*p >= '0' && *p <= '9' && slot < CONFIG_SYNTH_PROJECT_MAX_SLOTS) {
        slot = slot * 10u + (unsigned)(*p++ - '0');
    }
    if (slot >= CONFIG_SYNTH_PROJECT_MAX_SLOTS || *skip_spaces(p) != '\0') {
        reply("P< err usage");
        return;
    }

    project_slot_info_t info;
    if (!project_store_slot_info((uint8_t)slot, &info) || !info.used) {
        snprintf(out, sizeof(out), "P< err slot %u empty", slot);
        reply(out);
        return;
    }
    uint8_t *img = NULL;
    size_t len = 0;
    char name[PROJECT_NAME_LEN];
    if (!project_store_read_file((uint8_t)slot, &img, &len, name)) {
        reply("P< err read failed");
        return;
    }
    char line[5 + B64_LINE_MAX + 1] = "P< d ";
    for (size_t off = 0; off < len; off += B64_BYTES_MAX) {
        size_t n = len - off < B64_BYTES_MAX ? len - off : B64_BYTES_MAX;
        b64_encode(img + off, n, line + 5);
        reply(line);
    }
    heap_caps_free(img);
    snprintf(out, sizeof(out), "P< ok get %u %u %s", slot, (unsigned)len, name);
    reply(out);
}

/* Match a command word: cmd followed by the end of the line or a space. */
static const char *match_cmd(const char *p, const char *cmd)
{
    size_t n = strlen(cmd);
    if (strncmp(p, cmd, n) != 0) return NULL;
    if (p[n] != '\0' && !is_space(p[n])) return NULL;
    return p + n;
}

void project_xfer_line(const char *line, project_xfer_reply_fn reply)
{
    const char *p = skip_spaces(line + 2);   /* past "P>" */
    const char *args;
    if ((args = match_cmd(p, "put")) != NULL) {
        cmd_put(args, reply);
    } else if ((args = match_cmd(p, "d")) != NULL) {
        cmd_d(args, reply);
    } else if (match_cmd(p, "abort") != NULL) {
        xfer_drop();
        reply("P< ok abort");
    } else if (match_cmd(p, "ls") != NULL) {
        cmd_ls(reply);
    } else if ((args = match_cmd(p, "get")) != NULL) {
        cmd_get(args, reply);
    } else {
        reply("P< err unknown command");
    }
}

#else /* !CONFIG_SYNTH_PROJECT_STORE */

void project_xfer_line(const char *line, project_xfer_reply_fn reply)
{
    (void)line;
    reply("P< err no project store");
}

#endif /* CONFIG_SYNTH_PROJECT_STORE */
