#pragma once

#include <PowerDrainMonitor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

#include "EnduranceLadder.h"

// HAL-adjacent owner of CPU clock switching and the idle state machine.
//
// The governor does NOT bypass HalPowerManager::setPowerSaving(bool) — that
// stays the single funnel through which the clock moves. What the governor adds
// is a *target* (EnduranceLadder.h) instead of the binary 80 MHz floor, a
// persisted crash-strike ladder that makes 10 MHz self-healing, and a core-0
// task that holds a heavy job's clock up while it runs.
//
// Design source: docs/design/2026-10-01-endurance-governor-power-stats.md §3.1.

class EnduranceGovernor {
 public:
  // Why a caller believes the current clock target is unstable. Recorded as a
  // strike; the reason is kept only for logging (the persisted state is a
  // single byte by design).
  enum class InstabilityReason : uint8_t {
    None = 0,
    ClockSwitchFailure,  // setCpuFrequencyMhz() refused the governor's target
    Watchdog,            // a task overran its watchdog deadline
    Crash,               // re-armed from the boot-time marker on restart
  };

  // Read the persisted strike byte, migrate it into a (higher) ladder strategy
  // and apply the resulting clock target. Safe to call once from setup().
  void begin();

  // Main-loop tick: feeds the drain monitor a battery sample, runs the
  // touch-INT verification, and enforces any outstanding heavy job. Cheap; call
  // it from loop() alongside the existing powerManager.setPowerSaving() calls.
  void tick();

  // Heavy-job accounting. A heavy job (page render, Wi-Fi activation) raises the
  // clock for its whole duration; the returned token lowers it again.
  class Lock {
   public:
    Lock();
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
    bool valid() const { return valid_; }

   private:
    bool valid_ = false;
  };

  // Record a detected instability. Bumps the persisted strike byte IMMEDIATELY
  // (so a crash in the same instant still leaves the mark behind) and promotes
  // the ladder in this session via the escalation task.
  void reportInstability(InstabilityReason reason);

  // Profile + ladder accessors (settings-backed; see CrossPointSettings).
  endurance::Profile profile() const { return endurance::clampProfile(static_cast<uint8_t>(profile_)); }
  void setProfile(endurance::Profile p);

  int idleClockMHz() const { return idleClockMHz_; }
  int renderClockMHz() const { return renderClockMHz_; }
  bool idlePollSlices() const { return idlePollSlices_; }
  uint8_t strategyIndex() const { return strategyIndex_; }
  int bootClockMHz() const { return bootClockMHz_; }

  endurance::StrikeState strikes() const {
    return endurance::StrikeState{strikesIdle_.load(std::memory_order_relaxed),
                                  strikesLightSleep_.load(std::memory_order_relaxed)};
  }

  PowerDrainMonitor& drain() { return drain_; }
  const PowerDrainMonitor& drain() const { return drain_; }

  // ---- touch-INT wake verification (design doc §3.1) ----
  //
  // Crossfire arms the GT911 INT as a light-sleep wake source and demotes to
  // poll slices when the line misses or ghosts. XPoint never sleeps, so the
  // same three-step procedure runs against the mechanism that does exist here:
  // the GT911 INT line as the authority for "a touch is pending", which lets the
  // input manager skip its I2C poll. A line that is not trustworthy must not be
  // used to skip work, so a failed verdict demotes to always-poll.
  enum class WakeVerdict : uint8_t { Unverified = 0, Verified = 1, DemotedToPoll = 2 };
  WakeVerdict wakeVerdict() const { return wakeVerdict_.load(std::memory_order_relaxed); }
  const char* wakeVerdictText() const;

  // ---- per-sleep window hook (overlay's "Last sleep" row) ----
  void beginSleepWindow();
  void endSleepWindow();

  // Counters the Full overlay reports.
  uint32_t naps() const { return naps_; }
  uint32_t bootMs() const { return bootMs_; }
  uint32_t napMs() const { return napMs_; }
  uint32_t idleMs() const { return idleMs_; }
  uint32_t pageRenders() const { return pageRenders_; }
  uint32_t pageTurnMs() const { return pageTurnMs_; }
  // Records one completed page render: the wall time from entering the render
  // path to returning from it, which covers both the drawing and the panel
  // submission. The decompiled overlay splits this into "render" + "panel" but
  // this render pipeline exposes no per-stage timestamps, and inventing a split
  // would report two numbers that were never measured.
  void notePageTurn(unsigned long totalMs);

