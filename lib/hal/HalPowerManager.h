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

  // HAL-routed stock-parity shutdown marker (the freeink::PowerManager calls
  // stay inside the HAL; main.cpp must not touch SDK classes directly). See
  // docs/design/shutdown-reason-marker.md.
  //
  //   ShutdownKind             which clean power off happened last time;
  //                            None = no marker (normal boot, crash, brown-out)
  //   takeLastShutdownKind()   read + clear the RTC marker once per boot
  //   stageAutoPowerOff()      record that THIS sleep ends in an automatic
  //                            power off: writes the RTC marker for the next
  //                            boot
  //   stageUserPowerOff()      same, for the manual power-button power off
  enum class ShutdownKind : uint8_t { None = 0, User = 1, AutoOff = 2 };
  static ShutdownKind takeLastShutdownKind();
  static void stageAutoPowerOff();
  static void stageUserPowerOff();

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
