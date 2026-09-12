// Host-test task stub: task creation always fails, so the manager runs its
// flush paths inline (worker_ stays null and notifications are no-ops).
#pragma once

#include "FreeRTOS.h"

using TaskHandle_t = void*;
using TaskFunction_t = void (*)(void*);

inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char*, const uint32_t, void*, const UBaseType_t,
                                          TaskHandle_t*, const BaseType_t) {
  return pdFAIL;
}
inline void xTaskNotifyGive(TaskHandle_t) {}
inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }
inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }
inline void vTaskDelete(TaskHandle_t) {}
