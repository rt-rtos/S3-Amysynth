#include "project_store.h"
#include "project_fs.h"
#include "project_tlv.h"

#include "sdkconfig.h"
#include <string.h>
#if CONFIG_SYNTH_PROJECT_STORE

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "project_store";

#define PROJECT_HDR_LEN 32
#define SLOT_MAX_PATH (sizeof(PROJECT_FS_BASE) + 8 + 1)

static void slot_path(uint8_t slot, bool tmp, char out[SLOT_MAX_PATH])
{
    snprintf(out, SLOT_MAX_PATH, PROJECT_FS_BASE "/P%02u.%s",
             (unsigned)slot, tmp ? "tmp" : "amp");
}

static void put_hdr(uint8_t hdr[PROJECT_HDR_LEN], const char *name,
                    uint32_t payload_len, uint32_t crc)
{
    memset(hdr, 0, PROJECT_HDR_LEN);
    tlv_writer_t w;
    tlv_writer_init(&w, hdr, PROJECT_HDR_LEN);
    tlv_put_u32(&w, PROJECT_MAGIC);
    tlv_put_u16(&w, PROJECT_FMT_VERSION);
    tlv_put_u16(&w, 0);                        /* reserved */
    char nm[PROJECT_NAME_LEN] = {0};
    if (name) strncpy(nm, name, PROJECT_NAME_LEN - 1);
    tlv_put_bytes(&w, nm, PROJECT_NAME_LEN);
    tlv_put_u32(&w, payload_len);
    tlv_put_u32(&w, crc);
}

/* Decode put_hdr()'s layout and validate it: magic, format version <= ours,
 * payload length within the write-side bounds. One check for slot files and
 * in-memory images alike. */
static bool hdr_decode(const uint8_t hdr[PROJECT_HDR_LEN], uint32_t *payload_len,
                       uint32_t *crc, char nm[PROJECT_NAME_LEN])
{
    uint32_t magic = 0;
    uint16_t ver = 0, reserved = 0;
    tlv_reader_t r;
    tlv_reader_init(&r, hdr, PROJECT_HDR_LEN);
    bool ok = tlv_get_u32(&r, &magic) && tlv_get_u16(&r, &ver)
           && tlv_get_u16(&r, &reserved)
           && tlv_get_bytes(&r, nm, PROJECT_NAME_LEN)
           && tlv_get_u32(&r, payload_len) && tlv_get_u32(&r, crc);
    return ok && magic == PROJECT_MAGIC && ver <= PROJECT_FMT_VERSION
              && *payload_len > 0 && *payload_len <= (1u << 20);
}

bool project_store_write(uint8_t slot, const char *name,
                         const uint8_t *payload, size_t payload_len)
{
    /* Mirror the read-side payload bounds: writing a size read() would later
     * refuse must fail HERE, not turn the slot silently unreadable. */
    if (!project_fs_ok() || slot >= CONFIG_SYNTH_PROJECT_MAX_SLOTS || !payload
        || payload_len == 0 || payload_len > (1u << 20))
        return false;

    uint8_t hdr[PROJECT_HDR_LEN];
    put_hdr(hdr, name, (uint32_t)payload_len,
            project_crc32(payload, payload_len));

    char tmp_path[SLOT_MAX_PATH], final_path[SLOT_MAX_PATH];
    slot_path(slot, true, tmp_path);
    slot_path(slot, false, final_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) { ESP_LOGE(TAG, "open %s failed", tmp_path); return false; }
    bool ok = fwrite(hdr, 1, PROJECT_HDR_LEN, f) == PROJECT_HDR_LEN
           && fwrite(payload, 1, payload_len, f) == payload_len
           && fflush(f) == 0
           && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { unlink(tmp_path); ESP_LOGE(TAG, "write slot %u failed", slot); return false; }

    /* LittleFS rename won't overwrite an existing target, so the old file is
     * unlinked first. Power loss inside the unlink..rename window leaves the
     * slot empty (new data still in .tmp, reaped at next mount) - a narrow,
     * accepted tradeoff; the old data was about to be replaced anyway. */
    unlink(final_path);
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        ESP_LOGE(TAG, "rename slot %u failed", slot);
        return false;
    }
    return true;
}

/* Read and validate a slot file into a SPIRAM buffer: the payload alone, or
 * (whole) the header followed by the payload, the file as stored. */
