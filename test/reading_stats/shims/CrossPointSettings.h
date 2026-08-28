#pragma once

#include <cstdint>
#include <ctime>

// Host test shim for CrossPointSettings.h — supplies only the clock offset
// accessor, which ReadingStatsUtils uses for the local-time offset.
//
// The production build provides freeink::resolveUtcOffsetMinutes (AceTime-backed).
// On host there is no zone database, so we stub it to "unresolved" and let the
// accessor fall back to the cached clockTzOffsetMin value.

namespace freeink {
inline bool resolveUtcOffsetMinutes(const char*, int64_t, int&) { return false; }
}  // namespace freeink

class CrossPointSettings {
 public:
  // Auto-detected zone (host tests don't exercise detection; default UTC).
  char clockTimeZoneId[40] = "";
  int16_t clockTzOffsetMin = 0;
  uint8_t clockTzIsDst = 0;
  uint8_t trackReadingStats = 1;

  // Effective signed UTC offset in minutes for display (UTC until detected).
  // Mirrors production clamping so host tests exercise the same range assumptions.
  int clockEffectiveOffsetMin() const {
    if (clockTimeZoneId[0] == '\0') return 0;
    int off = clockTzOffsetMin;
    if (off < -720)
      off = -720;
    else if (off > 840)
      off = 840;
    return off;
  }
  bool shouldTrackReadingStats() const { return trackReadingStats != 0; }
  static CrossPointSettings& getInstance() {
    static CrossPointSettings instance;
    return instance;
  }
};

#define SETTINGS CrossPointSettings::getInstance()
