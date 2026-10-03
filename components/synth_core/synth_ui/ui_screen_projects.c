#include "sdkconfig.h"

#if CONFIG_SYNTH_PROJECT_STORE

#include "synth_ui/synth_ui_internal.h"
#include "project_store.h"
#include "project_fs.h"
#include "project_snapshot.h"
#include "project_templates.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ════════════════════════════════════════════════════════════════════════
 *  PROJECTS PAGE (persistent project storage)
 * ════════════════════════════════════════════════════════════════════════
 * Item model for the Projects page of the menu overlay (page state and input
 * routing live in ui_screen_menu.c, mirroring the global-FX page).
 *
 * Layout: item 0 = Back, item 1 = a read-only storage-usage line, item 2 =
 * "Templates..", items 3..N+2 = one row per slot; the per-slot action flow is
 * in CONTROLS.md. The Templates row swaps the item list for the template
 * list (item 0 = Back to the Templates row, items 1..T = one row per built-in
 * template, project_templates.h), whose only action is Load. There is no
 * toast overlay, so action results are shown inline in the acted-on row's
 * value field until the cursor moves off it. */

typedef enum { PA_LOAD = 0, PA_SAVE, PA_REN, PA_DEL, PA_EXIT, PA_COUNT } proj_action_t;

#define PROJ_ROW_TEMPLATES 2   /* "Templates.." */
#define PROJ_ROW_SLOT0     3   /* first slot row */

/* Encoder alphabet for the rename editor: A-Z, 0-9, space, '-', then the
 * '#' end sentinel that commits the name early. */
static const char PROJ_NAME_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -#";

static const char *TAG = "proj_menu";

static menu_item_view_t     s_items[PROJ_ROW_SLOT0 + CONFIG_SYNTH_PROJECT_MAX_SLOTS];
static project_slot_info_t  s_info[CONFIG_SYNTH_PROJECT_MAX_SLOTS];
static bool                 s_dirty = true;   /* rebuild s_info from the FS */
static bool                 s_tpl_list = false;   /* showing the template list */

static proj_action_t s_action   = PA_LOAD;
static bool          s_armed    = false;      /* Save/overwrite second-click */
static bool          s_renaming = false;
static char          s_name_buf[PROJECT_NAME_LEN];
static uint8_t       s_name_pos = 0;

/* Inline status message shown in place of a slot's normal value, cleared as
 * soon as the cursor leaves that row. */
static bool    s_status_active = false;
static uint8_t s_status_idx    = 0;
static char    s_status_msg[MENU_VALUE_LEN];

/* Deferred Load/Save request, executed by projects_menu_service() on the
 * synth_ui task (the s_layers applier, sequencer_core.h). Save defers too,
 * keeping project flash I/O off the input path. */
typedef enum { PREQ_NONE = 0, PREQ_LOAD, PREQ_SAVE, PREQ_LOAD_TPL } proj_req_t;
static volatile proj_req_t s_req = PREQ_NONE;   /* set last: publishes the fields below */
static uint8_t             s_req_slot     = 0;   /* template index for PREQ_LOAD_TPL */
static uint8_t             s_req_idx      = 0;
static bool                s_req_has_name = false;
static char                s_req_name[PROJECT_NAME_LEN];

static void set_status(uint8_t idx, const char *msg)
{
    s_status_active = true;
    s_status_idx    = idx;
    snprintf(s_status_msg, sizeof(s_status_msg), "%s", msg);
    ESP_LOGI(TAG, "projects: %s", msg);
}

static bool slot_corrupt(const project_slot_info_t *info)
{
    return info->used && strcmp(info->name, "<corrupt>") == 0;
}

static void fmt_label(char *out, size_t outsz, uint8_t slot,
                      const project_slot_info_t *info)
{
    if (info->used && !slot_corrupt(info) && info->name[0] != '\0') {
        snprintf(out, outsz, "%s", info->name);
    } else {
        snprintf(out, outsz, "P%02u", (unsigned)(slot + 1));
    }
}

static void fmt_value(char *out, size_t outsz, const project_slot_info_t *info)
{
    if (!info->used) {
        snprintf(out, outsz, "--");
    } else if (slot_corrupt(info)) {
        snprintf(out, outsz, "ERR");
    } else {
        unsigned kb = (unsigned)((info->size_bytes + 1023u) / 1024u);
        snprintf(out, outsz, "%uK", kb);
    }
}

