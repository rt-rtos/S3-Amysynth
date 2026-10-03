#pragma once

/* Whole-session save/load: walks every persistable subsystem (global mix,
 * sequencer layers, arp, drone, chord progression) through the TLV container
 * (project_tlv.h) and the atomic slot store (project_store.h). synth_ui task
 * ONLY - it is the layers applier (sequencer_core_set_layers_applier(),
 * sequencer_core.h) and drains the deferred UI mirror. */

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

#if CONFIG_SYNTH_PROJECT_SELFTEST
/* Full serialize -> parse -> apply -> delete round-trip against live
 * boot-default state. Logs SNAPSHOT SELFTEST PASS/FAIL. */
void project_snapshot_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
