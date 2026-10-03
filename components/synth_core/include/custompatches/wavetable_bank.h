#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── App-side wavetable bank ──────────────────────────────────────────────
 * Wavetables generated from Vital exports (custompatches/wavetables/, one
 * header per table, enumerated by manifest.inc) and served to AMY's WAVETABLE
 * oscillator as memory presets: each table is copied from flash rodata into
 * a pcm_load() buffer once at init, under a preset number of its own. AMY
 * resolves memory presets before its built-in ROM map, so the bank extends
 * the five vendored tables without touching the AMY header.
 *
 * Patch numbers: the bank owns SEQ_PATCH_WAVETABLE_APP_BASE ..
 * SEQ_PATCH_WAVETABLE_APP_MAX (sequencer_core.h); slots past the table count
 * are permanent holes the browse domains skip. It also resolves
 * SEQ_PATCH_WAVETABLE_CUSTOM, whose table the builder (wt_builder.h) owns.
 *
 * Execution context: init on app_main after amy_start() (pcm_init() empties
 * the memory-preset list, so nothing loaded earlier survives) and before any
 * task can apply a patch. The lookups are lock-free reads of init-time
 * state, callable from the UI/button tasks; never from the render task. */

/* Memory-preset numbers: above AMY's ROM map (24 entries incl. the vendored
 * wavetables) and below the gamma9001 drum base (256), a band nothing else
 * loads into. The bank takes one per app slot; the custom-wavetable builder
 * (wt_builder.h) takes the next one, 40. WT_BUILDER_PRESET needs
 * sequencer_core.h where it is used. */
#define WT_BANK_PRESET_BASE 32u
#define WT_BUILDER_PRESET   (WT_BANK_PRESET_BASE + SEQ_PATCH_WAVETABLE_APP_SLOTS)

/* Number of tables built into this firmware (0 when AMY_WAVETABLE is off). */
uint8_t wavetable_bank_count(void);

/* Load every table into AMY as a memory preset. A table that fails to
 * allocate is logged and left unloaded; its patch then falls back to the
 * first vendored table so the slot still sounds. */
void wavetable_bank_init(void);

/* AMY preset number to set alongside wave=WAVETABLE for `patch`, for both the
 * vendored range (SEQ_PATCH_WAVETABLE_BASE..MAX), the app range and the
 * custom table. Returns the first vendored table for anything outside those,
 * for an app slot that is empty or failed to load, and for the custom table
 * while the builder is unavailable. */
uint16_t wavetable_bank_preset_for_patch(uint16_t patch);

/* Display name ("Wavetable: <label>") for an app-range patch or the custom
 * table, NULL for any other number or an empty slot. Static storage, valid
 * forever. */
const char *wavetable_bank_patch_name(uint16_t patch);

#ifdef __cplusplus
}
#endif
