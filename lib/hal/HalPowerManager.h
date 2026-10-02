#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cassert>

#include "EnduranceGovernor.h"
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

  // Hysteresis for the low-power entry: millis() of the last request for
  // normal speed (render lock, user input). Low power re-engages only after
  // this dwell expires, so a burst of renders or a background build polling
  // with renders cannot flip-flop the CPU frequency between loop ticks
  // (measured: enter/restore pairs several times per second). Atomic:
  // pokeNormalSpeed() runs on the worker task, the governor loop reads/writes
  // it on the main task (review r5) — a torn 32-bit read is not possible on
  // this ABI, but atomicity also gives the load/store the right memory
  // ordering against the dwell decision.
  std::atomic<unsigned long> lastNormalMs{0};

  // Endurance governor state. The governor owns the idle clock *target* but
  // still moves the clock only through setPowerSaving() below, so it lives here
  // rather than as a second global. governorLowFreqMhz_ == 0 means "no
  // override": the pre-governor behaviour (LOW_POWER_FREQ) is unchanged until a
  // governor is constructed and installs a target.
  EnduranceGovernor endurance_{};
  // The governor's idle target, published as ONE word: the idle clock and the
  // poll-slice policy must be read as a pair, or idle entry can pair a new clock
  // with the previous rung's polling policy. bits 0..15 = idle MHz (0 = no
  // override), bit 16 = poll slices enabled.
  static constexpr uint32_t kPollSlicesBit = 1u << 16;
  std::atomic<uint32_t> governorTarget_{0};

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  // Minimum dwell at full speed after the last normal-speed request before
  // low power may re-engage (see lastNormalMs).
  static constexpr unsigned long NORMAL_POWER_DWELL_MS = 2000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;        // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Endurance governor hooks (see EnduranceGovernor.h). An idle MHz of 0
  // restores the stock LOW_POWER_FREQ floor. setGovernorTarget() publishes both
  // halves in a single release store so idle entry can never observe a mixed
  // (clock, polling) pair.
  void setGovernorTarget(int mhz, bool pollSlices) {
    const uint32_t freq = static_cast<uint32_t>(mhz > 0 ? mhz : 0) & 0xFFFFu;
    governorTarget_.store(freq | (pollSlices ? kPollSlicesBit : 0u), std::memory_order_release);
  }
  uint32_t governorTarget() const { return governorTarget_.load(std::memory_order_acquire); }
  int lowPowerFrequency() const {
    const uint32_t freq = governorTarget() & 0xFFFFu;
    return freq > 0 ? static_cast<int>(freq) : LOW_POWER_FREQ;
  }
  // Whether the idle loop may sleep in poll slices. The governor demotes this
  // when the touch-INT wake source fails verification.
  // Sets the polling half only, preserving the clock half (and vice versa) by
  // read-modify-writing the same atomic under a compare-exchange loop rather than
  // writing a bare field.
  void setIdlePollSlicesEnabled(bool enabled) {
    uint32_t cur = governorTarget_.load(std::memory_order_acquire);
    for (;;) {
      const uint32_t next = enabled ? (cur | kPollSlicesBit) : (cur & ~kPollSlicesBit);
      if (governorTarget_.compare_exchange_weak(cur, next, std::memory_order_acq_rel)) break;
    }
  }
  bool idlePollSlicesEnabled() const { return (governorTarget() & kPollSlicesBit) != 0; }
  // True while the clock is at the idle target — the escalation task uses it to
  // know a heavy job got throttled underneath itself.
  bool isLowPowerActive() const { return isLowPower; }

  EnduranceGovernor& endurance() { return endurance_; }

  // Refresh the full-speed dwell without holding the (single) NormalSpeed
  // lock: background build ticks call this per page so the governor cannot
  // throttle an active build, and re-engage low power the moment the ticks
  // stop. Any-task safe — one 32-bit store, same raciness as the rest of
  // the dwell bookkeeping.
  void pokeNormalSpeed() { lastNormalMs.store(millis(), std::memory_order_relaxed); }

  // Setup wake up GPIO and enter deep sleep. When autoPowerOffTimerUs is
  // non-zero an RTC timer is armed so the device wakes after that many
  // microseconds of dwell (auto power off).
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio, uint64_t autoPowerOffTimerUs = 0);

  // Final software power-off for auto power off: drives the master rail
  // latches LOW (held through sleep), disarms any RTC timer and re-enters
  // deep sleep waking only on the power button. Never returns.
  [[noreturn]] void enterPowerOffSleep(HalGPIO& gpio);

  // True when the battery is on external power / charging: the reader's
  // progress saver must not treat <5% as low battery in that state (design
  // decision log, 2026-09-10).
  bool isBatteryCharging() const;

  // True when external power (USB / charger) is PHYSICALLY present, which is not
  // the same question as isBatteryCharging(): a full battery stops charging with
  // the cable still attached. `known` reports whether the board can observe the
  // input rail at all; when it cannot, the answer is false and `known` is false,
  // so a caller never treats "cannot tell" as "on battery is true".
  bool isExternalPowerPresent(bool* known = nullptr) const;

  // Battery pack voltage in millivolts. Returns false when the active board has
  // no voltage path, or the gauge/ADC read failed, so a caller never renders a
  // real-looking value taken from an unsupported field (same "unknown is not
  // zero" discipline as isExternalPowerPresent's `known`).
  bool getBatteryMillivolts(uint16_t& millivoltsOut) const;

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
  // Clear any staged shutdown marker. Used to suppress a marker staged at
  // sleep entry when the wake was NOT the expected timer wake (e.g. user
  // interrupted auto-power-off dwell with a button press).
  static void clearShutdownMarker();

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
