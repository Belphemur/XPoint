#include "util/ClockFormat.h"

#include <HalTimeZone.h>

#include <cstdio>

namespace ClockFormat {

namespace {
// Renders the zone as "+HH:MM"/"-HH:MM" (UTC renders as "UTC"). Used whenever
// the abbreviation can't be resolved, so the timestamp keeps its meaning.
void formatOffsetZone(char* out, size_t outSize, int offsetMinutes) {
  if (offsetMinutes == 0) {
    snprintf(out, outSize, "UTC");
    return;
  }
  const char sign = offsetMinutes < 0 ? '-' : '+';
  const int minutes = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
  snprintf(out, outSize, "%c%02d:%02d", sign, minutes / 60, minutes % 60);
}
}  // namespace

bool formatTimestamp(char* out, size_t outSize, time_t utcEpoch, int offsetMinutes, const char* zoneId,
                     bool use12Hour) {
  if (out == nullptr || outSize == 0) return false;
  out[0] = '\0';
  if (outSize < 20 || utcEpoch <= 0) return false;

  // Shift into local time first; gmtime_r then decodes it without consulting TZ
  // (the firmware pins TZ to UTC0), so no localtime/DST surprises.
  const time_t localEpoch = utcEpoch + static_cast<time_t>(offsetMinutes) * 60;
  struct tm parts{};
  if (gmtime_r(&localEpoch, &parts) == nullptr) return false;

  char zone[freeink::kZoneAbbrevSize] = "";
  if (!freeink::resolveZoneAbbreviation(zoneId, static_cast<int64_t>(utcEpoch), zone, sizeof(zone))) {
    formatOffsetZone(zone, sizeof(zone), offsetMinutes);
  }

  if (use12Hour) {
    const bool pm = parts.tm_hour >= 12;
    int hour12 = parts.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(out, outSize, "%04d-%02d-%02d %d:%02d:%02d %s %s", parts.tm_year + 1900, parts.tm_mon + 1,
             parts.tm_mday, hour12, parts.tm_min, parts.tm_sec, pm ? "PM" : "AM", zone);
  } else {
    snprintf(out, outSize, "%04d-%02d-%02d %02d:%02d:%02d %s", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
             parts.tm_hour, parts.tm_min, parts.tm_sec, zone);
  }
  return true;
}

}  // namespace ClockFormat
