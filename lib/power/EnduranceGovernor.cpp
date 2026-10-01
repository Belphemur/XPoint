#include "EnduranceGovernor.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

#include "WifiLeakGuard.h"

namespace {
// Core-0 worker convention: every background task in this firmware pins to core
// 0 so one constant works on the dual-core S3 and the single-core C3
// (pinning to core 1 aborts at startup on the C3).
constexpr BaseType_t kCore = 0;
// 2048 B is enough: the task only checks two flags and calls setCpuFrequencyMhz.
constexpr uint32_t kEscalationStackBytes = 2048;
constexpr UBaseType_t kEscalationPriority = 1;
constexpr unsigned long kEscalationPeriodMs = 250;

// Touch-INT verification cadence. Fast enough to catch a gesture's assertion
// window, slow enough that the probe is not itself a wake source.
constexpr unsigned long kWakeProbeIntervalMs = 250;
// A gesture that never got its INT line asserted within this window after the
// line was read at rest is treated as a missed wake.
constexpr unsigned long kWakeProbeSettleMs = 1500;

// Strike byte layout (design doc §3.1 names bit0/bit1):
//   bit 0      idle-clock strikes >= 1
//   bit 1      light-sleep strikes >= 1
//   bits 2..4  persisted ladder floor (strategy index), so a promotion survives
//              the strike clear and the next boot stays at the safer clock.
constexpr uint8_t kStrikeIdleBit = 0x01;
constexpr uint8_t kStrikeLightSleepBit = 0x02;
constexpr uint8_t kRatchetShift = 2;
constexpr uint8_t kRatchetMask = 0x07;

uint8_t encodeStateByte(const endurance::StrikeState& strikes, uint8_t ratchet) {
  const uint8_t clampedRatchet = ratchet > 7 ? 7 : ratchet;
  return static_cast<uint8_t>(strikes.toByte() | static_cast<uint8_t>(clampedRatchet << kRatchetShift));
}

endurance::StrikeState decodeStrikes(uint8_t raw) { return endurance::strikeStateFromByte(raw); }

uint8_t decodeRatchet(uint8_t raw) {
  const uint8_t ratchet = static_cast<uint8_t>((raw >> kRatchetShift) & kRatchetMask);
  return ratchet >= endurance::kStrategyCount ? static_cast<uint8_t>(endurance::kStrategyCount - 1) : ratchet;
}

const char* reasonText(EnduranceGovernor::InstabilityReason reason) {
  switch (reason) {
    case EnduranceGovernor::InstabilityReason::ClockSwitchFailure:
      return "clock-switch";
    case EnduranceGovernor::InstabilityReason::Watchdog:
      return "watchdog";
    case EnduranceGovernor::InstabilityReason::Crash:
      return "crash";
    case EnduranceGovernor::InstabilityReason::None:
    default:
      return "none";
  }
}

// xTaskCreatePinnedToCore takes a plain function pointer, so the member body
// needs a trampoline carrying `this` through the task argument.
void escalationTrampoline(void* arg) { static_cast<EnduranceGovernor*>(arg)->escalationTaskEntry(); }
}  // namespace

EnduranceGovernor::EnduranceGovernor() = default;

EnduranceGovernor& EnduranceGovernor::instance() { return powerManager.endurance(); }

void EnduranceGovernor::begin() {
  if (began_) return;
  began_ = true;
  bootClockMHz_ = getCpuFrequencyMhz();
  bootStartMs_ = millis();

  loadAndMigrateStrikes();
  refreshFromSettings();
  applyStrategy();

  LOG_INF("PWR", "Endurance governor ready: boot at %d MHz, safety=%u", bootClockMHz_, strikes_.toByte());

  if (xTaskCreatePinnedToCore(escalationTrampoline, "endurance_gov", kEscalationStackBytes, this, kEscalationPriority,
                              &taskHandle_, kCore) != pdPASS) {
    taskHandle_ = nullptr;
    LOG_ERR("PWR", "Governor task creation failed; on-demand escalation disabled");
  }
}

