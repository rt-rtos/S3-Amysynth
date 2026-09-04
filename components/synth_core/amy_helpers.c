#include "amy_helpers.h"
#include "fx_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

/*
 * amy_event is large enough to be risky on small task stacks. First-party synth
 * modules share one scratch event and serialize access around the ingress seam.
 * Callers must not hold the returned pointer after send/cancel.
 */
static amy_event s_event;
static SemaphoreHandle_t s_event_mutex = NULL;
static TaskHandle_t s_render_task = NULL;

/*
 * AMY ingest pump.
 *
 * amy_add_event() runs AMY's full ingest in the caller's context, and for a
 * patch-number event that includes the patch-string parse - a multi-KB stack
 * frame with millisecond-scale runtime. Applying it inline meant two things:
 * every emitting task had to be sized for that worst case, and the parse ran
 * while holding the ingress mutex, so a UI gesture could stall the sequencer,
 * arp and live-play senders for the duration of the parse.
 *
 * Instead, producers copy their filled event into a FIFO and one pump task -
 * holding the single copy of the parse-frame stack rent - drains it into
 * amy_add_event(). The mutex hold collapses to a memcpy.
 *
 * The pump also serves one URGENT SOURCE: a registered drain callback for
 * deadline-sensitive jobs produced by a context that may not block on the
 * FIFO (the render task's decorated-step trigs, seq_trig_pump.c). The loop
 * drains the urgent source before every FIFO item, and sends made ON the
 * pump task (i.e. from that callback) apply inline instead of re-entering
 * the FIFO - so a decorated note never waits out a parse backlog, and the
 * pump cannot deadlock against its own queue. One doorbell (a task
 * notification) covers both sources; producers ring it after enqueueing.
 *
 * Ordering: the copy into the FIFO happens BEFORE the mutex is released, so
 * FIFO order equals the global emission order the previous inline scheme
 * produced. Voice-kill -> patch-load -> note-on chains cannot reorder across
 * producers, and events carry their own time/sequence fields, so AMY-side
 * scheduling is untouched. Urgent jobs jumping the FIFO does not weaken
 * this: they originate on the render task, which never participates in the
 * producer mutex, so no order between them and FIFO events ever existed.
 *
 * Backpressure: a full queue blocks the producer rather than dropping - losing
 * a patch-define or a note-off is worse than a short stall, and the wait is
 * strictly narrower than the old "block for someone else's whole parse".
 *
 * The pump loop must never acquire a lock beyond what amy_add_event() takes
 * internally - in particular never s_event_mutex, which a producer can hold
 * while blocked on a full FIFO only this task drains - and must never block
 * on anything but its own doorbell: it inherits the failure potential the
 * inline scheme had, where one stalled parse froze render, the tick-slaved
 * sequencer and all controls at once.
 */
#define AMY_INGEST_QUEUE_DEPTH          64
/* Shallower internal-RAM queue used only when the PSRAM storage alloc fails;
 * degraded burst headroom is acceptable, refusing to run is not. */
#define AMY_INGEST_QUEUE_DEPTH_FALLBACK 16
#define AMY_INGEST_TASK_STACK           8192
/* Same tier as the UI producers AND the TinyUSB device task: a long patch
 * parse time-slices with USB exactly as the old inline scheme did, instead of
 * strictly preempting it. Note the pump does NOT outrank every producer - the
 * NimBLE host and the render task sit far above it - so drain latency is
 * bounded by scheduling, not priority alone; the 64-deep FIFO absorbs bursts. */
#define AMY_INGEST_TASK_PRIO            5
#define AMY_INGEST_TASK_CORE            0  /* Core 1 is budgeted for the AMY DSP */

static const char *TAG_INGEST = "amy_ingest";

static QueueHandle_t s_ingest_queue = NULL;
static StaticQueue_t s_ingest_queue_struct;
static TaskHandle_t s_pump_task = NULL;

/* Deadline-sensitive side channel: drains one job and returns true, or false
 * when empty. Registered once, single-threaded, at init time. */
static bool (*s_urgent_drain)(void) = NULL;

/* Pump-context sends use a private scratch and never touch s_event_mutex; see
 * amy_helpers_event_begin(). */
static amy_event s_pump_event;
#if !defined(NDEBUG)
static bool s_pump_event_busy = false;
#endif

/* Bounded wait so a leaked begin/send pair asserts loudly instead of hanging
 * every AMY sender. Well above the worst-case legitimate hold (a patch-string
 * load building four FX reassert events): waiting this long means leaked, not
 * contended. */
#define AMY_HELPERS_EVENT_TIMEOUT_MS 250

