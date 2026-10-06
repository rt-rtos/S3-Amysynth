/* Serial test harness command interpreter: harness_exec(), contract in
 * include/harness.h. No transport here; harness.c feeds it lines from UART0.
 * Everything below compiles out when CONFIG_DEV_SERIAL_HARNESS is off. */

#include "sdkconfig.h"

#if CONFIG_DEV_SERIAL_HARNESS

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <inttypes.h>

#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_heap_caps.h"

#include "harness.h"
#include "sequencer_core.h"
#include "synth_ui.h"
#if CONFIG_SYNTH_PROJECT_STORE
#include "project_templates.h"
#endif
#include "dropout_stats.h"
#include "render_stats.h"
#include "usb_device_uac.h"

/* Longest reply is the unknown-command echo of a full line. */
#define HARNESS_REPLY_MAX   (HARNESS_LINE_MAX + 32)

static void replyf(harness_reply_fn reply, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void replyf(harness_reply_fn reply, const char *fmt, ...)
{
    char buf[HARNESS_REPLY_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    reply(buf);
}

/* One whole response line to the `reply` in scope. */
#define REPLY(fmt, ...) replyf(reply, "H< " fmt, ##__VA_ARGS__)

/* Return the value of "key=" in args, or NULL. Args is the mutable tail of
 * the command line; values are space-delimited. */
static char *arg_value(char *args, const char *key)
{
    size_t klen = strlen(key);
    for (char *tok = strtok(args, " "); tok; tok = strtok(NULL, " ")) {
        if (strncmp(tok, key, klen) == 0 && tok[klen] == '=') {
            return tok + klen + 1;
        }
    }
    return NULL;
}

static void cmd_sys_build(harness_reply_fn reply)
{
    const esp_app_desc_t *app = esp_app_get_description();
    REPLY("ok fw=%s proj=%s idf=%s", app->version, app->project_name,
          esp_get_idf_version());
}

static void cmd_st_heap(harness_reply_fn reply)
{
    REPLY("ok internal=%u largest=%u min=%u psram=%u",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void cmd_st_drop(harness_reply_fn reply)
{
    dropout_stats_t d;
    dropout_stats_get(&d);
    REPLY("ok wire_zlp=%" PRIu32 " ring_underrun=%" PRIu32
          " ring_overrun=%" PRIu32 " render_overrun=%" PRIu32
          " chunk_drop=%" PRIu32,
          d.wire_zlp, d.ring_underrun, d.ring_overrun, d.render_overrun,
          d.chunk_drop);
}

#if CONFIG_AMYSYNTH_DROPOUT_TS
static void cmd_st_dropts(harness_reply_fn reply)
{
    /* Harness task only, so a static bounce buffer beats 1 KB of stack. */
    static int64_t ts[DROPOUT_TS_RING];
    uint32_t seq;
    uint32_t n = dropout_ts_snapshot(ts, &seq);
    for (uint32_t i = 0; i < n; i++) {
        printf("DROPTS %" PRIu32 " %lld\n", seq - n + i, (long long)ts[i]);
    }
    REPLY("ok seq=%" PRIu32 " n=%" PRIu32 " ring=%d", seq, n, DROPOUT_TS_RING);
}
#endif

static void cmd_in_btn(const harness_hooks_t *hooks, char *args,
                       harness_reply_fn reply)
{
    if (hooks == NULL || hooks->inject_button == NULL) {
        REPLY("err no button hook");
        return;
    }
    /* arg_value consumes args via strtok; grab both keys in one pass by
     * scanning the copies. */
    char copy[HARNESS_LINE_MAX];
    strlcpy(copy, args, sizeof(copy));
    char *id_s = arg_value(copy, "id");
    char copy2[HARNESS_LINE_MAX];
    strlcpy(copy2, args, sizeof(copy2));
    char *act_s = arg_value(copy2, "act");
    if (id_s == NULL || act_s == NULL) {
        REPLY("err usage: in.btn id=<n> act=down|up|click|long");
        return;
    }
    harness_btn_act_t act;
    if      (strcmp(act_s, "down")  == 0) act = HARNESS_BTN_DOWN;
    else if (strcmp(act_s, "up")    == 0) act = HARNESS_BTN_UP;
    else if (strcmp(act_s, "click") == 0) act = HARNESS_BTN_CLICK;
    else if (strcmp(act_s, "long")  == 0) act = HARNESS_BTN_LONG;
    else { REPLY("err bad act '%s'", act_s); return; }

    hooks->inject_button(atoi(id_s), act);
    REPLY("ok");
}

static void cmd_in_enc(const harness_hooks_t *hooks, char *args,
                       harness_reply_fn reply)
{
    if (hooks == NULL || hooks->inject_encoder_steps == NULL) {
        REPLY("err no encoder hook");
        return;
    }
    char *d_s = arg_value(args, "delta");
    if (d_s == NULL) { REPLY("err usage: in.enc delta=<n>"); return; }
    long steps = strtol(d_s, NULL, 10);
    if (steps == 0) { REPLY("err zero delta"); return; }
    hooks->inject_encoder_steps(steps);
    REPLY("ok");
}

static void cmd_tr_bpm(char *args, harness_reply_fn reply)
{
    char *tok = strtok(args, " ");
    if (tok != NULL) {
        long bpm = strtol(tok, NULL, 10);
        if (bpm <= 0 || bpm > UINT16_MAX) { REPLY("err bad bpm"); return; }
        sequencer_core_set_bpm((uint16_t)bpm);
    }
    REPLY("ok bpm=%u", (unsigned)sequencer_core_get_bpm());
}

#if CONFIG_SYNTH_PROJECT_STORE
/* "pr.tpl" lists the built-in templates as "TPL <i> <name>" lines; "pr.tpl
 * <name or index>" queues one for the UI task to load. Names carry spaces,
 * so the whole argument is the name. */
static void cmd_pr_tpl(char *args, harness_reply_fn reply)
{
    size_t n = project_templates_count();
    for (size_t len = strlen(args); len > 0 && args[len - 1] == ' '; len--) args[len - 1] = '\0';
    if (*args == '\0') {
        for (size_t i = 0; i < n; i++) printf("TPL %u %s\n", (unsigned)i, project_templates_name(i));
        REPLY("ok n=%u", (unsigned)n);
        return;
    }
    char *end;
    size_t i = (size_t)strtoul(args, &end, 10);
    if (*end != '\0') {
        for (i = 0; i < n; i++) {
            if (strcasecmp(args, project_templates_name(i)) == 0) break;
        }
    }
    if (i >= n) { REPLY("err unknown template '%s'", args); return; }
    if (!synth_ui_projects_request_template(i)) { REPLY("err a load or save is queued"); return; }
    REPLY("ok tpl=%u %s", (unsigned)i, project_templates_name(i));
}
#endif

void harness_exec(const harness_hooks_t *hooks, const char *cmdline,
                  harness_reply_fn reply)
{
    /* The parser cuts the line up in place (strtok), so work on a copy. */
    char buf[HARNESS_LINE_MAX];
    if (strlcpy(buf, cmdline, sizeof(buf)) >= sizeof(buf)) {
        REPLY("err line too long");
        return;
    }
    char *line = buf;

    /* Strip the command sentinel; tolerate its absence so hand-typed
     * commands in a terminal work too. */
    if (strncmp(line, "H>", 2) == 0) line += 2;
    while (*line == ' ') line++;
    if (*line == '\0') return;          /* blank/log echo - ignore silently */

    char *args = strchr(line, ' ');
    if (args != NULL) { *args++ = '\0'; } else { args = line + strlen(line); }

    if      (strcmp(line, "sys.echo") == 0)  REPLY("ok %s", args);
    else if (strcmp(line, "sys.build") == 0) cmd_sys_build(reply);
    else if (strcmp(line, "st.heap") == 0)   cmd_st_heap(reply);
    else if (strcmp(line, "st.drop") == 0)   cmd_st_drop(reply);
    else if (strcmp(line, "st.uacpull") == 0) {
#if CONFIG_AMYSYNTH_DROPOUT_TS
        uint32_t c[4];
        int64_t t_us;
        if (uac_device_get_pull_stats(c, &t_us) == ESP_OK) {
            REPLY("ok t_us=%lld preload=%" PRIu32 " skip_room=%" PRIu32
                  " pull=%" PRIu32 " bytes=%" PRIu32,
                  (long long)t_us, c[0], c[1], c[2], c[3]);
        } else {
            REPLY("err pull stats unavailable");
        }
#else
        REPLY("err dropout ts not compiled in");
#endif
    }
    else if (strcmp(line, "st.dropts") == 0) {
#if CONFIG_AMYSYNTH_DROPOUT_TS
        cmd_st_dropts(reply);
#else
        REPLY("err dropout ts not compiled in");
#endif
    }
    else if (strcmp(line, "st.render") == 0) {
#if CONFIG_AMYSYNTH_RENDER_STATS
        render_stats_report();              /* prints its own block */
        REPLY("ok");
#else
        REPLY("err render_stats not compiled in");
#endif
    }
    else if (strcmp(line, "st.seqdump") == 0) {
#if CONFIG_SYNTH_DEV_MENU
        sequencer_core_dump_state();        /* prints SEQDUMP BEGIN/END */
        REPLY("ok");
#else
        REPLY("err dev menu not compiled in");
#endif
    }
    else if (strcmp(line, "tr.play") == 0) { sequencer_core_set_playing(true);  REPLY("ok"); }
    else if (strcmp(line, "tr.stop") == 0) { sequencer_core_set_playing(false); REPLY("ok"); }
    else if (strcmp(line, "tr.bpm") == 0)  cmd_tr_bpm(args, reply);
    else if (strcmp(line, "pr.tpl") == 0) {
#if CONFIG_SYNTH_PROJECT_STORE
        cmd_pr_tpl(args, reply);
#else
        REPLY("err project store not compiled in");
#endif
    }
    else if (strcmp(line, "in.btn") == 0)  cmd_in_btn(hooks, args, reply);
    else if (strcmp(line, "in.enc") == 0)  cmd_in_enc(hooks, args, reply);
    else REPLY("err unknown cmd '%s'", line);
}

#endif /* CONFIG_DEV_SERIAL_HARNESS */
