#pragma once

#include <cstdint>

// Host test shim for CrossPointSettings.h — supplies only clockUtcOffsetQ,
// which ReadingStatsUtils uses for the local-time offset.

class CrossPointSettings {
 public:
  uint8_t clockUtcOffsetQ = 48;
  uint8_t trackReadingStats = 1;
  bool shouldTrackReadingStats() const { return trackReadingStats != 0; }
  static CrossPointSettings& getInstance() {
    static CrossPointSettings instance;
    return instance;
  }
};

#define SETTINGS CrossPointSettings::getInstance()
