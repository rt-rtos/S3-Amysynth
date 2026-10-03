#pragma once

#include "amy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AMY ingress seam. All app-side AMY access goes through this queued API
 * (never direct synth[]/msynth[] access). amy_event is ~800 B, too large for
 * small task stacks, so begin() hands out one shared scratch event held under
 * the ingress mutex until the matching send(); do not keep the pointer after
 * send(), and pair every begin() with a send(). */
void amy_helpers_init(void);
/* Register the AMY render task so debug builds can assert that no ingress
 * helper is ever called from the locked render body. Lock order is
 * s_event_mutex -> ingest FIFO, with amy_queue_lock taken later on the
 * pump task; the render body holds amy_queue_lock and must not re-enter this
 * seam. */
void amy_helpers_set_render_task(TaskHandle_t render_task);
amy_event *amy_helpers_event_begin(void);
/* Hands the event to the ingest pump: a send is NOT an apply. Ordering between
 * sends is preserved, but the engine applies them asynchronously - never read
 * AMY state right after a send to observe its effect (it races the pump);
 * express dependencies as send order. There is deliberately no flush barrier.
 * Task context only.
 * Exception: on the pump task itself (urgent-source callbacks) a send applies
 * inline and returns only once applied.
 * A synth-creating or re-patching event is routed to its slot's bus (fx_bus.h)
 * unless it names one. */
void amy_helpers_event_send(amy_event *event);

/* Register the pump's single urgent source: a callback that drains one
 * deadline-sensitive job and returns true, or false when it has none. The
 * pump invokes it before every FIFO event, on the pump task, so the callback
 * may use begin()/send() freely (those apply inline there) but must never
 * block. Obligations: call once, during single-threaded init, before the
 * producer of those jobs starts. */
void amy_helpers_pump_register_urgent_source(bool (*drain_one)(void));

/* Ring the pump's doorbell after enqueueing work for the urgent source.
 * Non-blocking; callable from any task INCLUDING the render task (it is the
 * one ingress-seam entry point the render task may use). Not ISR-safe. */
void amy_helpers_pump_wake(void);

/* One-liners over begin/send for the two recurring event shapes; everything
 * else sets its own field mix via begin()/send() directly. The names state
 * the ingress route:
 * - NOTE-send: ticks[] set -> serialized to a wire string and tick-scheduled
 *   whole via sequencer_add_wire under amy_queue_lock.
 * - CONFIG-send: no ticks[] -> applied now via add_delta_to_queue under
 *   amy_queue_lock. */

/* Scheduled note-on/off (NOTE route). Carries the ticks[] tuple
 * (tick/period/tag); velocity 0.0f = note-off.
 * This signature is the attach seam for per-step parameter locks. */
void amy_helpers_note_send(uint8_t synth, float midi_note, float velocity,
                           uint32_t tag, uint32_t tick, uint32_t period);

/* amy_helpers_note_send() plus the voice's duty constant (a wavetable voice's
 * frame position, 0..1 over the table). `duty_const` is AMY's float unset
 * sentinel (AMY_UNSET_FLOAT, tested with AMY_IS_SET) for "leave the duty
 * alone", which is exactly amy_helpers_note_send(). A set value rides in the
 * note-on itself: AMY applies a note-on's set fields to the voice the note-on
 * allocates and to no other (patches_voices_for_event, patches.c), so it is a
 * per-voice frame with no extra event. It reaches every osc of that voice,
 * including a PULSE LFO carrier, whose pulse width it then sets (except at
 * 0 and 1, which AMY resets to 0.5). Note-ons only; a note-off sends it
 * unset. Same task rules as amy_helpers_note_send(). */
void amy_helpers_note_send_duty(uint8_t synth, float midi_note, float velocity,
                                uint32_t tag, uint32_t tick, uint32_t period,
                                float duty_const);

/* Send a begin()-obtained event on the CONFIG route, debug-asserting it
 * carries no sequence[] tuple (which would silently reroute it to the tick
 * scheduler). Use for apply-now synth/FX configuration events. */
void amy_helpers_config_send(amy_event *event);

/* Load a patch onto a synth, sizing its voice pool (CONFIG route).
 * synth_flags passes through for layer-specific instrument flags. */
void amy_send_patch(uint8_t synth, uint16_t patch_number, uint16_t num_voices,
                    uint32_t synth_flags);

#ifdef __cplusplus
}
#endif
