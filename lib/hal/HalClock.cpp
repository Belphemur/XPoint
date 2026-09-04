#include "HalClock.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
#include <Wire.h>
#include <soc/soc_caps.h>
#endif

HalClock halClock;  // Singleton instance

namespace {
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
// Same bus arbitration as the SDK's Rtc: the RTC shares a bus with the touch
// controller, so the bus index comes from the active board profile. Deliberately
// does NOT call wire.begin() — the bus is already up by the time any probe runs,
// and re-initialising it would drop the touch controller's configuration.
TwoWire& rtcProbeWire() {
  const auto& s = BoardConfig::ACTIVE.sensors;
#if SOC_I2C_NUM > 1
  return s.i2cBus == 1 ? Wire1 : Wire;
#else
  (void)s;
  return Wire;
#endif
}
#endif  // ENABLE_SERIAL_LOG && LOG_LEVEL >= 2
}  // namespace

void HalClock::logRawRegisters(const char* tag) {
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  const auto& s = BoardConfig::ACTIVE.sensors;
  if (s.rtcAddr == 0 || s.rtcType == BoardConfig::RtcType::None) return;

  // Register addresses are fixed by each chip; mirrored from the SDK's Rtc
  // because the SDK is a submodule and this is a read-only diagnostic.
  uint8_t timeReg = 0x00;
  uint8_t statusReg = 0x00;
  switch (s.rtcType) {
    case BoardConfig::RtcType::Pcf8563:
      timeReg = 0x02;
      statusReg = 0x00;
      break;
    case BoardConfig::RtcType::Ds3231:
      timeReg = 0x00;
      statusReg = 0x0F;
      break;
    case BoardConfig::RtcType::Rx8130:
      timeReg = 0x10;
      statusReg = 0x1D;
      break;
    case BoardConfig::RtcType::None:
      return;
  }

  auto& wire = rtcProbeWire();
  uint8_t raw[7] = {};
  wire.beginTransmission(s.rtcAddr);
  wire.write(timeReg);
  if (wire.endTransmission(false) != 0) {
    LOG_DBG("CLK", "raw[%s]: bus error at reg 0x%02X", tag, timeReg);
    return;
  }
  if (wire.requestFrom(s.rtcAddr, static_cast<uint8_t>(sizeof(raw)), static_cast<uint8_t>(true)) < sizeof(raw)) {
    LOG_DBG("CLK", "raw[%s]: short read at reg 0x%02X", tag, timeReg);
    return;
  }
  for (uint8_t& b : raw) b = wire.read();

  uint8_t status = 0xFF;
  wire.beginTransmission(s.rtcAddr);
  wire.write(statusReg);
  if (wire.endTransmission(false) == 0 &&
      wire.requestFrom(s.rtcAddr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) >= 1) {
    status = wire.read();
  }

  // BCD block order is chip-dependent: PCF8563/RX8130 read ss mm hh DD WW MM YY,
  // DS3231 reads ss mm hh WW DD MM YY. Bit 7 of MM is the PCF8563 century flag.
  LOG_DBG("CLK", "raw[%s]: type=%u status=0x%02X blk=%02X %02X %02X %02X %02X %02X %02X", tag,
          static_cast<unsigned>(s.rtcType), status, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6]);
#else
  (void)tag;
#endif
}

#if FREEINK_CAP_RTC
// The ESP32 has two unrelated clocks and nothing else in the firmware joins
// them: the I2C RTC (battery-backed, read via getTime()/getDateTime()) and the
// software clock behind time(), which lives in DRAM and is otherwise only ever
// written by configTzTime() inside syncFromNTP(). Left unseeded, time() returns
// a seconds-since-boot counter, so every consumer of it — most importantly
// clockEffectiveOffsetMin(), which hands the epoch to the AceTime zone resolver
// — computes against 1970 and picks the winter offset instead of the current
// DST one. Publishing the RTC's UTC wall clock here makes time() correct from
// power-on, with no network involved.
//
// FREEINK_CAP_RTC is the same flag that decides whether the Rtc lib links its
// I2C driver or the stub bodies, so the seed is compiled out exactly where
// getDateTime() could never succeed anyway.
void HalClock::seedSystemClockFromRtc() {
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
  if (!getDateTime(year, month, day, hour, minute)) {
    // Only the date is validated (getTime() accepts any hour/minute), so a flat
    // coin cell shows up here first. Dump the raw block so the failure mode is
    // identifiable from the log alone.
    logRawRegisters("badDate");
    LOG_INF("CLK", "seedSystemClockFromRtc: RTC date invalid (year=%u month=%u day=%u)", static_cast<unsigned>(year),
            static_cast<unsigned>(month), static_cast<unsigned>(day));
    return;
  }

  // getDateTime() validates the date only — it accepts any hour or minute value.
  // mktime() normalises those instead of rejecting them (e.g. hour 99 advances
  // the date silently before settimeofday() publishes the epoch), so check
  // explicitly here. Anything outside these bounds means the RTC is reporting
  // garbage even though the date looked valid.
  if (hour > 23 || minute > 59) {
    LOG_INF("CLK", "seedSystemClockFromRtc: RTC time invalid (hour=%u minute=%u)", static_cast<unsigned>(hour),
            static_cast<unsigned>(minute));
    return;
  }

  struct tm timeinfo{};
  timeinfo.tm_year = static_cast<int>(year) - 1900;
  timeinfo.tm_mon = static_cast<int>(month) - 1;
  timeinfo.tm_mday = static_cast<int>(day);
  timeinfo.tm_hour = static_cast<int>(hour);
  timeinfo.tm_min = static_cast<int>(minute);
  timeinfo.tm_sec = 0;
  timeinfo.tm_isdst = 0;
  // mktime() interprets the struct as local time, but the RTC holds UTC, so pin
  // TZ to UTC for the conversion. syncFromNTP() re-pins the same value when it
  // calls configTzTime("UTC0", ...), and no reader calls localtime().
  setenv("TZ", "UTC0", 1);
  tzset();
  const time_t epoch = mktime(&timeinfo);
  if (epoch == static_cast<time_t>(-1)) {
    LOG_INF("CLK", "seedSystemClockFromRtc: mktime rejected the date/time");
    return;
  }

  const timeval tv{epoch, 0};
  settimeofday(&tv, nullptr);
  LOG_INF("CLK", "System clock seeded from RTC: %04u-%02u-%02u %02u:%02u UTC", static_cast<unsigned>(year),
          static_cast<unsigned>(month), static_cast<unsigned>(day), static_cast<unsigned>(hour),
          static_cast<unsigned>(minute));
}
#endif  // FREEINK_CAP_RTC

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
#if FREEINK_CAP_RTC
  // Runtime gate on top of the CAP: the driver can be linked while this board's
  // I2C probe finds no chip.
  if (_available) seedSystemClockFromRtc();
#endif
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
  // Arms the SNTP request without blocking; the software clock is updated when
  // the reply lands, which the poll loop below waits for.
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      // Boards without an external RTC still get a correct system clock from
      // SNTP here; there is no hardware to persist to, so report success once
      // the system clock is set.
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
      logRawRegisters("preSet");
      if (_sdkRtc.set(dt)) {
        // Read back what actually landed, rather than trusting the struct we
        // handed over: a truncated or rejected burst shows up here immediately.
        logRawRegisters("postSet");
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
