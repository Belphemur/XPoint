#pragma once

#include <cstdint>

// Host test shim for CrossPointSettings.h — supplies only the clock offset
// accessor, which ReadingStatsUtils uses for the local-time offset.

class CrossPointSettings {
 public:
  // Auto-detected zone (host tests don't exercise detection; default UTC).
  char clockTimeZoneId[40] = "";
  int16_t clockTzOffsetMin = 0;
  uint8_t clockTzIsDst = 0;
  uint8_t trackReadingStats = 1;

  // Effective signed UTC offset in minutes for display (UTC until detected).
  int clockEffectiveOffsetMin() const {
    return (clockTimeZoneId[0] != '\0' && clockTzOffsetMin != 0) ? clockTzOffsetMin : 0;
  }
  bool shouldTrackReadingStats() const { return trackReadingStats != 0; }
  static CrossPointSettings& getInstance() {
    static CrossPointSettings instance;
    return instance;
  }
};

#define SETTINGS CrossPointSettings::getInstance()
