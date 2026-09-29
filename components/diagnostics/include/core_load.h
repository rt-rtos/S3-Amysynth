#pragma once
// Per-core busy-percentage sampler from idle-hook invocation counting.
// Each core's IDLE task calls a registered hook every idle-loop iteration;
// counting invocations per wall-clock window yields an idle rate, and busy
// is that rate measured against the highest rate ever observed on the core
// (a self-calibrating "fully idle" reference). The hook body is one counter
// increment - no locks, no kernel API, no critical sections. ISR time is
// captured naturally: interrupts steal cycles from the idle loop, lowering
// the observed rate.
//
// Rule: a sampler must not take kernel critical sections on a periodic grid
// (uxTaskGetSystemState() and similar TCB walks mask interrupts for
// milliseconds).
//
// Obligations: call core_load_sample() from a single task only (static
// delta state, no locking); never from an ISR. Keep the call interval
// >= ~100 ms so idle-rate noise averages out.
//
// Guarantees: returns true with busy_pct[core] = 0..100, the average busy
// share of each core since the previous successful call - band-indicator
// grade (roughly +-10-20%), biased LOW until the core has had at least one
// mostly-idle window since boot to calibrate against. Returns false with
// busy_pct untouched on the first call (baseline capture + hook
// registration) or if hook registration fails; the next successful call
// averages over the whole gap. Wrap-safe across one counter wrap.
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

#define CORE_LOAD_NUM_CORES CONFIG_FREERTOS_NUMBER_OF_CORES

bool core_load_sample(uint8_t busy_pct[CORE_LOAD_NUM_CORES]);

// Read the most recent successful core_load_sample() result without becoming
// a sampler.
// Obligations: none beyond linking - callable from any task, lock-free, never
// from an ISR only by convention (it is technically safe there too).
// Guarantees: returns true with the last published busy_pct values (each
// 0..100), which are as stale as the single sampler's call interval (status
// LED: 1 s); returns false with busy_pct untouched until the sampler has
// produced its first result (or when no sampler runs at all, e.g.
// CONFIG_AMYSYNTH_STATUS_LED off).
bool core_load_last(uint8_t busy_pct[CORE_LOAD_NUM_CORES]);
