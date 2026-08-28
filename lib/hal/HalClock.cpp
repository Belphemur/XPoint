#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

HalClock halClock;  // Singleton instance

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _lastPollMs = now;
  _hasCachedTime = true;
  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

namespace {
bool isValidCalendarDate(uint16_t year, uint8_t month, uint8_t day) {
  if (year < 2000 || year > 2099) return false;
  if (month < 1 || month > 12) return false;
  static constexpr uint8_t daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint8_t maxDay = daysInMonth[month - 1];
  if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) maxDay = 29;
  return day >= 1 && day <= maxDay;
}
}  // namespace

bool HalClock::getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  // Date/time fast path is only valid when we already have a cached calendar
  // date. A recent time-only poll from getTime() must not satisfy this.
  if (_hasCachedDate && _lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    year = _cachedYear;
    month = _cachedMonth;
    day = _cachedDay;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    // Without a fresh read we will only return a date we have already validated.
    if (!_hasCachedDate) return false;
    _lastPollMs = now;
    year = _cachedYear;
    month = _cachedMonth;
    day = _cachedDay;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _lastPollMs = now;
  _hasCachedTime = true;

  // Only cache and return dates that pass validation; never propagate a stale
  // date after the RTC starts reporting an invalid calendar.
  if (isValidCalendarDate(dt.year, dt.month, dt.day)) {
    _cachedYear = dt.year;
    _cachedMonth = dt.month;
    _cachedDay = dt.day;
    _hasCachedDate = true;

    year = _cachedYear;
    month = _cachedMonth;
    day = _cachedDay;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  return false;
}

bool HalClock::formatTime(char* buf, size_t bufSize, int offsetMinutes, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset in minutes, clamped to [-12:00, +14:00] so a corrupted
  // persisted/auto-detected value can't push display time out of range.
  if (offsetMinutes < -720) offsetMinutes = -720;
  if (offsetMinutes > 840) offsetMinutes = 840;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetMinutes;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      // Boards without an external RTC (e.g. x4pro) still get a correct system
      // clock from SNTP here; there is no hardware to persist to, so report
      // success once the system clock is set.
      if (!_available) {
        LOG_INF("CLK", "System clock synced via NTP (no external RTC)");
        return true;
      }

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      if (_sdkRtc.set(dt)) {
        _lastPollMs = 0;
        _cachedHour = dt.hour;
        _cachedMinute = dt.minute;
        _hasCachedTime = true;
        _cachedYear = dt.year;
        _cachedMonth = dt.month;
        _cachedDay = dt.day;
        _hasCachedDate = true;
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