static void amy_ingest_task(void *arg)
{
    (void)arg;
    amy_event ev;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            /* Urgent jobs first - and again between every FIFO item, so a
             * decorated-step note due next sequencer tick is never stuck
             * behind a backlog of parses. */
            if (s_urgent_drain != NULL && s_urgent_drain()) continue;
            if (xQueueReceive(s_ingest_queue, &ev, 0) == pdTRUE) {
                amy_add_event(&ev);
                continue;
            }
            break;
        }
    }
}

void amy_helpers_init(void)
{
    if (s_event_mutex == NULL) {
        s_event_mutex = xSemaphoreCreateMutex();
        configASSERT(s_event_mutex != NULL);
    }
    if (s_ingest_queue == NULL) {
        /* Queue storage is bulk cold data touched only by memcpy, so it lives
         * in PSRAM to keep tens of KB out of scarce internal RAM. */
        const size_t item = sizeof(amy_event);
        uint8_t *storage = heap_caps_malloc(AMY_INGEST_QUEUE_DEPTH * item,
                                            MALLOC_CAP_SPIRAM);
        if (storage != NULL) {
            s_ingest_queue = xQueueCreateStatic(AMY_INGEST_QUEUE_DEPTH, item,
                                                storage,
                                                &s_ingest_queue_struct);
        } else {
            ESP_LOGW(TAG_INGEST, "PSRAM queue alloc failed; using internal RAM");
            s_ingest_queue = xQueueCreate(AMY_INGEST_QUEUE_DEPTH_FALLBACK, item);
        }
        /* Lazy init runs single-threaded during startup, before any producer
         * task exists; a missing queue here means the seam would be used from
         * a task with nowhere to put events. */
        configASSERT(s_ingest_queue != NULL);

        BaseType_t ok = xTaskCreatePinnedToCore(amy_ingest_task, "amy_ingest",
                                                AMY_INGEST_TASK_STACK, NULL,
                                                AMY_INGEST_TASK_PRIO,
                                                &s_pump_task,
                                                AMY_INGEST_TASK_CORE);
        configASSERT(ok == pdPASS);
    }
}

void amy_helpers_pump_register_urgent_source(bool (*drain_one)(void))
{
    configASSERT(s_urgent_drain == NULL || s_urgent_drain == drain_one);
    s_urgent_drain = drain_one;
}

void amy_helpers_pump_wake(void)
{
    /* Non-blocking; safe from any task including render. NULL only in the
     * asserts-off degrade path where sends apply inline anyway. */
    if (s_pump_task != NULL) xTaskNotifyGive(s_pump_task);
}

/* Lock hierarchy: s_event_mutex is the OUTERMOST first-party lock. It is held
 * across the FIFO enqueue only; the AMY-side locks are taken later, on the pump
 * task, so the acquisition order is
 *     s_event_mutex -> ingest FIFO, then amy_queue_lock on the pump.
 * The render body (amy_render on Core 1) holds amy_queue_lock for its whole
 * duration, so it must NEVER call an ingress helper: it would both invert that
 * order and risk blocking on a FIFO the pump cannot drain. Register its handle
 * here so a debug build can catch that inversion. Optional: the guard is
 * skipped until a handle is set. */
void amy_helpers_set_render_task(TaskHandle_t render_task)
{
    s_render_task = render_task;
}

amy_event *amy_helpers_event_begin(void)
{
    amy_helpers_init();
#if !defined(NDEBUG)
    /* Render must not re-enter the ingress seam while holding amy_queue_lock. */
    configASSERT(s_render_task == NULL ||
                 xTaskGetCurrentTaskHandle() != s_render_task);
#endif
    if (s_pump_task != NULL && xTaskGetCurrentTaskHandle() == s_pump_task) {
        /* Pump-context send (urgent-source expansion): private scratch, no
         * mutex. A producer can legitimately hold s_event_mutex while blocked
         * on a full FIFO that only this task drains - taking it here would
         * close that cycle into a deadlock. */
#if !defined(NDEBUG)
        configASSERT(!s_pump_event_busy &&
                     "amy_helpers: nested pump-context begin");
        s_pump_event_busy = true;
#endif
        s_pump_event = amy_default_event();
        return &s_pump_event;
    }
    if (xSemaphoreTake(s_event_mutex,
                       pdMS_TO_TICKS(AMY_HELPERS_EVENT_TIMEOUT_MS)) != pdTRUE) {
        /* A prior begin/send pair leaked the mutex: fail loud, never hang. */
        configASSERT(0 && "amy_helpers: s_event_mutex timeout, leaked begin/send");
    }
    s_event = amy_default_event();
    return &s_event;
}

