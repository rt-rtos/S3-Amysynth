#pragma once
/* Where does a given allocation actually live?
 *
 * On the S3 the address is the region, so classifying a pointer is a range
 * compare - which makes this the only reliable answer to questions an allocator
 * attribute cannot settle. A MALLOC_CAP_SPIRAM request silently falls back to
 * internal RAM under pressure; the gamma9001 drum blob is flash-mapped or
 * staged in PSRAM depending on whether MMU pages were available at boot; and
 * under LTO an IRAM attribute in source proves nothing about where the linker
 * put the code.
 *
 * Placement does not change after init, so the intended use is to track the
 * notable allocations as they are made and print the map once, not to poll.
 *
 * Cost when unused: the tracking table (DIAG_MEM_MAX_ENTRIES * 12 bytes of
 * DRAM) and nothing else. Not Kconfig-gated.
 */
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Short human-readable region name for `p` ("DRAM", "PSRAM", "IRAM",
 * "DROM/flash", ...). Never returns NULL; unrecognised addresses give "?".
 *
 * Caveat for executable addresses: with CONFIG_SPIRAM_FETCH_INSTRUCTIONS or
 * CONFIG_SPIRAM_RODATA, flash content is copied into PSRAM but keeps the same
 * virtual range, so the address cannot distinguish flash-XIP from PSRAM-XIP.
 * Only the Kconfig setting can. */
const char *diag_mem_region(const void *p);

/* Record a notable allocation for the map below. `name` must have static
 * lifetime. Safe to call with a NULL pointer - a failed allocation is exactly
 * what you want the map to show. Returns false if the table is full. */
bool diag_mem_track(const char *name, const void *p, size_t bytes);

/* Log one line per tracked allocation (name, address, region, size) followed by
 * per-cap heap totals. Call once after init completes. */
void diag_mem_report(void);

#ifdef __cplusplus
}
#endif
