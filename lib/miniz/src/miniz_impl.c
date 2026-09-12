/* Compiles the vendored miniz with CrossPoint's configuration. The include
 * order is load-bearing (the config defines/renames must be seen first).
 * Firmware uses the SoC ROM's precompiled tinfl instead, so the vendored
 * source compiles to nothing on device; host tests keep it. */
// clang-format off
#if !defined(ESP_PLATFORM)
#include "MinizConfig.h"

#include "../third_party/miniz.c"
#endif
// clang-format on
