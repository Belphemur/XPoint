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
  void setProfile(endurance::Profile p);

  // The published clock/poll targets are written by the escalation task and read
  // by the render path and the overlay, so every accessor takes the state mutex:
  // a bare read could otherwise observe idleClockMHz_ and idlePollSlices_ from
  // two different rungs.
  int idleClockMHz() const {
    lockState();
    const int v = idleClockMHz_;
    unlockState();
    return v;
  }
  int renderClockMHz() const {
    lockState();
    const int v = renderClockMHz_;
    unlockState();
    return v;
  }
  bool idlePollSlices() const {
    lockState();
    const bool v = idlePollSlices_;
    unlockState();
    return v;
  }
  uint8_t strategyIndex() const {
    lockState();
    const uint8_t v = strategyIndex_;
    unlockState();
    return v;
  }
  endurance::Profile profile() const {
    lockState();
    const endurance::Profile p = endurance::clampProfile(static_cast<uint8_t>(profile_));
    unlockState();
    return p;
  }
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

  // Whether the app currently expects the radio to be UP. The Wi-Fi leak
  // guard's no-session detector only runs when nothing owns the radio (the home
  // screen), so a long legitimate transfer can never be cut mid-flight by an
  // elapsed-time threshold. Set from main.cpp where the activity stack is known.
  void setUnownedRadioShutdownAllowed(bool allowed) {
    unownedRadioShutdownAllowed_.store(allowed, std::memory_order_relaxed);
  }

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
  // Write the persisted ladder floor so an in-session promotion survives reboot.
  void persistFloor(uint8_t floorIndex);
  void refreshFromSettings();
  // Re-resolve the rung and the strike floor from an explicit strike batch, and
  // return the resulting rung. The batch is an argument so the escalation task can
  // consume it atomically BEFORE promoting: reading whatever happens to be live at
  // resolve time cannot distinguish a strike this promotion already accounted for
  // from one reported after it. `advanceFloor` marks the sole strike consumer --
  // every other caller passes false with a zero batch, so a profile change can move
  // the rung but can never ratchet the floor. See EnduranceLadder.h::resolveRung.
  // Returning the index is essential rather than convenient: the caller must not
  // re-read strategyIndex_ across a gap where another task could have recomputed it
  // from a batch this promotion never saw.
  uint8_t resolveLadder(uint8_t idleStrikes, uint8_t lightStrikes, bool advanceFloor);
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
  mutable SemaphoreHandle_t stateMutex_ = nullptr;
  void lockState() const;
  void unlockState() const;
  // Fail the touch-INT wake verification: record the verdict and switch the
  // input manager back to its tight poll cadence. Sticky, so a later profile or
  // ladder refresh cannot silently re-enable the cadence that drops touches.
  void demoteToPollSlices(const char* reason);

  endurance::Profile profile_ = endurance::Profile::Endurance;
  std::atomic<uint8_t> strikesIdle_{0};
  std::atomic<uint8_t> strikesLightSleep_{0};
  uint8_t strategyIndex_ = 0;
  int idleClockMHz_ = 10;
  int renderClockMHz_ = 80;
  int bootClockMHz_ = 240;
  bool idlePollSlices_ = true;
  // Set once the touch-INT verification fails. applyStrategy() honours it, so
  // the demotion survives every subsequent refresh and strike escalation.
  std::atomic<bool> pollSlicesDemoted_{false};
  bool began_ = false;
  // Set when the state mutex could not be allocated: the governor then runs no
  // task and mutates nothing, so the unsynchronised accessors stay safe.
  bool disabled_ = false;

  // Heavy-job refcount. Held up by Lock; the escalation task restores the clock
  // if the main loop asked for idle while a heavy job is still running. Atomic:
  // Lock is taken from whatever task runs the job, the counter is read by the
  // core-0 escalation task.
 public:
  void beginHeavyJob() { heavyJobs_.fetch_add(1, std::memory_order_relaxed); }
  void endHeavyJob() { heavyJobs_.fetch_sub(1, std::memory_order_relaxed); }
  bool heavyJobActive() const { return heavyJobs_.load(std::memory_order_relaxed) > 0; }

  // Whether a running heavy job is allowed to hold the clock up. Backed by the
  // user's "Heavy-Job Boost" setting; read by HalPowerManager::setPowerSaving().
  void setHeavyJobHoldEnabled(bool enabled) { heavyJobHold_.store(enabled, std::memory_order_relaxed); }
  bool heavyJobHoldEnabled() const { return heavyJobHold_.load(std::memory_order_relaxed); }

 private:
  std::atomic<int> heavyJobs_{0};
  std::atomic<bool> heavyJobHold_{true};

  // Ladder floor recovered from NVS. Ratchets up ONLY through a strike batch the
  // escalation task consumed (resolveLadder's advanceFloor): a profile change
  // selects a rung within it, never raises it. A device that once proved unstable
  // at 10 MHz never silently returns there.
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
  // Probe cadence. Without it the verification ran on every main-loop pass, so
  // an untouched device (verdict stays Unverified) paid a gpio_config + read
  // hundreds of times a second — work that defeats the power goal of the very
  // feature it serves, and which the interval exists to prevent.
  unsigned long lastWakeProbeRunMs_ = 0;
  int8_t wakeProbePinConfigured_ = -1;
  // Consecutive probes that saw the INT line asserted with no gesture behind it.
  // The input manager runs its own 10 ms task, so a single asynchronous sample
  // can legitimately catch the line asserted a moment before the gesture is
  // published; one sample must not permanently demote the device.
  uint8_t ghostStreak_ = 0;

  TaskHandle_t taskHandle_ = nullptr;
  // Set by reportInstability() on any task, consumed by the escalation task.
  // exchange() rather than a plain clear so a strike recorded between the load
  // and the clear cannot be swallowed.
  std::atomic<bool> strikesDirty_{false};
  // Read/written by both tasks; the mutex covers it, but making it atomic keeps
  // the verdict readable from the overlay without taking the lock.
  std::atomic<WakeVerdict> wakeVerdict_{WakeVerdict::Unverified};
  std::atomic<bool> unownedRadioShutdownAllowed_{false};

  friend class HalPowerManager;
};
