#pragma once

#include <cstdint>

class GfxRenderer;
class MappedInputManager;

// Shared frontlight adjustment for every surface that changes it: the control
// center panel, the side-edge drag, and the reader's quick menu. Mutators
// update SETTINGS and the hardware immediately and mark a dirty flag;
// persistIfDirty() performs the single SD write at a lifecycle boundary
// (sleep, power-off, leaving the reader) — never while a value is being
// modified, so a slider drag costs no SD writes.
namespace frontlight {

// Clamps to [FRONTLIGHT_MIN_BRIGHTNESS, 100] and turns the light on.
void setBrightness(uint8_t percent);
// No-op when the panel has no color channel.
void setWarmth(uint8_t percent);
void setOn(bool on);
// Relative; stepping brightness to <=0 turns the light off (drag-to-bottom).
void adjustBrightness(int delta);
void adjustWarmth(int delta);

// One SD write if any mutator has run since the last flush.
void persistIfDirty();

// A side-edge drag, shared by every screen. ActivityManager owns one and feeds
// it each input tick; a drag starts on a touch-down inside the left/right band
// and consumes the touch until release, so page turns and taps never fire on
// the same frames.
class SwipeGesture {
 public:
  // `policy` is the current activity's Activity::allowsFrontlightSwipe().
  // Returns true when this tick's touch is consumed by the drag.
  bool update(MappedInputManager& input, GfxRenderer& renderer, bool policy);

 private:
  bool active_ = false;
  bool leftSide_ = false;  // true = warmth edge, false = brightness edge
  int lastY_ = 0;
};

}  // namespace frontlight