void EnduranceGovernor::loadAndMigrateStrikes() {
  const uint8_t oldState = [&] {
    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, false)) return static_cast<uint8_t>(0);
    const uint8_t raw = prefs.getUChar(kNvsStateKey, 0);
    prefs.end();
    return raw;
  }();

  strikes_ = decodeStrikes(oldState);
  persistedFloor_ = decodeRatchet(oldState);

  const uint8_t base = endurance::kProfileBaseStrategy[static_cast<uint8_t>(profile())];
  const uint8_t promoted = endurance::promote(base, strikes_.idleStrikes, strikes_.lightSleepStrikes);
  // The floor only ratchets up: a device that once proved unstable at 10 MHz
  // never silently returns there because a later boot read a clean byte.
  strategyIndex_ = promoted > persistedFloor_ ? promoted : persistedFloor_;

  const uint8_t newState = encodeStateByte(endurance::StrikeState{}, strategyIndex_);
  if (newState != oldState) {
    LOG_INF("PWR", kEnduranceMigrationLogFormat, static_cast<unsigned>(oldState), static_cast<unsigned>(newState));
    Preferences prefs;
    if (prefs.begin(kNvsNamespace, false)) {
      prefs.putUChar(kNvsStateKey, newState);
      prefs.end();
    } else {
      LOG_ERR("PWR", "endurance.state migration write failed (NVS open)");
    }
  }
  // The strikes have been consumed by the promotion above.
  strikes_ = endurance::StrikeState{};
}

void EnduranceGovernor::setProfile(endurance::Profile p) {
  profile_ = p;
  refreshFromSettings();
  applyStrategy();
}

void EnduranceGovernor::refreshFromSettings() {
  profile_ = endurance::clampProfile(static_cast<uint8_t>(profile_));
  const uint8_t base = endurance::kProfileBaseStrategy[static_cast<uint8_t>(profile_)];
  const uint8_t promoted = endurance::promote(base, strikes_.idleStrikes, strikes_.lightSleepStrikes);
  strategyIndex_ = promoted > persistedFloor_ ? promoted : persistedFloor_;
  strategy_ = endurance::kStrategies[strategyIndex_];
}

void EnduranceGovernor::applyStrategy() {
  endurance::ProfileDefaults defaults = endurance::defaultsFor(strategy_, profile_);
  // Crossfire's ladder logs "render 80 MHz" because 80 MHz IS its boot clock.
  // XPoint boots at 240 MHz on the S3, so pinning renders down to 80 would be a
  // straight performance regression; the render clock is therefore floored at the
  // boot clock and never lowered.
  renderClockMHz_ = defaults.renderClockMHz > bootClockMHz_ ? defaults.renderClockMHz : bootClockMHz_;
  idleClockMHz_ = defaults.idleClockMHz;
  idlePollSlices_ = defaults.idlePollSlices;

  // The clock still moves only through HalPowerManager::setPowerSaving(); the
  // governor supplies the target, not the transition.
  powerManager.setLowPowerFrequency(idleClockMHz_);
  powerManager.setIdlePollSlicesEnabled(idlePollSlices_);
  LOG_INF("PWR", "Profile %u: idle %d / active %d / burst %d / render %u MHz, light sleep %s",
          static_cast<unsigned>(profile_), idleClockMHz_, bootClockMHz_, bootClockMHz_,
          static_cast<unsigned>(renderClockMHz_), idlePollSlices_ ? "enabled" : "disabled");
}

void EnduranceGovernor::reportInstability(InstabilityReason reason) {
  if (reason == InstabilityReason::ClockSwitchFailure) {
    strikes_.idleStrikes = 1;
  } else if (reason == InstabilityReason::Watchdog) {
    strikes_.lightSleepStrikes = 1;
  }
  const uint8_t byte = encodeStateByte(strikes_, strategyIndex_);
  // Persist synchronously: the common reason to strike is that the device is
  // about to crash or reset, and the escalation task may never run again.
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, false)) {
    prefs.putUChar(kNvsStateKey, byte);
    prefs.end();
  } else {
    LOG_ERR("PWR", "endurance.state strike write failed (NVS open)");
  }
  strikesDirty_ = true;
  LOG_ERR("PWR", "Endurance strike recorded: %s (state 0x%02x)", reasonText(reason), static_cast<unsigned>(byte));
}

void EnduranceGovernor::escalationTaskEntry() {
  for (;;) {
    if (strikesDirty_) {
      strikesDirty_ = false;
      // Promote this session immediately rather than waiting for the next boot.
      const uint8_t base = endurance::kProfileBaseStrategy[static_cast<uint8_t>(profile())];
      const uint8_t promoted = endurance::promote(base, strikes_.idleStrikes, strikes_.lightSleepStrikes);
      if (promoted > strategyIndex_) {
        strategyIndex_ = promoted;
        applyStrategy();
        LOG_ERR("PWR", "Escalated to strategy %u after strike", static_cast<unsigned>(strategyIndex_));
      }
      strikes_ = endurance::StrikeState{};
    }
    // A heavy job still running must never be throttled by an idle tick from
    // another task; restore the clock here so the job's window is honoured even
    // if the main loop asked for idle in the meantime.
    if (heavyJobs_.load(std::memory_order_relaxed) > 0 && powerManager.isLowPowerActive()) {
      powerManager.setPowerSaving(false);
    }
    vTaskDelay(pdMS_TO_TICKS(kEscalationPeriodMs));
  }
}