static void fmt_bytes(char *out, size_t outsz, size_t bytes)
{
    if (bytes < 1024u * 1024u) {
        snprintf(out, outsz, "%uK", (unsigned)((bytes + 1023u) / 1024u));
    } else {
        float mb = (float)bytes / (1024.0f * 1024.0f);
        snprintf(out, outsz, "%.1fM", (double)mb);
    }
}

static const char *action_name(proj_action_t a, bool armed)
{
    switch (a) {
        case PA_LOAD: return "Load";
        case PA_SAVE: return armed ? "Save!" : "Save";
        case PA_REN:  return "Ren";
        case PA_DEL:  return "Del";
        case PA_EXIT: return "Exit";
        default:      return "?";
    }
}

/* Compact rename-editor value: the bracketed char under the cursor plus a
 * 1-based position/length pair. A full name window does not fit
 * MENU_VALUE_LEN, and this beats a scrolling text window for legibility. */
static void fmt_rename_value(char *out, size_t outsz)
{
    char c = s_name_buf[s_name_pos];
    if (c == '\0') c = '_';
    snprintf(out, outsz, "[%c]%u/%u", c,
             (unsigned)(s_name_pos + 1), (unsigned)(PROJECT_NAME_LEN - 1));
}

static int char_index(char c)
{
    if (c == '\0') c = ' ';
    const char *p = strchr(PROJ_NAME_CHARS, c);
    return p ? (int)(p - PROJ_NAME_CHARS) : 0;
}

static void enter_rename(uint8_t slot)
{
    const project_slot_info_t *info = &s_info[slot];
    memset(s_name_buf, 0, sizeof(s_name_buf));
    if (info->used && !slot_corrupt(info)) {
        snprintf(s_name_buf, sizeof(s_name_buf), "%s", info->name);
    }
    s_name_pos = 0;
    s_renaming = true;
}

static void commit_rename(uint8_t idx, uint8_t slot)
{
    bool ok = project_store_rename(slot, s_name_buf);
    set_status(idx, ok ? "RENAMED" : "REN FAIL");
    s_renaming = false;
    s_armed    = false;
    s_dirty    = true;
}

/* Explicit Save/Discard for the rename editor (MY_BUTTON_1 / MY_BUTTON_2).
 * Unlike the click path, whose return value the menu router feeds back into
 * seq_state.menu_editing, these are reached straight from the button handler
 * and so must end the editing sub-state themselves. Both derive the acted-on
 * row from the menu cursor, which stays parked on the slot being renamed. */
bool projects_menu_is_renaming(void)
{
    return s_renaming;
}

void projects_menu_rename_commit(void)
{
    if (!s_renaming) return;
    uint8_t idx  = (uint8_t)seq_state.menu_cursor;
    uint8_t slot = (uint8_t)(idx - PROJ_ROW_SLOT0);
    commit_rename(idx, slot);        /* clears s_renaming, sets status + s_dirty */
    seq_state.menu_editing = false;  /* back to browsing the slot list */
}

void projects_menu_rename_cancel(void)
{
    if (!s_renaming) return;
    s_renaming = false;
    s_armed    = false;
    seq_state.menu_editing = false;
    set_status((uint8_t)seq_state.menu_cursor, "CANCEL");
}

/* Template rows shown: the table, capped to what s_items can hold. */
static uint8_t tpl_rows(void)
{
    size_t n   = project_templates_count();
    size_t cap = sizeof(s_items) / sizeof(s_items[0]) - 1u;
    return (uint8_t)(n < cap ? n : cap);
}

/* Leaving the template list (its Back row, or a reset) lands on the
 * Templates row of the project list. */
static void close_tpl_list(void)
{
    s_tpl_list            = false;
    s_status_active       = false;
    seq_state.menu_cursor = PROJ_ROW_TEMPLATES;
}

void projects_menu_reset(void)
{
    if (s_tpl_list) close_tpl_list();
    s_dirty         = true;
    s_action        = PA_LOAD;
    s_armed         = false;
    s_renaming      = false;
    s_status_active = false;
}

uint8_t projects_menu_item_count(void)
{
    if (s_tpl_list) return (uint8_t)(1 + tpl_rows());
    return (uint8_t)(PROJ_ROW_SLOT0 + CONFIG_SYNTH_PROJECT_MAX_SLOTS);
}

/* The template list's Back row returns to the project list, not the main
 * menu, so it goes through projects_menu_handle_click() as a value row. */
bool projects_menu_item_is_back(uint8_t idx)
{
    return idx == 0 && !s_tpl_list;
}

