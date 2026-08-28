#pragma once

#include <cstdint>

namespace freeink {

struct TimeZoneInfo {
  bool valid = false;
  char id[40] = "";
  int offsetMin = 0;
  bool isDst = false;
};

// Best-effort time zone detection via IP geolocation (ipwho.is, worldtimeapi.org fallback).
// Blocking (~2-10s over WiFi); returns TimeZoneInfo{valid=false} when detection fails.
TimeZoneInfo detectTimeZoneFromIp();

// Resolve the *live* UTC offset (minutes, signed) for an IANA zone id at a given
// UTC epoch, folding DST automatically from the bundled AceTime zone database.
// This is what keeps the displayed clock correct across DST transitions without
// any network call or a stored static offset.
//
// Returns true and writes `offsetMin` on success. Returns false when the id is
// unknown to the database or `ianaId` is empty/null (caller should fall back to
// its last-detected cached offset in that case).
bool resolveUtcOffsetMinutes(const char* ianaId, int64_t utcEpochSeconds, int& offsetMin);

}  // namespace freeink
