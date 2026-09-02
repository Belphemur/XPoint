#pragma once

// Thin wrapper for the generated build/board_features.h. The macros
// FREEINK_CAP_HOME_KEY / FREEINK_CAP_MENU_BUTTON are emitted by
// scripts/gen_board_features.py, which derives them from BoardConfig.h via a
// small host-side dump; consumers gate on `#if FREEINK_CAP_HOME_KEY` /
// `#if FREEINK_CAP_MENU_BUTTON`.
#include "../build/board_features.h"
