// RTOS profiling dump. IDF FreeRTOS (CONFIG_FREERTOS_SMP unset) does not
// declare the SMP-kernel helpers, so one uxTaskGetSystemState() snapshot
// supplies the task table and, via xCoreID and the IDLE task counters, the
// per-core busy %.
#include "rtos_stats.h"

#if CONFIG_AMYSYNTH_RTOS_STATS

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "rtos_stats";

void rtos_stats_report(void)
{
    UBaseType_t num_tasks = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = malloc(num_tasks * sizeof(TaskStatus_t));
    if (tasks == NULL) {
        ESP_LOGW(TAG, "rtos stats: malloc(%u tasks) failed", (unsigned)num_tasks);
        return;
    }

    uint32_t total_runtime = 0;
    // On-demand profiling dump (AMYSYNTH_RTOS_STATS, default n). The IRQ-mask
    // window is acceptable ONLY because this never runs in release builds and
    // profiling sessions accept stream artifacts; never wire this into a
    // default-on periodic path: the TCB walk masks IRQs for milliseconds and
    // beats against the USB frame clock.
    // ast-grep-ignore
    num_tasks = uxTaskGetSystemState(tasks, num_tasks, &total_runtime);
    if (num_tasks == 0 || total_runtime == 0) {
        ESP_LOGW(TAG, "rtos stats: snapshot unavailable");
        free(tasks);
        return;
    }

    // Per-task table; also collect per-core IDLE counters for the busy % below.
    uint64_t idle_now[portNUM_PROCESSORS] = {0};
    ESP_LOGI(TAG, "RTOS tasks: name             core prio stack_hwm cpu%%(cumulative)");
    for (UBaseType_t i = 0; i < num_tasks; i++) {
        const TaskStatus_t *t = &tasks[i];
        uint32_t cpu_pct = (uint32_t)(((uint64_t)t->ulRunTimeCounter * 100ULL) / total_runtime);
        int core = (int)t->xCoreID; // tskNO_AFFINITY shows as a large value
        ESP_LOGI(TAG, "  %-16s %4d %4u %9u %3u%%",
                 t->pcTaskName,
                 core,
                 (unsigned)t->uxCurrentPriority,
                 (unsigned)t->usStackHighWaterMark,
                 (unsigned)cpu_pct);
        // The IDLE tasks are named "IDLE0"/"IDLE1" on the IDF kernel.
        if (strncmp(t->pcTaskName, "IDLE", 4) == 0) {
            int c = t->pcTaskName[4] - '0';
            if (c >= 0 && c < portNUM_PROCESSORS) {
                idle_now[c] = (uint64_t)t->ulRunTimeCounter;
            }
        }
    }

    // Busy % = IDLE-counter delta vs esp_timer wall time (same us time base
    // with RUN_TIME_STATS_USING_ESP_TIMER=y).
    static uint64_t s_prev_wall_us = 0;
    static uint64_t s_prev_idle_us[portNUM_PROCESSORS] = {0};
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint64_t wall_delta = now_us - s_prev_wall_us;
    for (int core = 0; core < portNUM_PROCESSORS; core++) {
        uint64_t idle_delta = idle_now[core] - s_prev_idle_us[core];
        if (s_prev_wall_us != 0 && wall_delta > 0) {
            uint32_t idle_pct = (uint32_t)((idle_delta * 100ULL) / wall_delta);
            if (idle_pct > 100) idle_pct = 100; // clamp sampling skew
            ESP_LOGI(TAG, "core %d load: busy=%u%% idle=%u%% (interval %u ms)",
                     core, (unsigned)(100 - idle_pct), (unsigned)idle_pct,
                     (unsigned)(wall_delta / 1000ULL));
        } else {
            ESP_LOGI(TAG, "core %d load: (baseline captured)", core);
        }
        s_prev_idle_us[core] = idle_now[core];
    }
    s_prev_wall_us = now_us;

    free(tasks);

    // Heap snapshot: internal vs PSRAM free + largest free block.
    ESP_LOGI(TAG,
             "heap: free=%u min_free=%u | internal free=%u largest=%u | psram free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

#endif // CONFIG_AMYSYNTH_RTOS_STATS
