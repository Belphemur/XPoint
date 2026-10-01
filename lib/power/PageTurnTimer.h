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
  PageTurnTimer() : startMs_(millis()) {}
  ~PageTurnTimer() { powerManager.endurance().notePageTurn(millis() - startMs_); }

  PageTurnTimer(const PageTurnTimer&) = delete;
  PageTurnTimer& operator=(const PageTurnTimer&) = delete;

 private:
  unsigned long startMs_;
};
