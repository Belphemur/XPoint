#pragma once

#include <cstdint>

// Host test shim for CrossPointSettings.h — supplies only clockUtcOffsetQ,
// which ReadingStatsUtils uses for the local-time offset.

class CrossPointSettings {
 public:
  uint8_t clockUtcOffsetQ = 48;
  static CrossPointSettings& getInstance() {
    static CrossPointSettings instance;
    return instance;
  }
};

#define SETTINGS CrossPointSettings::getInstance()
