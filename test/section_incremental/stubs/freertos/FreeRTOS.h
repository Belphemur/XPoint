// Host-test stub of the FreeRTOS surface Epub.cpp's GIF generation uses.
// Mirrors test/progress_manager/stubs/freertos/FreeRTOS.h (same single-threaded
// no-op semantics) so the host Epub tests compile against the GIF feature.
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
