#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Gamma9001 drum bank cache ────────────────────────────────────────────
 * The 3.6 MB drum sample blob stays in its flash partition; only the presets
 * the drum layers reference right now live in PSRAM, each loaded as an AMY
 * memory preset under its own gamma number (about 27 KB on average), which
 * shadows the blob entry in AMY's lookup. The set is recomputed from
 * sequencer state on every sync, not tracked by reference counts, so it
 * cannot drift: missing presets are read from the partition (UI task, a few
 * ms), unreferenced ones are released.
 *
 * Release is deferred: the render task counts a few blocks past the sync
 * (the osc reset that accompanies every preset change is a queued delta and
 * executes at the next block start), then the UI task unloads the preset
 * under the lock. The samples stay valid until that unload, and nothing is
 * freed on the render task.
 *
 * Execution context: init and sync on the UI task (sync is also reached
 * from the button task through the preset setters, which serialise on the
 * amy_helpers event mutex like every other setter path); render_service on
 * the render task with amy_queue_lock released. */

/* Bind the mounted partition (NULL: banks unavailable). Boot, before any
 * layer is configured. */
void     drum_cache_init(const esp_partition_t *part);
bool     drum_cache_available(void);

/* Bring the window set in line with what the drum layers reference. Loads
 * are synchronous; releases complete over the next blocks. */
void     drum_cache_sync(void);

/* Periodic UI-task service: frees windows whose release has drained. */
void     drum_cache_service(void);

/* Render task (Core 1), lock released, once per block: advances releases. */
void     drum_cache_render_service(void);

uint32_t drum_cache_bytes(void);   /* PSRAM held by loaded windows */

#ifdef __cplusplus
}
#endif
