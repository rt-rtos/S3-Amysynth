#include "sequencer_core/seq_core_internal.h"
#include "amy_helpers.h"
#include "diag_report.h"
#include "freertos/queue.h"

/*
 * Decorated-step trig jobs (the ingest pump's urgent source).
 *
 * sequencer_core_service_tick() always runs on amy_usb_render_task (it's
 * called from amy_execute_deltas() -> sequencer_check_and_fill(), inline in
 * amy_update()). trig_schedule_ratchets() ends in amy_helpers_note_send(),
 * whose amy_helpers_event_begin() asserts if the caller is the render task -
 * and would be wrong to allow even without the assert, since it can block for
 * up to AMY_HELPERS_EVENT_TIMEOUT_MS on a mutex/FIFO the render task must
 * never wait on.
 *
 * So the render task's tick hook only enqueues a tiny job descriptor
 * (non-blocking, never waits) and rings the ingest pump's doorbell. The pump
 * drains this queue as its registered urgent source, doing the actual
 * velocity/pitch/tag-emission work on the pump task, where a send applies
 * inline - ahead of whatever parse backlog the event FIFO holds.
 *
 * Deadline: a ratchet's k==0 sub-hit is scheduled for grid_tick + 1 + swing +
 * nudge, which the service tick's SEQ_TRIG_LOOKAHEAD_TICKS evaluation keeps at
 * least one sequencer tick ahead (single-digit ms at typical tempos) even for
 * the earliest nudge - and AMY silently drops any event whose tick has already
 * passed. The urgent-before-FIFO drain order bounds the wait to at most one
 * in-flight amy_add_event() (worst case a patch-string parse), not the whole
 * backlog.
 */

#define SEQ_TRIG_QUEUE_DEPTH 32

typedef enum {
    SEQ_TRIG_JOB_STEP = 0,       /* decorated-step fire */
    SEQ_TRIG_JOB_FREEZE_RELEASE, /* loop-bounce freeze: release every source voice */
} seq_trig_job_kind_t;

typedef struct {
    uint8_t  kind;
    uint8_t  layer_idx;
    uint8_t  track;
    uint8_t  step;
    uint32_t grid_tick;   /* absolute grid boundary of the step (see service tick) */
} seq_trig_job_t;

static QueueHandle_t s_trig_queue = NULL;
static StaticQueue_t s_trig_queue_struct;
static uint8_t s_trig_queue_storage[SEQ_TRIG_QUEUE_DEPTH * sizeof(seq_trig_job_t)];

/* Diagnostics only - never gate correctness on these. A nonzero drops_full
 * means the queue is genuinely undersized for the pattern being played; a
 * nonzero drops_mutating means the consumer raced a layer add/delete, which
 * the engine already tolerates as an inaudible dropped tick (see the
 * s_layers_mutating check in sequencer_core_service_tick()). */
static uint32_t s_trig_drops_full     = 0;
static uint32_t s_trig_drops_mutating = 0;

static void sequencer_core_trig_pump_report(void);

/* Runs on the ingest pump task. Drains one job; returns false when empty. */
static bool seq_trig_drain_one(void)
{
    seq_trig_job_t job;
    if (xQueueReceive(s_trig_queue, &job, 0) != pdTRUE) return false;
    if (job.kind == SEQ_TRIG_JOB_FREEZE_RELEASE) {
        sequencer_core_freeze_release_apply();
        return true;
    }
    /* Closes the race the deferral itself introduces: a delete_layer()
     * compaction landing between the render task's enqueue and this dequeue
     * would otherwise read a shifted or reused s_layers[] slot. Re-check both
     * the in-flight flag and the (possibly now smaller) layer count before
     * touching s_layers. */
    if (s_layers_mutating || job.layer_idx >= s_num_layers) {
        s_trig_drops_mutating++;
        return true;
    }
    trig_schedule_ratchets(job.layer_idx, &s_layers[job.layer_idx],
                           job.track, job.step, job.grid_tick);
    return true;
}

void sequencer_core_trig_pump_init(void)
{
    if (s_trig_queue != NULL) return;   /* idempotent, like amy_helpers_init() */

    s_trig_queue = xQueueCreateStatic(SEQ_TRIG_QUEUE_DEPTH, sizeof(seq_trig_job_t),
                                      s_trig_queue_storage, &s_trig_queue_struct);
    configASSERT(s_trig_queue != NULL);

    amy_helpers_pump_register_urgent_source(seq_trig_drain_one);

    diag_register_reporter("seq-trig", sequencer_core_trig_pump_report);
}

void sequencer_core_trig_enqueue(uint8_t layer_idx, uint8_t track, uint8_t step,
                                 uint32_t grid_tick)
{
    seq_trig_job_t job = { SEQ_TRIG_JOB_STEP, layer_idx, track, step, grid_tick };
    /* Zero timeout: the render task must never block here. A full queue means
     * the consumer has stalled or this tick produced more decorated fires
     * than the queue was sized for - drop and count, never wait. */
    if (xQueueSend(s_trig_queue, &job, 0) != pdTRUE) {
        s_trig_drops_full++;
        return;
    }
    amy_helpers_pump_wake();
}

void sequencer_core_freeze_release_enqueue(void)
{
    seq_trig_job_t job = { SEQ_TRIG_JOB_FREEZE_RELEASE, 0, 0, 0, 0 };
    if (xQueueSend(s_trig_queue, &job, 0) != pdTRUE) {
        s_trig_drops_full++;
        return;
    }
    amy_helpers_pump_wake();
}

static void sequencer_core_trig_pump_report(void)
{
    if (s_trig_drops_full == 0 && s_trig_drops_mutating == 0) return;
    ESP_LOGI(TAG, "seq_trig: drops_full=%u drops_mutating=%u",
             (unsigned)s_trig_drops_full, (unsigned)s_trig_drops_mutating);
}
