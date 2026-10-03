#include "amy_helpers.h"
#include "fx_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

/* Shared scratch event and ingress mutex: see amy_helpers.h. */
static amy_event s_event;
static SemaphoreHandle_t s_event_mutex = NULL;
static TaskHandle_t s_render_task = NULL;

/*
 * AMY ingest pump.
 *
 * amy_add_event() runs AMY's whole ingest in the caller's context, including
 * the patch-string parse for a patch-number event: a multi-KB stack frame and
 * milliseconds of runtime. Producers therefore only copy their event into a
 * FIFO (the ingress mutex is held for that copy), and this one task drains
 * the FIFO into amy_add_event(), so only its stack is sized for the parse.
 *
 * Ordering: the copy happens before the mutex is released, so FIFO order is
 * the global emission order across producers (voice-kill -> patch-load ->
 * note-on cannot reorder). Events carry their own time fields, so AMY-side
 * scheduling is unchanged.
 *
 * Urgent source: one registered callback (seq_trig_pump.c, jobs from the
 * render task) is drained before every FIFO item, and sends made on this task
 * apply inline, so a decorated step never waits behind a parse backlog. Those
 * jobs never take the producer mutex, so skipping the FIFO breaks no order.
 * Producers ring one task-notification doorbell for both sources.
 *
 * Backpressure: a full FIFO blocks the producer; nothing is dropped.
 *
 * The pump must never take s_event_mutex (a producer can hold it while
 * blocked on the full FIFO this task drains) and must block only on its
 * doorbell: a stalled pump stalls render, the sequencer and all controls.
 */
#define AMY_INGEST_QUEUE_DEPTH          64
/* Shallower internal-RAM queue used only when the PSRAM storage alloc fails;
 * degraded burst headroom is acceptable, refusing to run is not. */
#define AMY_INGEST_QUEUE_DEPTH_FALLBACK 16
#define AMY_INGEST_TASK_STACK           8192
/* Same tier as the UI producers AND the TinyUSB device task: a long patch
 * parse time-slices with USB instead of strictly preempting it. Note the pump does NOT outrank every producer - the
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

/* Lock order and the render-task rule: amy_helpers.h. The guard is skipped
 * until a handle is set. */
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
        /* Pump-context send: private scratch, no mutex (see the pump block). */
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

/* A send is not an apply (amy_helpers.h). */
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

/* ── Typed ingress entry points ─────────────────────────────────────────
 * NOTE vs CONFIG route: see amy_helpers.h. */

void amy_helpers_note_send_duty(uint8_t synth, float midi_note, float velocity,
                                uint32_t tag, uint32_t tick, uint32_t period,
                                float duty_const)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth                     = synth;
    e->midi_note                 = midi_note;
    e->velocity                  = velocity;
    e->ticks[TICKS_TAG]    = tag;
    e->ticks[TICKS_TICK]   = tick;
    e->ticks[TICKS_PERIOD] = period;
    if (AMY_IS_SET(duty_const)) e->duty_coefs[COEF_CONST] = duty_const;
    amy_helpers_event_send(e);
}

void amy_helpers_note_send(uint8_t synth, float midi_note, float velocity,
                           uint32_t tag, uint32_t tick, uint32_t period)
{
    amy_helpers_note_send_duty(synth, midi_note, velocity, tag, tick, period,
                               AMY_UNSET_FLOAT);
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
