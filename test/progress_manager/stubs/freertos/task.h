// Host-test task stub: creation succeeds (so begin()'s failure cleanup is
// not exercised) but the worker body never runs — suites exercise the real
// flush logic inline through flushNow()/closeBook() (worker notifications
// are no-ops).
#pragma once

#include "FreeRTOS.h"

using TaskHandle_t = void*;
using TaskFunction_t = void (*)(void*);

inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char*, const uint32_t, void*, const UBaseType_t,
                                          TaskHandle_t* handle, const BaseType_t) {
  *handle = reinterpret_cast<TaskHandle_t>(1);  // non-null dummy; body never invoked
  return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t) {}
inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }
inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }
inline void vTaskDelete(TaskHandle_t) {}
