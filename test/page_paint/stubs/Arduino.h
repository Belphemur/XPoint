// Host stub for the Arduino core surface PagePaint.cpp uses (slice-budget
// timing). Constant 0 keeps the budget check inert: paintTextSliced then
// completes in one call, which is exactly what cursor tests want.
#pragma once

#include <cstdint>

inline uint32_t millis() { return 0; }
