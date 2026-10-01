#pragma once

#include <cstdint>

#include "util/RepeatHoldDiscount.h"

extern unsigned long testNowMs;
inline unsigned long millis() { return testNowMs; }

class MappedInputManager {
 public:
  enum class Button { NavPrevious, NavNext, Left = NavPrevious, Right = NavNext };
  struct Frame {
    uint32_t heldMs = 0;
    uint8_t pressed = 0;
    uint8_t released = 0;
    uint8_t held = 0;
    // Render-stall time the loop task was blocked for since the press edge
    // (the production MappedInputManager accrues this from inter-tick gaps).
    uint32_t stallMs = 0;
  } frame;
  bool wasPressed(Button button) const { return frame.pressed & (1u << static_cast<unsigned>(button)); }
  bool wasReleased(Button button) const { return frame.released & (1u << static_cast<unsigned>(button)); }
  bool isPressed(Button button) const { return frame.held & (1u << static_cast<unsigned>(button)); }
  unsigned long getHeldTime() const { return frame.heldMs; }
  // Uses the SAME discount arithmetic as production, so a capped or otherwise
  // weakened discount cannot pass these tests.
  unsigned long getRepeatHeldTime() const { return repeathold::discount(frame.heldMs, frame.stallMs); }
  // Host tests model the non-PSRAM (C3-class) build: no async poll task.
  static constexpr bool kRepeatHoldExcludesStalls() { return true; }
};