static bool slot_load(uint8_t slot, bool whole, uint8_t **out, size_t *out_len,
                      char name_out[PROJECT_NAME_LEN])
{
    if (!project_fs_ok() || slot >= CONFIG_SYNTH_PROJECT_MAX_SLOTS || !out)
        return false;

    char path[SLOT_MAX_PATH];
    slot_path(slot, false, path);
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t hdr[PROJECT_HDR_LEN];
    bool ok = fread(hdr, 1, PROJECT_HDR_LEN, f) == PROJECT_HDR_LEN;

    uint32_t payload_len = 0, crc = 0;
    char nm[PROJECT_NAME_LEN] = {0};
    ok = ok && hdr_decode(hdr, &payload_len, &crc, nm);

    const size_t lead = whole ? PROJECT_HDR_LEN : 0;
    uint8_t *buf = NULL;
    if (ok) {
        buf = heap_caps_malloc(lead + payload_len, MALLOC_CAP_SPIRAM);
        ok = buf && fread(buf + lead, 1, payload_len, f) == payload_len
                 && project_crc32(buf + lead, payload_len) == crc;
    }
    fclose(f);

    if (!ok) {
        free(buf);
        ESP_LOGW(TAG, "slot %u invalid/corrupt - refused", slot);
        return false;
    }
    if (whole) memcpy(buf, hdr, PROJECT_HDR_LEN);
    *out = buf;
    if (out_len) *out_len = lead + payload_len;
    if (name_out) memcpy(name_out, nm, PROJECT_NAME_LEN);
    return true;
}

bool project_store_read(uint8_t slot, uint8_t **out, size_t *out_len,
                        char name_out[PROJECT_NAME_LEN])
{
    return slot_load(slot, false, out, out_len, name_out);
}

bool project_store_read_file(uint8_t slot, uint8_t **out, size_t *out_len,
                             char name_out[PROJECT_NAME_LEN])
{
    return slot_load(slot, true, out, out_len, name_out);
}

bool project_store_check_image(const uint8_t *img, size_t len,
                               const uint8_t **payload, size_t *payload_len,
                               char name_out[PROJECT_NAME_LEN])
{
    if (!img || !payload || !payload_len || len < PROJECT_HDR_LEN)
        return false;

    uint32_t plen = 0, crc = 0;
    char nm[PROJECT_NAME_LEN] = {0};
    if (!hdr_decode(img, &plen, &crc, nm)
        || len != PROJECT_HDR_LEN + (size_t)plen
        || project_crc32(img + PROJECT_HDR_LEN, plen) != crc)
        return false;

    *payload     = img + PROJECT_HDR_LEN;
    *payload_len = plen;
    if (name_out) memcpy(name_out, nm, PROJECT_NAME_LEN);
    return true;
}

bool project_store_slot_info(uint8_t slot, project_slot_info_t *info)
{
    if (!project_fs_ok() || slot >= CONFIG_SYNTH_PROJECT_MAX_SLOTS || !info)
        return false;

    char path[SLOT_MAX_PATH];
    slot_path(slot, false, path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        info->used = false;
        return true;
    }

    uint8_t hdr[PROJECT_HDR_LEN];
    bool ok = fread(hdr, 1, PROJECT_HDR_LEN, f) == PROJECT_HDR_LEN;

    uint32_t magic = 0;
    uint16_t ver = 0, reserved = 0;
    char nm[PROJECT_NAME_LEN] = {0};
    if (ok) {
        tlv_reader_t r;
        tlv_reader_init(&r, hdr, PROJECT_HDR_LEN);
        ok = tlv_get_u32(&r, &magic) && tlv_get_u16(&r, &ver)
          && tlv_get_u16(&r, &reserved)
          && tlv_get_bytes(&r, nm, PROJECT_NAME_LEN);
    }

    long size_bytes = 0;
    if (ok && fseek(f, 0, SEEK_END) == 0) {
        size_bytes = ftell(f);
    }
    fclose(f);

    info->used = true;
    if (ok && magic == PROJECT_MAGIC && ver <= PROJECT_FMT_VERSION) {
        memcpy(info->name, nm, PROJECT_NAME_LEN);
        info->fmt_version = ver;
        info->size_bytes = (uint32_t)size_bytes;
    } else {
        strncpy(info->name, "<corrupt>", PROJECT_NAME_LEN - 1);
        info->name[PROJECT_NAME_LEN - 1] = '\0';
        info->fmt_version = 0;
        info->size_bytes = 0;
    }
    return true;
}

bool project_store_delete(uint8_t slot)
{
    if (!project_fs_ok() || slot >= CONFIG_SYNTH_PROJECT_MAX_SLOTS)
        return false;

    char path[SLOT_MAX_PATH];
    slot_path(slot, false, path);
    return unlink(path) == 0 || access(path, F_OK) != 0;
}

bool project_store_rename(uint8_t slot, const char *new_name)
{
    if (!project_fs_ok() || slot >= CONFIG_SYNTH_PROJECT_MAX_SLOTS || !new_name)
        return false;

    uint8_t *buf = NULL;
    size_t len = 0;
    char old_name[PROJECT_NAME_LEN];
    if (!project_store_read(slot, &buf, &len, old_name)) {
        return false;
    }

    bool ok = project_store_write(slot, new_name, buf, len);
    free(buf);
    return ok;
}

void project_store_cleanup_tmp(void)
{
    if (!project_fs_ok())
        return;

    /* Enumerate the slots rather than scanning the directory: the only temp
     * files this module can create are slot_path(slot, tmp) for slot <
     * MAX_SLOTS, so the name comes from the same function that writes it.
     * Nothing to bound, no path to assemble, and it cannot unlink a .tmp that
     * is not ours. Absent files return ENOENT, which is the normal case. */
    for (uint8_t slot = 0; slot < CONFIG_SYNTH_PROJECT_MAX_SLOTS; slot++) {
        char path[SLOT_MAX_PATH];
        slot_path(slot, true, path);
        unlink(path);
    }
}

