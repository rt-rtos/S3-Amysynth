#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "custompatches/wt_synth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Custom wavetable builder (SEQ_PATCH_WAVETABLE_CUSTOM) ────────────────
 * Owns the one global custom table: its sixteen parameters (wt_params_t, the
 * math in wt_synth.h), the live AMY memory preset WT_BUILDER_PRESET
 * (wavetable_bank.h) every row, the arp and the drones play on patch 288,
 * a 64 KB PSRAM float scratch, and the rebuild that turns a parameter edit
 * into a new table.
 *
 * Rebuild: a setter stores its byte and raises a dirty flag (release). The
 * service snapshots the parameters when it starts a build (exchange of the
 * flag with acq_rel, then a copy, so a setter landing in between re-arms it:
 * worst case one extra rebuild), builds WT_FRAMES_PER_SLICE frames per call,
 * and on the call after the last frame finishes the table in place in the
 * scratch, copies it into the live buffer outside amy_queue_lock, publishes
 * the previews and bumps the generation. A flag raised mid-build lets the
 * build finish and starts the next one. A full table takes five calls.
 *
 * Torn block: the S3 caches are shared by both cores, the preset's sample
 * pointer is fixed after pcm_load() and the copy never touches the preset
 * list, so the render task at worst plays one block across an old/new
 * boundary inside a frame: one step the size of the edit.
 *
 * Never-freed invariant: the live buffer is allocated once at init and never
 * freed or reloaded. The only runtime path that frees memory presets is
 * amy_reset_oscs() -> pcm_unload_all_presets(), reached from a delta
 * carrying RESET_ALL_OSCS|RESET_SYNTHS; no app path sends those (the app
 * sends osc-index resets only). The wavetable bank relies on the same
 * invariant.
 *
 * Execution context: init on app_main after wavetable_bank_init(), before any
 * task can apply a patch. The parameter writers (setters, set_params,
 * copy/reset_keyframe, blend_mid) run on any Core-0 task: encoder_task, button_task,
 * seq_ui on project load. The service, generation(), preview(),
 * frame_preview() and frame_harmonics() run on the seq_ui task only. wt_builder_preset() and get_params() are lock-free from
 * any Core-0 task. Nothing here is for the render task or an ISR. */

#define WT_FRAMES_PER_SLICE 16

/* wt_builder_preset() while the builder is unavailable; the wavetable bank
 * turns it into the first vendored table. */
#define WT_BUILDER_PRESET_NONE 0xFFFFu

typedef enum {
    WT_FIELD_SHAPE = 0,
    WT_FIELD_WIDTH,
    WT_FIELD_BRIGHT,
    WT_FIELD_SYNC,
    WT_FIELD_PEAK,
    WT_FIELD_RANGE,     /* global: the keyframe argument is ignored */
} wt_field_t;

/* Allocate the scratch and the live preset, then build the default table
 * synchronously so the patch is valid from boot. On any allocation failure:
 * frees what was allocated, logs once, returns false and leaves the builder
 * disabled (preset() returns WT_BUILDER_PRESET_NONE, the setters still store
 * parameters, the service does nothing). */
bool wt_builder_init(void);

/* Store one field of keyframe 0 (A), 1 (M) or 2 (B; above 2 clamps to 2),
 * clamped to its domain (wt_synth.h; a peak of 1 becomes Off), and mark the
 * table dirty. */
void wt_builder_set_field(wt_field_t field, uint8_t keyframe, uint8_t value);

/* Whole parameter set (project load); clamped, then marked dirty. */
void wt_builder_set_params(const wt_params_t *p);
void wt_builder_get_params(wt_params_t *out);

/* Copy all five fields of keyframe `from` (0..2) over the next one,
 * (from + 1) % 3: A -> M, M -> B, B -> A. */
void wt_builder_copy_keyframe(uint8_t from);
/* Keyframe k (0..2) back to the default saw; range untouched. */
void wt_builder_reset_keyframe(uint8_t k);
/* Keyframe 1 (M) to the halfway blend of A and B (wt_params_blend_mid()),
 * then mark the table dirty. */
void wt_builder_blend_mid(void);

/* One rebuild step (see above). seq_ui task, every wake. */
void wt_builder_service(void);

/* The AMY preset of the live table, or WT_BUILDER_PRESET_NONE. */
uint16_t wt_builder_preset(void);

/* Counts finished rebuilds (1 after init); a view hashes it so a finished
 * rebuild redraws the preview. */
uint32_t wt_builder_generation(void);

/* Previews of frames 0, WT_MID_FRAME and 63 of the live table. */
void wt_builder_preview(wt_preview_t *out);

/* Preview of any frame 0..63 of the live table (above 63 clamps) into
 * out[WT_PREVIEW_POINTS]; zeros while the builder is unavailable. Reads the
 * table the service writes, hence the seq_ui task only. */
void wt_builder_frame_preview(uint8_t frame, int8_t *out);

/* Harmonic levels 1..count of any frame of the live table, in
 * wt_synth_frame_harmonics() units; all 255 while the builder is
 * unavailable. About 250 multiply-adds per harmonic: call it when the frame
 * or the generation changes, not on every wake. seq_ui task only. */
void wt_builder_frame_harmonics(uint8_t frame, uint8_t count, uint8_t *out);

#ifdef __cplusplus
}
#endif
