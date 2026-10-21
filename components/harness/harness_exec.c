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
#include "amy.h"
#include "amy_helpers.h"
#include "sequencer_core.h"
#include "synth_ui.h"
#include "lfo_bank.h"
#include "synth_slots.h"   /* LFO_SYNTH_MAX */
#include "sequencer.h"     /* sequencer_ticks */
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
    REPLY("ok wire_zlp=%" PRIu32
          " ring_overrun=%" PRIu32 " render_overrun=%" PRIu32,
          d.wire_zlp, d.ring_overrun, d.render_overrun);
}

/* One "LFOBANK" line per live shared LFO synth (lfo_bank.h), with its
 * carrier's running phase read from AMY's osc table under amy_queue_lock
 * (-1 when the synth has no osc yet), then the count and the bar grid's
 * reference: the current tick and the bar baseline. */
static void cmd_st_lfobank(harness_reply_fn reply)
{
    unsigned used = 0;
    for (int i = 0; i < LFO_SYNTH_MAX; i++) {
        uint16_t refs;
        bool pending;
        seq_lfo_t key;
        uint8_t n = lfo_bank_info((uint8_t)i, &refs, &pending, &key);
        if (n == LFO_BANK_NONE) continue;
        float phase = -1.0f;
        amy_grab_lock();
        uint16_t osc = synth_first_osc[n];
        if (AMY_IS_SET(osc) && osc < AMY_OSCS && synth[osc] != NULL)
            phase = P2F(synth[osc]->phase);
        amy_release_lock();
        printf("LFOBANK %u refs=%u pending=%d wave=%u rate=%u wob=%u,%u,%u"
               " phase=%.4f\n",
               (unsigned)n, (unsigned)refs, pending ? 1 : 0,
               (unsigned)key.wave, (unsigned)key.rate, (unsigned)key.wob_rate,
               (unsigned)key.wob_depth, (unsigned)key.wob_reach, (double)phase);
        used++;
    }
    REPLY("ok n=%u tick=%" PRIu32 " base=%" PRIu32, used, sequencer_ticks(),
          sequencer_core_bar_baseline_tick());
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

/* "amy.wire <msg>" parses one AMY wire message (no trailing Z) into an event
 * and sends it through amy_helpers, the app's only path into AMY: queued, not
 * applied, by the time the reply prints. An osc number in a message without
 * a synth is absolute. */
static void cmd_amy_wire(char *args, harness_reply_fn reply)
{
    if (*args == '\0') { REPLY("err empty message"); return; }
    amy_event *e = amy_helpers_event_begin();
    amy_parse_message(args, e);
    amy_helpers_event_send(e);
    REPLY("ok");
}

/* "amy.voices synth=<n>" prints "VOICE <voice> <base_osc> L=<m0>,<m1>" per
 * voice of the synth: the absolute osc numbers a raw-osc amy.wire message
 * needs, and the base osc's two mod sources (-1 = unset). Reads AMY's voice
 * and osc tables under amy_queue_lock. */
static void cmd_amy_voices(char *args, harness_reply_fn reply)
{
    const char *v = arg_value(args, "synth");
    if (v == NULL) { REPLY("err need synth=<n>"); return; }
    uint16_t voices[MAX_VOICES_PER_INSTRUMENT];
    uint16_t base[MAX_VOICES_PER_INSTRUMENT];
    int mod[MAX_VOICES_PER_INSTRUMENT][NUM_MOD_SOURCES];
    amy_grab_lock();
    int n = instrument_get_num_voices((int)strtol(v, NULL, 10), voices);
    for (int i = 0; i < n; i++) {
        if (!amy_voice_base_osc(voices[i], &base[i])) base[i] = UINT16_MAX;
        for (int m = 0; m < NUM_MOD_SOURCES; m++) {
            mod[i][m] = (base[i] != UINT16_MAX && synth[base[i]] != NULL &&
                         AMY_IS_SET(synth[base[i]]->mod_source[m]))
                        ? (int)synth[base[i]]->mod_source[m] : -1;
        }
    }
    amy_release_lock();
    for (int i = 0; i < n; i++) {
        printf("VOICE %u %u L=%d,%d\n", (unsigned)voices[i], (unsigned)base[i], mod[i][0], mod[i][1]);
    }
    REPLY("ok n=%d", n);
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
    else if (strcmp(line, "st.lfobank") == 0) cmd_st_lfobank(reply);
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
    else if (strcmp(line, "amy.wire") == 0)   cmd_amy_wire(args, reply);
    else if (strcmp(line, "amy.voices") == 0) cmd_amy_voices(args, reply);
    else if (strcmp(line, "in.btn") == 0)  cmd_in_btn(hooks, args, reply);
    else if (strcmp(line, "in.enc") == 0)  cmd_in_enc(hooks, args, reply);
    else REPLY("err unknown cmd '%s'", line);
}

#endif /* CONFIG_DEV_SERIAL_HARNESS */
