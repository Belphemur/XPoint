#pragma once

#include <cstdint>

// Host test shim for HalClock.h — supplies the single method ReadingStatsUtils
// uses (getCurrentLocalReadingStatsDateTime -> halClock.getDateTime). Always
// reports no RTC so the date-dependent math degrades gracefully.

class HalClock {
 public:
  bool getDateTime(uint16_t&, uint8_t&, uint8_t&, uint8_t&, uint8_t&) const { return false; }
};

inline HalClock halClock;
