// Host-test semaphore stub: non-null dummies; take/give are no-ops.
// Extends the progress_manager stub with the static-allocation API Epub.cpp
// uses for its GIF-generation mutex.
#pragma once

#include "FreeRTOS.h"

using SemaphoreHandle_t = void*;
struct StaticSemaphore_t {};

inline SemaphoreHandle_t xSemaphoreCreateMutex() { return reinterpret_cast<SemaphoreHandle_t>(1); }
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*) {
  return reinterpret_cast<SemaphoreHandle_t>(1);
}
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return reinterpret_cast<SemaphoreHandle_t>(1); }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
