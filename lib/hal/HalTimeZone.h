#pragma once

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

}  // namespace freeink
