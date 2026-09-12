// Definitions for the progress_manager host-test stubs.

#include "HalPowerManager.h"
#include "HalStorage.h"
#include "freertos/FreeRTOS.h"

// Test-controllable wall clock (declared in the suite via extern).
unsigned long testClockMs = 0;
unsigned long millis() { return testClockMs; }

HalStorage HalStorage::instance;
HalPowerManager powerManager;
