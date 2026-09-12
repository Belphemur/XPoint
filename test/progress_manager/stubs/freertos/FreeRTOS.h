// Host-test stub of the FreeRTOS surface ProgressManager uses.
// Single-threaded: semaphores are counters-only no-ops and the worker task is
// never created (xTaskCreatePinnedToCore fails), so suites exercise the real
// snapshot/commit logic without a second task.
#pragma once

#include <cstdint>

using BaseType_t = long;
using UBaseType_t = unsigned int;
using TickType_t = uint32_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))

// Wall clock, test-controllable (defined in the suite's Stubs.cpp).
unsigned long millis();