#if CONFIG_SYNTH_PROJECT_SELFTEST
void project_store_selftest(void)
{
    /* TLV round-trip */
    uint8_t buf[128];
    tlv_writer_t w; tlv_writer_init(&w, buf, sizeof buf);
    size_t h = tlv_begin_section(&w, 0x54534554u, 3);   /* 'TEST' */
    tlv_put_u8(&w, 200); tlv_put_i16(&w, -1234);
    tlv_put_f32(&w, 0.5f); tlv_put_u32(&w, 0xDEADBEEFu);
    tlv_end_section(&w, h);
    bool pass = !w.err;

    tlv_reader_t r, body; uint32_t tag; uint8_t ver;
    tlv_reader_init(&r, buf, w.len);
    pass = pass && tlv_next_section(&r, &tag, &ver, &body)
        && tag == 0x54534554u && ver == 3;
    uint8_t a; int16_t b; float c; uint32_t d;
    pass = pass && tlv_get_u8(&body, &a) && a == 200
        && tlv_get_i16(&body, &b) && b == -1234
        && tlv_get_f32(&body, &c) && c == 0.5f
        && tlv_get_u32(&body, &d) && d == 0xDEADBEEFu;

    /* Slot round-trip in the top slot - but never over a real project:
     * the test deletes the slot afterwards, so an occupied slot means skip. */
    uint8_t slot = CONFIG_SYNTH_PROJECT_MAX_SLOTS - 1;
    project_slot_info_t info;
    if (project_store_slot_info(slot, &info) && info.used) {
        ESP_LOGW(TAG, "SELFTEST SKIPPED (slot P%02u in use)", (unsigned)slot);
        return;
    }
    pass = pass && project_store_write(slot, "selftest", buf, w.len);
    uint8_t *rb = NULL; size_t rlen = 0; char nm[PROJECT_NAME_LEN];
    pass = pass && project_store_read(slot, &rb, &rlen, nm)
        && rlen == w.len && memcmp(rb, buf, rlen) == 0
        && strcmp(nm, "selftest") == 0;
    free(rb);
    project_store_delete(slot);

    ESP_LOGI("project_store", "SELFTEST %s", pass ? "PASS" : "FAIL");
}
#endif

#else /* !CONFIG_SYNTH_PROJECT_STORE */

bool project_store_slot_info(uint8_t slot, project_slot_info_t *info)
{ (void)slot; (void)info; return false; }

bool project_store_write(uint8_t slot, const char *name,
                         const uint8_t *payload, size_t payload_len)
{ (void)slot; (void)name; (void)payload; (void)payload_len; return false; }

bool project_store_read(uint8_t slot, uint8_t **out, size_t *out_len,
                        char name_out[PROJECT_NAME_LEN])
{ (void)slot; (void)out; (void)out_len; (void)name_out; return false; }

bool project_store_read_file(uint8_t slot, uint8_t **out, size_t *out_len,
                             char name_out[PROJECT_NAME_LEN])
{ (void)slot; (void)out; (void)out_len; (void)name_out; return false; }

bool project_store_check_image(const uint8_t *img, size_t len,
                               const uint8_t **payload, size_t *payload_len,
                               char name_out[PROJECT_NAME_LEN])
{ (void)img; (void)len; (void)payload; (void)payload_len; (void)name_out; return false; }

bool project_store_delete(uint8_t slot)
{ (void)slot; return false; }

bool project_store_rename(uint8_t slot, const char *new_name)
{ (void)slot; (void)new_name; return false; }

void project_store_cleanup_tmp(void)
{ }

#if CONFIG_SYNTH_PROJECT_SELFTEST
void project_store_selftest(void)
{ }
#endif

#endif /* CONFIG_SYNTH_PROJECT_STORE */

void project_store_name_from(const char *in, char out[PROJECT_NAME_LEN])
{
    size_t n = 0;
    if (in) {
        for (const char *p = in; *p; p++) {
            if (*p == '/' || *p == '\\') in = p + 1;
        }
        size_t len = strlen(in);
        if (len >= 4 && in[len - 4] == '.'
            && (in[len - 3] | 0x20) == 'a' && (in[len - 2] | 0x20) == 'm'
            && (in[len - 1] | 0x20) == 'p') {
            len -= 4;
        }
        for (size_t i = 0; i < len && n < PROJECT_NAME_LEN - 1; i++) {
            char c = in[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (strchr(PROJECT_NAME_CHARS, c) == NULL) c = ' ';
            if (c == ' ' && (n == 0 || out[n - 1] == ' ')) continue;
            out[n++] = c;
        }
        while (n > 0 && out[n - 1] == ' ') n--;
    }
    if (n == 0) {
        strcpy(out, "IMPORT");
        return;
    }
    out[n] = '\0';
}