bool projects_menu_item_is_value(uint8_t idx)
{
    if (s_tpl_list) return idx < 1 + tpl_rows();
    return idx >= PROJ_ROW_TEMPLATES
        && idx < PROJ_ROW_SLOT0 + (uint8_t)CONFIG_SYNTH_PROJECT_MAX_SLOTS;
}

static const menu_item_view_t *build_tpl_items(void)
{
    if (s_status_active && seq_state.menu_cursor != s_status_idx) {
        s_status_active = false;
    }

    snprintf(s_items[0].label, MENU_LABEL_LEN, "< Back");
    s_items[0].value[0] = '\0';

    uint8_t n = tpl_rows();
    for (uint8_t i = 0; i < n; i++) {
        uint8_t idx = (uint8_t)(1 + i);
        snprintf(s_items[idx].label, MENU_LABEL_LEN, "%s", project_templates_name(i));
        if (seq_state.menu_editing && seq_state.menu_cursor == idx) {
            snprintf(s_items[idx].value, MENU_VALUE_LEN, "%s", action_name(PA_LOAD, false));
        } else if (s_status_active && s_status_idx == idx) {
            snprintf(s_items[idx].value, MENU_VALUE_LEN, "%s", s_status_msg);
        } else {
            s_items[idx].value[0] = '\0';
        }
    }
    return s_items;
}

const menu_item_view_t *projects_menu_build_items(void)
{
    if (s_tpl_list) return build_tpl_items();

    if (s_dirty) {
        for (uint8_t i = 0; i < CONFIG_SYNTH_PROJECT_MAX_SLOTS; i++) {
            if (!project_store_slot_info(i, &s_info[i])) {
                s_info[i].used = false;
            }
        }
        s_dirty = false;
    }

    /* A status message belongs to the row its action ran on, so moving off
     * that row clears it and it can never linger on the wrong slot. */
    if (s_status_active && seq_state.menu_cursor != s_status_idx) {
        s_status_active = false;
    }

    snprintf(s_items[0].label, MENU_LABEL_LEN, "< Back");
    s_items[0].value[0] = '\0';

    snprintf(s_items[1].label, MENU_LABEL_LEN, "Storage");
    {
        size_t total = 0, used = 0;
        if (project_fs_ok() && project_fs_stats(&total, &used)) {
            /* 6 chars each so "u/t" provably fits the value field, which is
             * what keeps -Werror=format-truncation satisfied. */
            char u[7], t[7];
            fmt_bytes(u, sizeof(u), used);
            fmt_bytes(t, sizeof(t), total);
            snprintf(s_items[1].value, MENU_VALUE_LEN, "%s/%s", u, t);
        } else {
            snprintf(s_items[1].value, MENU_VALUE_LEN, "ERR");
        }
    }

    snprintf(s_items[PROJ_ROW_TEMPLATES].label, MENU_LABEL_LEN, "Templates..");
    s_items[PROJ_ROW_TEMPLATES].value[0] = '\0';

    for (uint8_t slot = 0; slot < CONFIG_SYNTH_PROJECT_MAX_SLOTS; slot++) {
        uint8_t idx = (uint8_t)(PROJ_ROW_SLOT0 + slot);
        const project_slot_info_t *info = &s_info[slot];

        fmt_label(s_items[idx].label, MENU_LABEL_LEN, slot, info);

        bool editing_this = seq_state.menu_editing && seq_state.menu_cursor == idx;
        if (editing_this && s_renaming) {
            fmt_rename_value(s_items[idx].value, MENU_VALUE_LEN);
        } else if (editing_this) {
            snprintf(s_items[idx].value, MENU_VALUE_LEN, "%s",
                     action_name(s_action, s_armed));
        } else if (s_status_active && s_status_idx == idx) {
            snprintf(s_items[idx].value, MENU_VALUE_LEN, "%s", s_status_msg);
        } else {
            fmt_value(s_items[idx].value, MENU_VALUE_LEN, info);
        }
    }

    return s_items;
}

void projects_menu_edit_value(uint8_t idx, int delta)
{
    if (!projects_menu_item_is_value(idx)) return;
    /* Template rows have Load only; the Templates row has no value. */
    if (s_tpl_list || idx == PROJ_ROW_TEMPLATES) return;
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    if (dir == 0) return;

    if (s_renaming) {
        int n = (int)(sizeof(PROJ_NAME_CHARS) - 1);
        int cur = char_index(s_name_buf[s_name_pos]);
        cur = ((cur + dir) % n + n) % n;
        s_name_buf[s_name_pos] = PROJ_NAME_CHARS[cur];
        return;
    }

    int a = ((int)s_action + dir) % (int)PA_COUNT;
    if (a < 0) a += PA_COUNT;
    s_action = (proj_action_t)a;
    s_armed  = false;   /* action changed: any pending confirm is void */
}

