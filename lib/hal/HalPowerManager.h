#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep. When autoPowerOffTimerUs is
  // non-zero an RTC timer is armed so the device wakes after that many
  // microseconds of dwell (auto power off).
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio, uint64_t autoPowerOffTimerUs = 0);

  // Final software power-off for auto power off: drives the master rail
  // latches LOW (held through sleep), disarms any RTC timer and re-enters
  // deep sleep waking only on the power button. Never returns.
  [[noreturn]] void enterPowerOffSleep(HalGPIO& gpio);

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // Gauge-read health: a transiently failing Coulomb gauge must never surface
  // a frozen or 0% cache as a real low-battery signal (auto-sleep, OTA gate).
  // HEALTHY = last poll succeeded; STALE = repeated/long-failed reads, the
  // reported percentage is the last known-good value; UNSUPPORTED is reserved for
  // a board with no gauge but is currently unused — ADC boards report HEALTHY
  // (their reads cannot fail), so every supported board lands in HEALTHY or STALE.
  enum class BatteryHealthState : uint8_t { HEALTHY = 0, STALE = 1, UNSUPPORTED = 2 };
  BatteryHealthState getBatteryHealthState() const;
  bool isBatteryHealthStale() const;  // true only when state == STALE

  // Dev-only: log battery drain across deep sleep (sleep-entry mV is stashed in
  // RTC memory; on wake we compare against the new reading and the sleep
  // duration). Compiled out unless LOG_LEVEL >= 2 (dev/x4pro builds).
  void logSleepBattery() const;

  // Dev-only: append the last computed sleep-drain row to /.crosspoint/sleep_trace.csv.
  // MUST be called AFTER Storage.begin() (the SD card is mounted by then); logSleepBattery()
  // runs from begin() before the card is up, so the actual SD write is deferred here.
  void flushSleepTrace() const;

  // Dev-only: stage which trigger is putting the device to sleep (0 = auto-timeout,
  // 1 = power-button, 2 = quick-resume) so the SD sleep-trace CSV can record it.
  // Called from enterDeepSleep() before startDeepSleep(); the actual SD row is
  // written at wake by logSleepBattery(). Compiled out unless LOG_LEVEL >= 2.
  static void setSleepReason(uint8_t reason);

  // Dev-only: the last computed sleep-drain result, persisted in RTC memory so
  // the UI can show it after wake (when the serial port is back up). Invalid
  // until the device has slept at least once. Compiled out unless
  // LOG_LEVEL >= 2.
  struct SleepDrain {
    bool valid = false;    // false on cold boot / before first sleep
    uint16_t entryMv = 0;  // battery mV captured just before sleep (for CSV)
    uint16_t wakeMv = 0;   // battery mV at wake (for CSV)
    uint32_t sleptSeconds = 0;
    int deltaMv = 0;         // + = gained charge (was on charger), - = discharged
    double mvPerHour = 0.0;  // signed net rate, mV/h (kept for logging)
    double milliamps = 0.0;  // signed net current, mA (+ = charging, - = discharging)
    bool rateValid = false;  // false when the sleep was too short for a trustworthy rate (rate shown as n/a)
    // Diagnostics: how this sleep segment ended. A wake cause other than the
    // power button (or a non-deep-sleep reset reason) means the device was NOT
    // asleep for the whole interval — it woke spuriously and the reported drain
    // is an average that mixes awake time in. Persisted so the UI/serial can
    // show it. esp_sleep_wakeup_cause_t / esp_reset_reason_t are stable enums.
    uint8_t wakeCause = 0;    // esp_sleep_get_wakeup_cause() at wake
    uint8_t resetReason = 0;  // esp_reset_reason() at wake
  };
  SleepDrain getLastSleepDrain() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
