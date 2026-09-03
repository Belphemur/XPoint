#pragma once

#include <Arduino.h>
#include <Rtc.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable uint16_t _cachedYear = 2000;
  mutable uint8_t _cachedMonth = 1;
  mutable uint8_t _cachedDay = 1;
  mutable bool _hasCachedDate = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

  // Publishes the RTC's UTC wall clock into the ESP32's software clock, so
  // time() is a real epoch from power-on instead of a seconds-since-boot
  // counter. Defined and called only under FREEINK_CAP_RTC; on boards without
  // an RTC the declaration alone costs nothing and is never referenced.
  void seedSystemClockFromRtc();

  // Dev-only diagnostic (compiles to a no-op below LOG_LEVEL 2 or without
  // ENABLE_SERIAL_LOG): reads the RTC time and status registers straight off
  // the bus and logs them as raw BCD, bypassing Rtc::DateTime. The HAL only
  // ever sees the decoded values, which hides whether a rejected date came from
  // a POR-zeroed chip, a set century bit, or plain garbage. Called only on the
  // path where the calendar-date gate rejects the RTC contents.
  void logRawRegisters(const char* tag);

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Get current date and time. Returns false if RTC is not available or
  // returns an invalid date.
  bool getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // offsetMinutes: signed UTC offset in minutes (e.g. -240 = UTC-4, +330 = UTC+5:30, 0 = UTC).
  //   Clamped to [-720, +840] (UTC-12:00 .. UTC+14:00).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, int offsetMinutes = 0, bool use12Hour = false) const;

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();
};