/* Click on item idx. Returns the desired seq_state.menu_editing value: true
 * keeps the row's action/rename sub-state open, false executes or aborts and
 * returns to browsing the slot list. */
bool projects_menu_handle_click(uint8_t idx)
{
    if (!projects_menu_item_is_value(idx)) return false;
    /* A queued load/save is still in flight: ignore clicks so a second request
     * cannot clobber the pending one's fields. */
    if (s_req != PREQ_NONE) return seq_state.menu_editing;

    if (s_tpl_list) {
        if (idx == 0) {
            close_tpl_list();
            return false;
        }
        if (!seq_state.menu_editing) return true;   /* show the Load action */
        s_req_slot = (uint8_t)(idx - 1);
        s_req_idx  = idx;
        s_req      = PREQ_LOAD_TPL;
        set_status(idx, "LOAD..");
        return false;
    }
    if (idx == PROJ_ROW_TEMPLATES) {
        s_tpl_list            = true;
        s_status_active       = false;
        seq_state.menu_cursor = 0;
        return false;
    }

    uint8_t slot = (uint8_t)(idx - PROJ_ROW_SLOT0);
    const project_slot_info_t *info = &s_info[slot];
    bool corrupt = slot_corrupt(info);

    if (!seq_state.menu_editing) {
        /* First click: enter the action sub-menu fresh. */
        s_action   = PA_LOAD;
        s_armed    = false;
        s_renaming = false;
        return true;
    }

    if (s_renaming) {
        char c = s_name_buf[s_name_pos];
        if (c == '#') {
            s_name_buf[s_name_pos] = '\0';
            commit_rename(idx, slot);
            return false;
        }
        s_name_pos++;
        if (s_name_pos >= PROJECT_NAME_LEN - 1) {
            s_name_buf[PROJECT_NAME_LEN - 1] = '\0';
            commit_rename(idx, slot);
            return false;
        }
        return true;
    }

    switch (s_action) {
        case PA_LOAD:
            if (!info->used || corrupt) {
                set_status(idx, "NO PROJ");
                s_armed = false;
                return false;
            }
            s_req_slot = slot;
            s_req_idx  = idx;
            s_req      = PREQ_LOAD;
            set_status(idx, "LOAD..");
            s_armed = false;
            return false;

        case PA_SAVE:
            if (info->used && !s_armed) {
                s_armed = true;
                return true;   /* second click required to overwrite */
            }
            s_req_slot     = slot;
            s_req_idx      = idx;
            s_req_has_name = info->used;
            if (info->used) {
                snprintf(s_req_name, sizeof(s_req_name), "%s", info->name);
            }
            s_req = PREQ_SAVE;
            set_status(idx, "SAVE..");
            s_armed = false;
            return false;

        case PA_REN:
            if (!info->used || corrupt) {
                set_status(idx, "NO PROJ");
                s_armed = false;
                return false;
            }
            enter_rename(slot);
            return true;

        case PA_DEL:
            if (!info->used) {
                set_status(idx, "NO PROJ");
                s_armed = false;
                return false;
            }
            set_status(idx, project_store_delete(slot) ? "DELETED" : "DEL FAIL");
            s_armed = false;
            s_dirty = true;
            return false;

        case PA_EXIT:
        default:
            s_armed = false;
            return false;
    }
}

/* Runs once per synth_ui_task frame: executes the queued load/save on the
 * layers-applier task, then rewrites the acted-on row's inline status. */
void projects_menu_service(void)
{
    if (s_req == PREQ_NONE) return;

    if (s_req == PREQ_LOAD) {
        bool ok = project_snapshot_load(s_req_slot);
        set_status(s_req_idx, ok ? "LOADED" : "LOAD FAIL");
    } else if (s_req == PREQ_LOAD_TPL) {
        bool ok = project_templates_load(s_req_slot);
        set_status(s_req_idx, ok ? "LOADED" : "LOAD FAIL");
    } else {
        bool ok = project_snapshot_save(s_req_slot,
                                        s_req_has_name ? s_req_name : NULL);
        set_status(s_req_idx, ok ? "SAVED" : "SAVE FAIL");
        s_dirty = true;
    }
    s_req = PREQ_NONE;
}

#endif /* CONFIG_SYNTH_PROJECT_STORE */
