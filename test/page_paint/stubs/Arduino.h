// Host stub for the Arduino core surface PagePaint.cpp uses (slice-budget
// timing). g_fakeMillis is test-controllable so the sliced-paint tests can
// drive the budget checks deterministically.
#pragma once

#include <cstdint>

inline uint32_t g_fakeMillis = 0;
inline uint32_t millis() { return g_fakeMillis; }