  void setOnUsbPower(bool onUsb);

  // Singleton. The instance lives inside HalPowerManager's already-allocated
  // object graph (see HalPowerManager::endurance()) so the port adds no new
  // DRAM static of its own.
  static EnduranceGovernor& instance();

  // NVS namespace/key.
  static constexpr const char* kNvsNamespace = "endurance";
  static constexpr const char* kNvsStateKey = "state";
  // Migration log tag; matches the decompiled Crossfire string. A macro, not a
  // constexpr: the LOG_* macros paste the format onto "\n" and so require a
  // string literal at the call site.
#define kEnduranceMigrationLogFormat "Migrated endurance.2 safety state 0x%02x -> 0x%02x"

 private:
  EnduranceGovernor();

  void loadAndMigrateStrikes();
  void refreshFromSettings();
  void applyStrategy();

 public:
  // FreeRTOS body of the escalation task. Public only because
  // xTaskCreatePinnedToCore takes a plain function pointer and the trampoline
  // in the .cpp has to reach it; it is not part of the class's public API.
  void escalationTaskEntry();

 private:
  // Strategy + clock targets are written by the core-0 escalation task (through
  // applyStrategy) and read by the main task's render path and the overlay. They
  // live behind a FreeRTOS mutex rather than being bare members: reading
  // idleClockMHz_ and idlePollSlices_ without a lock could hand the reader a
  // mismatched pair from two different rungs.
  SemaphoreHandle_t stateMutex_ = nullptr;
  void lockState();
  void unlockState();

  endurance::Profile profile_ = endurance::Profile::Endurance;
  std::atomic<uint8_t> strikesIdle_{0};
  std::atomic<uint8_t> strikesLightSleep_{0};
  endurance::Strategy strategy_{endurance::kStrategies[0]};
  uint8_t strategyIndex_ = 0;
  int idleClockMHz_ = 10;
  int renderClockMHz_ = 80;
  int bootClockMHz_ = 240;
  bool idlePollSlices_ = true;
  bool began_ = false;

  // Heavy-job refcount. Held up by Lock; the escalation task restores the clock
  // if the main loop asked for idle while a heavy job is still running. Atomic:
  // Lock is taken from whatever task runs the job, the counter is read by the
  // core-0 escalation task.
 public:
  void beginHeavyJob() { heavyJobs_.fetch_add(1, std::memory_order_relaxed); }
  void endHeavyJob() { heavyJobs_.fetch_sub(1, std::memory_order_relaxed); }
  bool heavyJobActive() const { return heavyJobs_.load(std::memory_order_relaxed) > 0; }

 private:
  std::atomic<int> heavyJobs_{0};

  // Ladder floor recovered from NVS. Only ever ratchets up: a device that once
  // proved unstable at 10 MHz never silently returns there.
  uint8_t persistedFloor_ = 0;
  unsigned long lastTickMs_ = 0;
  unsigned long lastBatterySampleMs_ = 0;
  uint8_t lastBatteryPct_ = PowerDrainMonitor::kInvalid;
  // Profile byte handed over by refreshFromSettings() (which may run on any
  // task, including the web-server task) and adopted by tick() on the main task,
  // so the ladder is only ever re-resolved under the state mutex.
  std::atomic<uint32_t> pendingProfile_{0};

  // Drain measurement + per-sleep window.
  PowerDrainMonitor drain_{};
  unsigned long sleepStartMs_ = 0;
  uint32_t naps_ = 0;
  uint32_t napMs_ = 0;
  uint32_t idleMs_ = 0;
  uint32_t bootMs_ = 0;
  unsigned long bootStartMs_ = 0;
  uint32_t pageRenders_ = 0;
  uint32_t pageTurnMs_ = 0;

  // Touch-INT verification.
  unsigned long lastWakeProbeMs_ = 0;

  TaskHandle_t taskHandle_ = nullptr;
  // Set by reportInstability() on any task, consumed by the escalation task.
  // exchange() rather than a plain clear so a strike recorded between the load
  // and the clear cannot be swallowed.
  std::atomic<bool> strikesDirty_{false};
  // Read/written by both tasks; the mutex covers it, but making it atomic keeps
  // the verdict readable from the overlay without taking the lock.
  std::atomic<WakeVerdict> wakeVerdict_{WakeVerdict::Unverified};

  friend class HalPowerManager;
};