/* INVARIANT: a send is not an apply. This hands the event to the pump; the
 * engine applies it later. Reading AMY state (synth[]/msynth[], patch or voice
 * info) right after a send to observe that send's effect is a bug class - the
 * read will race the pump. Express dependencies as send ORDER, which the FIFO
 * preserves exactly, or poll state that self-heals across UI frames. There is
 * deliberately no flush/drain barrier; add one only when a caller genuinely
 * needs synchronous apply. */
void amy_helpers_event_send(amy_event *event)
{
    /* Slot -> bus routing policy (fx_bus.h): a synth (re)creating event lands
     * on its slot's bus unless it names one. Bus 0 stays unset - it is the
     * engine default, and a spelled-out 0 would fan one BUS delta per osc for
     * nothing. */
    if (AMY_IS_UNSET(event->bus) && AMY_IS_SET(event->synth) &&
        (AMY_IS_SET(event->num_voices) || AMY_IS_SET(event->patch_number) ||
         AMY_IS_SET(event->oscs_per_voice))) {
        uint8_t b = fx_bus_for_synth((uint8_t)event->synth);
        if (b != FX_BUS_HOME) event->bus = b;
    }
    if (event == &s_pump_event) {
        /* On the pump task a send IS an apply: routing through the FIFO from
         * its own consumer would deadlock when full, and the urgent path
         * exists precisely to skip the backlog. */
        amy_add_event(event);
#if !defined(NDEBUG)
        s_pump_event_busy = false;
#endif
        return;
    }
    configASSERT(event == &s_event);
    if (s_ingest_queue == NULL) {
        /* Init failed in a build with assertions compiled out: degrade to the
         * inline apply rather than dropping the event or crashing. */
        amy_add_event(event);
        xSemaphoreGive(s_event_mutex);
        return;
    }
    /* Enqueue BEFORE releasing the mutex: that is what makes FIFO order equal
     * global emission order across producers. */
    if (xQueueSend(s_ingest_queue, event,
                   pdMS_TO_TICKS(AMY_HELPERS_EVENT_TIMEOUT_MS)) != pdTRUE) {
        /* 64 events of sustained backlog means ingest has stalled system-wide
         * (wedged or starved pump). Fail loud rather than hang the senders
         * silently; events are never dropped while assertions are enabled. */
        configASSERT(0 && "amy_helpers: ingest queue full, pump stalled");
    }
    amy_helpers_pump_wake();
    xSemaphoreGive(s_event_mutex);
}

void amy_helpers_event_cancel(amy_event *event)
{
    if (event == &s_pump_event) {
#if !defined(NDEBUG)
        s_pump_event_busy = false;
#endif
        return;
    }
    configASSERT(event == &s_event);
    xSemaphoreGive(s_event_mutex);
}

/* ── Typed ingress entry points ─────────────────────────────────────────
 * amy_add_event() routes by shape: an event with ticks[] set is serialized
 * to a wire string and tick-scheduled whole (sequencer_add_wire, under
 * amy_queue_lock); one without is applied now (add_delta_to_queue /
 * amy_queue_lock). These entry points make the route visible at the call
 * site; the note-send signature is also where per-step parameter-lock
 * fields will attach. */

void amy_helpers_note_send(uint8_t synth, float midi_note, float velocity,
                           uint32_t tag, uint32_t tick, uint32_t period)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth                     = synth;
    e->midi_note                 = midi_note;
    e->velocity                  = velocity;
    e->ticks[TICKS_TAG]    = tag;
    e->ticks[TICKS_TICK]   = tick;
    e->ticks[TICKS_PERIOD] = period;
    amy_helpers_event_send(e);
}

void amy_helpers_config_send(amy_event *event)
{
    /* A sequence[] tuple would silently reroute this to the tick scheduler
     * instead of applying now. "Unset" is the AMY_UNSET sentinel, NOT zero;
     * mirrors amy_add_event's own routing test. */
    configASSERT(AMY_IS_UNSET(event->ticks[TICKS_TAG]) &&
                 AMY_IS_UNSET(event->ticks[TICKS_TICK]) &&
                 AMY_IS_UNSET(event->ticks[TICKS_PERIOD]));
    amy_helpers_event_send(event);
}

void amy_send_patch(uint8_t synth, uint16_t patch_number, uint16_t num_voices,
                    uint32_t synth_flags)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth        = synth;
    e->patch_number = patch_number;
    e->num_voices   = num_voices;
    e->synth_flags  = synth_flags;
    amy_helpers_config_send(e);
}

void amy_send_all_notes_off(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->reset_osc = RESET_ALL_NOTES;
    amy_helpers_config_send(e);
}
