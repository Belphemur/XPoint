// millis() lives in the FreeRTOS stub header (see freertos/FreeRTOS.h) with
// its definition in Stubs.cpp; this header exists so any accidental
// <Arduino.h> include in the chain resolves harmlessly.
#pragma once

#include "freertos/FreeRTOS.h"
