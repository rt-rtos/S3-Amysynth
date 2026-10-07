#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Project slot storage layer ──────────────────────────────────────────
 * Atomic save/load of synth project state via 32-byte header + TLV payload.
 * Files: <PROJECT_FS_BASE>/Pnn.amp (n=0-based slot index).
 * Header (32 bytes): magic, version, reserved, name, payload_len, payload_crc32.
 * Payload: arbitrary TLV stream; this layer does not parse it. */

#define PROJECT_NAME_LEN   16          /* incl. NUL */
#define PROJECT_MAGIC      0x50594D41u /* "AMYP" little-endian */
#define PROJECT_FMT_VERSION 1

/* Characters a project name is made of (the Projects screen's name editor
 * and project_store_name_from() both draw from this set). */
#define PROJECT_NAME_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -"

typedef struct {
    bool     used;
    char     name[PROJECT_NAME_LEN];
    uint32_t size_bytes;
    uint16_t fmt_version;
} project_slot_info_t;

/* Query slot metadata (absent = used=false, present = used=true + name/size/version).
 * Invalid header: used=true, name="<corrupt>". Returns true unless I/O fails.
 * Context: any task; reads concurrent with another task's writes are
 * serialized by esp_littlefs's own filesystem lock. */
bool project_store_slot_info(uint8_t slot, project_slot_info_t *info);

/* Atomic: writes <base>/Pnn.tmp, fsync, rename to <base>/Pnn.amp. */
bool project_store_write(uint8_t slot, const char *name,
                         const uint8_t *payload, size_t payload_len);

/* Reads + validates header (magic, version <= current, CRC32 over payload).
 * Returns malloc'd (SPIRAM) payload via *out (caller frees) + length. */
bool project_store_read(uint8_t slot, uint8_t **out, size_t *out_len,
                        char name_out[PROJECT_NAME_LEN]);

/* As project_store_read(), but *out holds the whole slot file, the 32-byte
 * header followed by the payload, as stored and as
 * project_store_check_image() accepts it; *out_len is its length. Same
 * checks, same malloc'd (SPIRAM) buffer the caller frees; false with no
 * buffer on any failure. Context: any task; reads concurrent with another
 * task's writes are serialized by esp_littlefs's own filesystem lock. */
bool project_store_read_file(uint8_t slot, uint8_t **out, size_t *out_len,
                             char name_out[PROJECT_NAME_LEN]);

/* Validate an in-memory project image (32-byte header + payload, the layout
 * of a slot file) with the same header checks as project_store_read(), plus
 * len == 32 + payload_len and CRC32 over the payload.
 * Context: any task; no allocation, no FS access.
 * On success: *payload points into img, *payload_len is its length, name_out
 * (if non-NULL) holds the header name. Returns false on any invalid image
 * (or NULL img/payload/payload_len), leaving the outputs unwritten. */
bool project_store_check_image(const uint8_t *img, size_t len,
                               const uint8_t **payload, size_t *payload_len,
                               char name_out[PROJECT_NAME_LEN]);

/* Unlink final path. Returns true if gone or absent. */
bool project_store_delete(uint8_t slot);

/* Read whole file, rewrite with new name via write(). Frees the buffer. */
bool project_store_rename(uint8_t slot, const char *new_name);

/* Delete stale *.tmp files. Call after mount. */
void project_store_cleanup_tmp(void);

/* Map a free-form string (typically a file name) to a project name. In
 * order: keep the part after the last '/' or '\'; drop a trailing ".amp"
 * (any case); ASCII lowercase -> uppercase; any character not in
 * PROJECT_NAME_CHARS -> space; collapse runs of spaces; trim; keep at most
 * PROJECT_NAME_LEN - 1 characters; trim trailing spaces again; empty ->
 * "IMPORT". in may be NULL (-> "IMPORT"); out is always NUL-terminated.
 * Context: any; pure function, no FS access, present with or without
 * CONFIG_SYNTH_PROJECT_STORE. */
void project_store_name_from(const char *in, char out[PROJECT_NAME_LEN]);

#if CONFIG_SYNTH_PROJECT_SELFTEST
/* TLV round-trip + slot round-trip selftest. */
void project_store_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
