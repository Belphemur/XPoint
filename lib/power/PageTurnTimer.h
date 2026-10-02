#pragma once

#include <Arduino.h>
#include <HalPowerManager.h>

// Reports one completed page render to the endurance governor.
//
// RAII so every early return in a render path (empty chapter, out-of-bounds page,
// failed SD read, allocation failure) still closes the measurement instead of
// leaving a timer running, and so the readers do not each hand-roll the same
// millis() arithmetic.
//
// One instance per render, and only one: the TTF path has its own render entry
// point, so the legacy path must not also hold a timer or a single page would
// report two samples and double the accumulated page time.
class PageTurnTimer {
 public:
  PageTurnTimer() = default;
  ~PageTurnTimer() {
    if (!cancelled_) powerManager.endurance().notePageTurn(millis() - startMs_);
  }

  // Drop this sample. A TTF pre-render pass warms glyph and layout state and
  // never commits a page, so counting it inflated the average with several
  // samples per displayed page.
  void cancel() { cancelled_ = true; }

  PageTurnTimer(const PageTurnTimer&) = delete;
  PageTurnTimer& operator=(const PageTurnTimer&) = delete;

 private:
  unsigned long startMs_ = millis();
  bool cancelled_ = false;
};
