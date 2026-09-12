// Host-test semaphore stub: non-null dummies; take/give are no-ops.
#pragma once

#include "FreeRTOS.h"

using SemaphoreHandle_t = void*;

inline SemaphoreHandle_t xSemaphoreCreateMutex() { return reinterpret_cast<SemaphoreHandle_t>(1); }
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return reinterpret_cast<SemaphoreHandle_t>(1); }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
