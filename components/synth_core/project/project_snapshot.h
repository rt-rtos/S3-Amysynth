#pragma once

/* Whole-session save/load: walks every persistable subsystem (GLOB global
 * mix and FX, LAYR sequencer layers, ARP arp, DROP ball drop, DRON and DSTD
 * drones, PROG chord progression, CHRD chord presets, CLIP bounce clips,
 * MIXR mixer, PGEN progression generator, WTCU custom wavetable, FMCU custom
 * FM voice) through the TLV container (project_tlv.h) and the atomic slot
 * store (project_store.h). synth_ui task ONLY - it is the layers applier
 * (sequencer_core_set_layers_applier(), sequencer_core.h) and drains the
 * deferred UI mirror. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Serialize the full session and write it to `slot`. name may be NULL
 * (keeps "P<nn>"). Returns false on any failure (nothing partially saved). */
bool project_snapshot_save(uint8_t slot, const char *name);

/* Load + apply slot. Validates fully before touching live state.
 * Stops the transport. Returns false and leaves the session untouched on
 * any validation failure. */
bool project_snapshot_load(uint8_t slot);

/* Parse + apply an already validated TLV payload (the part of a project image
 * after its header, see project_store_check_image()); `name` is only logged.
 * Same context and guarantees as project_snapshot_load(): the session is left
 * untouched on any failure. Neither frees nor keeps `payload`. */
bool project_snapshot_load_buffer(const uint8_t *payload, size_t len, const char *name);

/* Project file sections, for the partial-load report below. */
typedef enum {
    PROJECT_SECT_GLOB = 0, PROJECT_SECT_LAYR, PROJECT_SECT_ARP, PROJECT_SECT_DROP,
    PROJECT_SECT_DRON, PROJECT_SECT_DSTD, PROJECT_SECT_PROG, PROJECT_SECT_CHRD,
    PROJECT_SECT_CLIP, PROJECT_SECT_MIXR, PROJECT_SECT_PGEN, PROJECT_SECT_WTCU,
    PROJECT_SECT_FMCU,
    PROJECT_SECT_COUNT
} project_sect_t;

/* Sections the last project_snapshot_load() / project_snapshot_load_buffer()
 * skipped because the file holds them at a version this build does not read
 * (bit 1u << project_sect_t); they loaded as if absent. Valid after that load
 * returned true; every load resets it. GLOB and LAYR are never skipped (the
 * load refuses the file instead). Same context as the load. */
uint16_t project_snapshot_skipped(void);

/* Versions of skipped section s from the last load: the file's and this
 * build's. Returns false (outputs untouched) when s was not skipped. Same
 * context as the load. */
bool project_snapshot_skipped_version(project_sect_t s, uint8_t *file_ver,
                                      uint8_t *build_ver);

/* The section's tag as a string ("GLOB", "ARP", ...); "?" when out of
 * range. Any task; the string is static. */
const char *project_snapshot_sect_name(project_sect_t s);

#if CONFIG_SYNTH_PROJECT_SELFTEST
/* Full serialize -> parse -> apply -> delete round-trip against live
 * boot-default state. Logs SNAPSHOT SELFTEST PASS/FAIL. */
void project_snapshot_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
