#pragma once

// Thin wrapper for the generated build/board_features.h. The macros
// FREEINK_CAP_HOME_KEY / FREEINK_CAP_MENU_BUTTON are emitted by
// scripts/gen_board_features.py, which derives them from BoardConfig.h via a
// small host-side dump; consumers gate on `#if FREEINK_CAP_HOME_KEY` /
// `#if FREEINK_CAP_MENU_BUTTON`.
//
// __has_include guard: when the generated header is not present (host builds,
// IDE indexing, or a fresh checkout before PlatformIO's pre: scripts ran), the
// macros default to 0 so the firmware compiles as if no feature were present.
// PlatformIO's pre:extra_scripts entry in platformio.ini guarantees the header
// exists for every firmware build; this guard keeps every other compile path
// (ctest, single-file `gcc -c`, IDEs) buildable without that plumbing.
#if __has_include("../build/board_features.h")
#include "../build/board_features.h"
#else
#define FREEINK_CAP_HOME_KEY    0
#define FREEINK_CAP_MENU_BUTTON 0
#endif