const char* EnduranceGovernor::wakeVerdictText() const {
  switch (wakeVerdict_) {
    case WakeVerdict::Verified:
      return "verified";
    case WakeVerdict::DemotedToPoll:
      return "poll";
    case WakeVerdict::Unverified:
    default:
      return "unverified";
  }
}

void EnduranceGovernor::tick() {
  const unsigned long now = millis();
  const unsigned long deltaMs = lastTickMs_ == 0 ? 0 : now - lastTickMs_;
  lastTickMs_ = now;
  bootMs_ = static_cast<uint32_t>(now - bootStartMs_);

  const bool onUsb = powerManager.isBatteryCharging();
  drain_.setOnUsbPower(onUsb);
  drain_.sample(powerManager.getBatteryPercentage(), now);

  // Idle vs nap accounting for the Full overlay. Which bucket a tick lands in is
  // decided by the clock state the governor itself asked for, not by a timer.
  if (deltaMs > 0) {
    if (powerManager.isLowPowerActive()) {
      napMs_ += static_cast<uint32_t>(deltaMs);
    } else {
      idleMs_ += static_cast<uint32_t>(deltaMs);
    }
  }

  WifiLeakGuard::poll(now);

  // Touch-INT verification, cached after the first verdict.
  if (wakeVerdict_ == WakeVerdict::Unverified && BoardConfig::hasTouch()) {
    const int8_t irq = BoardConfig::ACTIVE.touch.irq;
    if (irq >= 0) {
      pinMode(irq, INPUT);
      const bool asserted =
          BoardConfig::ACTIVE.touch.irqActiveLow ? (digitalRead(irq) == LOW) : (digitalRead(irq) == HIGH);
      const bool touchSeen = gpio.wasTouchActivity();
      if (touchSeen) {
        if (lastWakeProbeMs_ != 0 && now - lastWakeProbeMs_ <= kWakeProbeSettleMs) {
          // A gesture arrived inside the settle window with the INT line
          // asserted: the line is a usable wake source.
          wakeVerdict_ = WakeVerdict::Verified;
          LOG_INF("PWR", "Touch INT verified as a light-sleep wake source");
        } else {
          lastWakeProbeMs_ = now;
        }
      } else if (asserted) {
        // INT asserted with no gesture behind it. A line that ghosts cannot be
        // used to skip work, so demote to always-poll.
        wakeVerdict_ = WakeVerdict::DemotedToPoll;
        LOG_ERR("PWR", "Touch INT wakes without touches; using poll slices");
      }
    }
  }
}

void EnduranceGovernor::beginSleepWindow() {
  const unsigned long now = millis();
  sleepStartMs_ = now;
  drain_.beginSleep(drain_.lastPercent(), now, powerManager.isBatteryCharging());
}

void EnduranceGovernor::endSleepWindow() {
  const unsigned long now = millis();
  const unsigned long slept = now - sleepStartMs_;
  drain_.endSleep(drain_.lastPercent(), now, powerManager.isBatteryCharging());
  if (naps_ < 0xFFFFFFFFU) naps_++;
  napMs_ += static_cast<uint32_t>(slept);
}

void EnduranceGovernor::notePageTurn(const unsigned long renderMs, const unsigned long panelMs,
                                     const unsigned long totalMs) {
  if (pageRenders_ < 0xFFFFFFFFU) pageRenders_++;
  pageRenderMs_ += static_cast<uint32_t>(renderMs);
  panelRefreshMs_ += static_cast<uint32_t>(panelMs);
  pageTurnMs_ += static_cast<uint32_t>(totalMs);
}

void EnduranceGovernor::setOnUsbPower(const bool onUsb) { drain_.setOnUsbPower(onUsb); }

EnduranceGovernor::Lock::Lock() {
  EnduranceGovernor& gov = EnduranceGovernor::instance();
  gov.beginHeavyJob();
  // Raise the clock for the whole job now rather than at the next idle tick.
  powerManager.setPowerSaving(false);
  valid_ = true;
}

EnduranceGovernor::Lock::~Lock() {
  if (valid_) EnduranceGovernor::instance().endHeavyJob();
}
