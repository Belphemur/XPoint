#include "EnduranceGovernor.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <Preferences.h>
#include <TrustedTime.h>
#include <esp_system.h>
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

// Battery is sampled on this cadence, not once per loop() pass.
constexpr unsigned long kBatterySampleIntervalMs = 5000;

// RTC slow memory survives deep sleep, so the nap bookkeeping and the pre-sleep
// gauge reading are carried across the reboot that a sleep causes. millis() does
// NOT survive it, which is why no sleep DURATION is persisted: there is no
// cheap boot-survivable clock in this firmware to derive one from, and a
// fabricated duration would put a wrong number in the overlay's per-sleep rate.
static RTC_DATA_ATTR uint8_t _preSleepPct = 0xFF;
// Whether external power was present when the nap began (255 = board cannot
// tell). A sleep that starts or ends on USB must not yield a battery rate.
static RTC_DATA_ATTR uint8_t _preSleepUsb = 0xFF;
static RTC_DATA_ATTR uint32_t _persistedNaps = 0;
// Epoch seconds at sleep entry, when the clock was trustworthy. Zero means "no
// clock", and the sleep window is then closed without a duration rather than
// with an invented one.
static RTC_DATA_ATTR int64_t _preSleepEpoch = 0;

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

void EnduranceGovernor::lockState() const {
  if (stateMutex_ != nullptr) xSemaphoreTake(stateMutex_, portMAX_DELAY);
}
void EnduranceGovernor::unlockState() const {
  if (stateMutex_ != nullptr) xSemaphoreGive(stateMutex_);
}

void EnduranceGovernor::begin() {
  if (began_) return;
  began_ = true;
  stateMutex_ = xSemaphoreCreateMutex();
  if (stateMutex_ == nullptr) {
    // Without it lockState()/unlockState() degrade to no-ops, and the ladder
    // state would be written concurrently by the main tick, the escalation task
    // and a web-server profile change. Running the task unsynchronised is worse
    // than not running it, so start nothing: with no target installed,
    // HalPowerManager keeps the stock LOW_POWER_FREQ behaviour, which is the
    // pre-feature default. The sleep markers are cleared so a later boot does
    // not count a nap the disabled governor never consumed.
    LOG_ERR("PWR", "Governor state mutex allocation failed; endurance governor disabled");
    disabled_ = true;
    _preSleepPct = 0xFF;
    _preSleepEpoch = 0;
    return;
  }
  bootClockMHz_ = getCpuFrequencyMhz();
  bootStartMs_ = millis();

  // A nap staged by the previous boot's sleep entry closes here. This is the only
  // point at which the sleep actually ended, so it is where a real
  // PowerDrainMonitor window is opened and closed — otherwise the overlay's
  // sleep history could never become valid.
  if (_preSleepPct != 0xFF) {
    naps_ = _persistedNaps < 0xFFFFFFFFU ? _persistedNaps + 1 : _persistedNaps;
    _persistedNaps = naps_;

    // A gauge board reports 0% for "unknown" until its first successful read.
    // Recording that as the wake percentage would show a near-100% drop over the
    // sleep and fabricate a drain rate in the overlay's sleep history.
    const bool wakePctKnown = !powerManager.isBatteryHealthStale() &&
                              powerManager.getBatteryHealthState() != HalPowerManager::BatteryHealthState::STALE;
    const uint8_t wakePct =
        wakePctKnown ? static_cast<uint8_t>(powerManager.getBatteryPercentage()) : PowerDrainMonitor::kInvalid;
    const int64_t wakeEpoch = trustedtime::trustedNow();
    unsigned long sleptMs = 0;
    bool haveDuration = false;
    if (_preSleepEpoch != 0 && wakeEpoch > _preSleepEpoch) {
      const int64_t sleptSeconds = wakeEpoch - _preSleepEpoch;
      // Guard against an implausible jump rather than reporting a rate derived
      // from a clock step.
      if (sleptSeconds < 30LL * 24LL * 60LL * 60LL) {
        sleptMs = static_cast<unsigned long>(sleptSeconds * 1000LL);
        haveDuration = true;
      }
    }
    // Passing `false` at both endpoints made every restored sleep look like a
    // battery sleep. Re-check the rail at wake and combine it with the staged
    // entry state: a nap that began or ended on external power has no battery
    // drain meaning, so it must not yield a rate (or a zero-drain record).
    // Off-battery must be KNOWN at both ends, not merely "not seen on USB".
    // isExternalPowerPresent() returns false with *known == false* on boards that
    // cannot observe the rail, and `_preSleepUsb == 0xFF` is that unknown case;
    // treating either as battery power would let a nap of indeterminate power
    // source produce a battery drain rate.
    bool wakeUsbKnown = false;
    const bool wakeOnUsb = powerManager.isExternalPowerPresent(&wakeUsbKnown);
    const bool knownOnBattery = _preSleepUsb == 0 && wakeUsbKnown && !wakeOnUsb;
    if (haveDuration && wakePct != PowerDrainMonitor::kInvalid && knownOnBattery) {
      drain_.beginSleep(_preSleepPct, 0, false);
      drain_.endSleep(wakePct, sleptMs, false);
      LOG_DBG("PWR", "Woke from a nap at %u%% -> %u%% after %lus (nap #%lu)", static_cast<unsigned>(_preSleepPct),
              static_cast<unsigned>(wakePct), static_cast<unsigned long>(sleptMs / 1000UL),
              static_cast<unsigned long>(naps_));
    } else {
      LOG_DBG("PWR",
              "Woke from a nap at %u%% -> %u%% (no rate: no clock, stale gauge, or power source unknown) (nap #%lu)",
              static_cast<unsigned>(_preSleepPct), static_cast<unsigned>(wakePct), static_cast<unsigned long>(naps_));
    }
    _preSleepPct = 0xFF;
    _preSleepEpoch = 0;
  }

  loadAndMigrateStrikes();
  refreshFromSettings();
  applyStrategy();

  // A crash is the signal the ladder is built around. Only genuine FAULT resets
  // count: ESP_RST_SW is a deliberate `silentRestartTo()` heap-defrag reboot and
  // ESP_RST_EXT/ESP_RST_POWERON/ESP_RST_DEEPSLEEP are normal events, so treating
  // "anything not power-on" as a crash would ratchet a healthy device to the
  // stock clock on every routine restart.
  const auto resetReason = esp_reset_reason();
  const bool faultReset = resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP ||
                          resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_INT_WDT ||
                          resetReason == ESP_RST_WDT || resetReason == ESP_RST_BROWNOUT;
  if (faultReset) {
    LOG_ERR("PWR", "Fault reset (%d) after boot - recording an endurance strike", static_cast<int>(resetReason));
    reportInstability(InstabilityReason::Crash);
  }

  LOG_INF("PWR", "Endurance governor ready: boot at %d MHz, safety=%u", bootClockMHz_, strikes().toByte());

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

  strikesIdle_.store(decodeStrikes(oldState).idleStrikes, std::memory_order_relaxed);
  strikesLightSleep_.store(decodeStrikes(oldState).lightSleepStrikes, std::memory_order_relaxed);
  persistedFloor_ = decodeRatchet(oldState);

  // Boot resolves the rung with the SAME rule the runtime path uses: escalate
  // from the persisted floor, not from the profile base. Using promote(base, ...)
  // here made a boot disagree with the escalation task (which escalates from the
  // floor), so a device already at rung 2 that struck again came back at rung 2.
  const endurance::StrikeState bootStrikes = strikes();
  const uint8_t escalated =
      endurance::escalate(persistedFloor_, bootStrikes.idleStrikes, bootStrikes.lightSleepStrikes);
  const uint8_t base = endurance::kProfileBaseStrategy[static_cast<uint8_t>(profile())];
  const uint8_t wanted = escalated > base ? escalated : base;
  // The floor only ratchets up: a device that once proved unstable at 10 MHz
  // never silently returns there because a later boot read a clean byte.
  strategyIndex_ = wanted > persistedFloor_ ? wanted : persistedFloor_;
  // Carry the promotion into the in-memory floor. Without this the very next
  // refreshFromSettings() re-derives the rung from the (now cleared) strikes and
  // the boot's promotion evaporates before the clock is ever set from it.
  persistedFloor_ = strategyIndex_;

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
  strikesIdle_.store(0, std::memory_order_relaxed);
  strikesLightSleep_.store(0, std::memory_order_relaxed);
}

void EnduranceGovernor::setProfile(endurance::Profile p) {
  if (disabled_) return;
  // The profile can be changed from the web-server task, so the byte is handed
  // over rather than re-resolved here: the ladder is only ever mutated on the
  // main task, under the state mutex.
  pendingProfile_.store(static_cast<uint32_t>(p) + 1u, std::memory_order_release);
}

void EnduranceGovernor::persistFloor(const uint8_t floorIndex) {
  // Carry whatever strike bits are STILL live: these are the residual, i.e. the
  // strikes reported after the caller consumed its batch. reportInstability()
  // persists its own byte when a strike arrives, so a floor write that
  // unconditionally zeroed the strike half would undo a strike reported while
  // this promotion was being computed. Callers must therefore consume their
  // batch before calling this.
  const uint8_t byte = encodeStateByte(strikes(), floorIndex);
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, false)) {
    prefs.putUChar(kNvsStateKey, byte);
    prefs.end();
  } else {
    LOG_ERR("PWR", "endurance.state floor write failed (NVS open)");
  }
}

void EnduranceGovernor::refreshFromSettings() {
  lockState();
  profile_ = endurance::clampProfile(static_cast<uint8_t>(profile_));
  const endurance::StrikeState current = strikes();
  // Escalate from the rung the device is ON, not from the profile base: strike
  // counters are boolean flags, so a base-derived rung would be recomputed
  // identically on every strike and the ladder could never climb past base+1.
  const uint8_t escalated = endurance::escalate(persistedFloor_, current.idleStrikes, current.lightSleepStrikes);
  const uint8_t base = endurance::kProfileBaseStrategy[static_cast<uint8_t>(profile_)];
  const uint8_t wanted = escalated > base ? escalated : base;
  if (wanted != strategyIndex_) {
    strategyIndex_ = wanted;
  }
  unlockState();
}

void EnduranceGovernor::applyStrategy() {
  endurance::Profile profile;
  endurance::Strategy strategy;
  int bootClock;
  lockState();
  profile = profile_;
  strategy = endurance::kStrategies[strategyIndex_];
  bootClock = bootClockMHz_;
  unlockState();

  const endurance::ProfileDefaults defaults = endurance::defaultsFor(strategy, profile);
  // Crossfire's ladder logs "render 80 MHz" because 80 MHz IS its boot clock.
  // XPoint boots at 240 MHz on the S3, so pinning renders down to 80 would be a
  // straight performance regression; the render clock is therefore floored at the
  // boot clock and never lowered.
  const int renderClock = defaults.renderClockMHz > bootClock ? defaults.renderClockMHz : bootClock;
  // Read the sticky demotion flag, publish the members and push the HAL target
  // in ONE locked section. Splitting them let a demoteToPollSlices() land between
  // the read and the publish, so a stale pollSlices = true could re-set the bit a
  // demotion had just cleared, restoring the cadence that drops touches with no
  // later applyStrategy to correct it. setGovernorTarget() is a plain atomic
  // store with no callback into the governor, so it is safe to hold the mutex
  // across it.
  lockState();
  const bool pollSlices = defaults.idlePollSlices && !pollSlicesDemoted_.load(std::memory_order_relaxed);
  renderClockMHz_ = renderClock;
  idleClockMHz_ = defaults.idleClockMHz;
  idlePollSlices_ = pollSlices;
  // The clock still moves only through HalPowerManager::setPowerSaving(); the
  // governor supplies the target, not the transition. Published as one snapshot:
  // two separate setters would let idle entry pair a new clock with the previous
  // rung's polling policy.
  powerManager.setGovernorTarget(defaults.idleClockMHz, pollSlices);
  unlockState();

  // "poll slices", not "light sleep": XPoint never enters light sleep, so logging
  // the Crossfire wording here would claim a sleep mode the device does not take.
  LOG_INF("PWR", "Profile %u: idle %d / active %d / render %u MHz, poll slices %s", static_cast<unsigned>(profile),
          defaults.idleClockMHz, bootClock, static_cast<unsigned>(renderClock), pollSlices ? "enabled" : "disabled");
}

void EnduranceGovernor::reportInstability(InstabilityReason reason) {
  switch (reason) {
    case InstabilityReason::ClockSwitchFailure:
      strikesIdle_.store(1, std::memory_order_relaxed);
      break;
    case InstabilityReason::Watchdog:
      strikesLightSleep_.store(1, std::memory_order_relaxed);
      break;
    case InstabilityReason::Crash:
      // A crash says nothing about WHICH dimension was at fault, so it strikes
      // both: the ladder has to move or the device keeps crashing at the same
      // clock forever.
      strikesIdle_.store(1, std::memory_order_relaxed);
      strikesLightSleep_.store(1, std::memory_order_relaxed);
      break;
    case InstabilityReason::None:
    default:
      return;
  }
  const endurance::StrikeState pending{strikesIdle_.load(std::memory_order_relaxed),
                                       strikesLightSleep_.load(std::memory_order_relaxed)};
  uint8_t index;
  lockState();
  index = strategyIndex_;
  unlockState();
  const uint8_t byte = encodeStateByte(pending, index);
  // Persist synchronously: the common reason to strike is that the device is
  // about to crash or reset, and the escalation task may never run again.
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, false)) {
    prefs.putUChar(kNvsStateKey, byte);
    prefs.end();
  } else {
    LOG_ERR("PWR", "endurance.state strike write failed (NVS open)");
  }
  strikesDirty_.store(true, std::memory_order_release);
  LOG_ERR("PWR", "Endurance strike recorded: %s (state 0x%02x)", reasonText(reason), static_cast<unsigned>(byte));
}

void EnduranceGovernor::escalationTaskEntry() {
  for (;;) {
    // exchange(), not load-then-clear: a strike recorded between the load and
    // the clear would otherwise be swallowed.
    if (strikesDirty_.exchange(false, std::memory_order_acquire)) {
      // Capture the batch this iteration acts on. A strike reported while the
      // promotion is being computed must survive it, otherwise the worker clears
      // a flag it never accounted for and that instability is lost.
      const uint8_t idleSeen = strikesIdle_.load(std::memory_order_relaxed);
      const uint8_t lightSeen = strikesLightSleep_.load(std::memory_order_relaxed);
      // Promote this session immediately rather than waiting for the next boot.
      refreshFromSettings();
      uint8_t index;
      lockState();
      index = strategyIndex_;
      persistedFloor_ = index;
      unlockState();
      // Consume only the captured batch BEFORE persisting, with compare-exchange
      // so a strike that arrived mid-promotion stays set for the next pass.
      uint8_t expectIdle = idleSeen;
      strikesIdle_.compare_exchange_strong(expectIdle, 0, std::memory_order_acq_rel);
      uint8_t expectLight = lightSeen;
      strikesLightSleep_.compare_exchange_strong(expectLight, 0, std::memory_order_acq_rel);
      // Persist the raised floor: otherwise the promotion lives only in RAM and
      // the next boot's loadAndMigrateStrikes() reads the pre-escalation ratchet,
      // putting the device back on the rung that was failing it. Ordering matters
      // exactly as much as the write itself: persistFloor() encodes the live
      // strike bits, so it must run after the batch has been consumed. Writing
      // the byte first would store the strikes this promotion already folded into
      // `index`, and the next boot would escalate() from them again and climb two
      // rungs past the settled level.
      persistFloor(index);
      applyStrategy();
      LOG_ERR("PWR", "Escalated to strategy %u after strike", static_cast<unsigned>(index));
    }
    // A heavy job still running must never be throttled by an idle tick from
    // another task; restore the clock here so the job's window is honoured even
    // if the main loop asked for idle in the meantime. Gated on the same hold
    // flag setPowerSaving() consults: restoring it here while the user turned
    // Heavy-Job Boost off would re-raise the clock anyway and make the setting
    // ineffective.
    if (heavyJobs_.load(std::memory_order_relaxed) > 0 && heavyJobHoldEnabled() && powerManager.isLowPowerActive()) {
      powerManager.setPowerSaving(false);
    }
    vTaskDelay(pdMS_TO_TICKS(kEscalationPeriodMs));
  }
}

const char* EnduranceGovernor::wakeVerdictText() const {
  switch (wakeVerdict_.load(std::memory_order_relaxed)) {
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
  if (disabled_) return;
  const unsigned long now = millis();
  const unsigned long deltaMs = lastTickMs_ == 0 ? 0 : now - lastTickMs_;
  lastTickMs_ = now;
  bootMs_ = static_cast<uint32_t>(now - bootStartMs_);

  // External power, NOT "charging": a full battery stops charging with the cable
  // still attached, and reporting a drain rate for a device running off USB is
  // exactly the wrong number. Only a board that can observe the input rail
  // answers this; elsewhere the flag stays false and the rate is simply absent.
  // Both the input-rail read and the gauge read are synchronous I2C/ADC traffic
  // on a bus shared with the GT911 touch controller, so neither may happen once
  // per loop() pass — see the throttle below.
  bool externalKnown = false;
  bool onUsb = false;
  const bool gaugeSupported = powerManager.getBatteryHealthState() != HalPowerManager::BatteryHealthState::UNSUPPORTED;

  // Battery sampling is throttled: the loop runs hundreds of times a second and
  // an ADC read per pass both advances the HAL's smoothing filter far faster
  // than it was designed for (a noisier status-bar reading) and lets +/-1% gauge
  // jitter restart the drain window often enough that the 10-minute gate may
  // never open.
  if (now - lastBatterySampleMs_ >= kBatterySampleIntervalMs) {
    lastBatterySampleMs_ = now;

    externalKnown = false;
    onUsb = powerManager.isExternalPowerPresent(&externalKnown);
    drain_.setOnUsbPower(externalKnown && onUsb);

    // Poll first, then validate. A read can itself be the one that notices the
    // gauge has gone bad, and checking health *before* the read would miss that
    // transition and also skip recovery once a local sample exists.
    const uint8_t reported =
        gaugeSupported ? static_cast<uint8_t>(powerManager.getBatteryPercentage()) : PowerDrainMonitor::kInvalid;
    const bool usable =
        gaugeSupported && powerManager.getBatteryHealthState() == HalPowerManager::BatteryHealthState::HEALTHY;

    if (!usable) {
      // The HAL retains the last known-good percentage once the gauge goes
      // stale, so a stale poll returns a FROZEN number. Feeding that in would
      // keep extending the drain window with fabricated constant charge and
      // report an artificially low — possibly zero — consumption rate. Drop the
      // partial window instead, so the rate stays unavailable until a fresh
      // ten-minute window completes on trustworthy samples.
      if (lastBatteryPct_ != PowerDrainMonitor::kInvalid) {
        drain_.resetWindow();
        lastBatteryPct_ = PowerDrainMonitor::kInvalid;
      }
      drain_.sample(PowerDrainMonitor::kInvalid, now);
    } else {
      // A gauge board reports 0% for "unknown" until its first successful read;
      // that must never be sampled as a real empty pack.
      const bool firstAndUnknown = lastBatteryPct_ == PowerDrainMonitor::kInvalid && reported == 0;
      if (firstAndUnknown) {
        drain_.sample(PowerDrainMonitor::kInvalid, now);
      } else {
        lastBatteryPct_ = reported;
        drain_.sample(reported, now);
      }
    }
  }

  // A profile change requested from another task is adopted here, on the main
  // task, so the ladder is only re-resolved under the state mutex.
  const uint32_t requestedProfile = pendingProfile_.exchange(0, std::memory_order_acquire);
  if (requestedProfile != 0) {
    const endurance::Profile wanted = endurance::clampProfile(static_cast<uint8_t>(requestedProfile - 1u));
    // main.cpp pushes the persisted profile every loop pass, so this path runs
    // constantly. Re-resolving and re-applying an unchanged profile would take
    // two mutex round-trips and re-log the ladder line on every pass.
    lockState();
    const bool changed = wanted != profile_;
    if (changed) profile_ = wanted;
    unlockState();
    if (changed) {
      refreshFromSettings();
      applyStrategy();
    }
  }

  // Idle vs nap accounting for the Full overlay. Which bucket a tick lands in is
  // decided by the clock state the governor itself asked for, not by a timer.
  if (deltaMs > 0) {
    if (powerManager.isLowPowerActive()) {
      napMs_ += static_cast<uint32_t>(deltaMs);
    } else {
      idleMs_ += static_cast<uint32_t>(deltaMs);
    }
  }

  WifiLeakGuard::poll(now, unownedRadioShutdownAllowed_.load(std::memory_order_relaxed));

  // Touch-INT verification, cached after the first verdict.
  if (wakeVerdict() == WakeVerdict::Unverified && BoardConfig::hasTouch() &&
      now - lastWakeProbeRunMs_ >= kWakeProbeIntervalMs) {
    lastWakeProbeRunMs_ = now;
    const int8_t irq = BoardConfig::ACTIVE.touch.irq;
    if (irq >= 0) {
      // Configure once: the probe is now throttled, and repeating gpio_config on
      // every pass is what the interval exists to stop.
      if (wakeProbePinConfigured_ != irq) {
        pinMode(irq, INPUT);
        wakeProbePinConfigured_ = irq;
      }
      const bool asserted =
          BoardConfig::ACTIVE.touch.irqActiveLow ? (digitalRead(irq) == LOW) : (digitalRead(irq) == HIGH);
      const bool touchSeen = gpio.wasTouchActivity();
      const bool windowOpen = lastWakeProbeMs_ != 0;
      const bool windowExpired = windowOpen && (now - lastWakeProbeMs_) > kWakeProbeSettleMs;

      // The streak means CONSECUTIVE asserted probes, so any probe that finds the
      // line quiet breaks it -- not just a gesture. Without this, two isolated
      // spurious assertions minutes apart, each individually explainable by the
      // input task's latency, would reach 2 and permanently demote the device.
      if (!asserted) ghostStreak_ = 0;

      if (touchSeen) {
        ghostStreak_ = 0;
        // The gesture opens the settle window; the line normally asserts within
        // it. Both halves are required for a verified verdict: a gesture alone
        // proves the touch path works, not that the INT line reports it.
        if (asserted) {
          wakeVerdict_.store(WakeVerdict::Verified, std::memory_order_relaxed);
          LOG_INF("PWR", "Touch INT verified as a light-sleep wake source");
        } else if (!windowOpen) {
          lastWakeProbeMs_ = now;
        }
      } else if (windowOpen && !windowExpired) {
        // Still inside the settle window: the assertion may yet arrive.
        if (asserted) {
          wakeVerdict_.store(WakeVerdict::Verified, std::memory_order_relaxed);
          LOG_INF("PWR", "Touch INT verified as a light-sleep wake source");
        }
      } else if (asserted && ++ghostStreak_ >= 2) {
        // INT asserted with no gesture behind it, seen on two consecutive probes.
        // The input manager runs a separate 10 ms task, so a single sample can
        // catch the line asserted just before the gesture is published; only a
        // persisting condition means the line really ghosts.
        demoteToPollSlices("Touch INT wakes without touches; using poll slices");
      } else if (windowExpired) {
        // A gesture was registered and the line never asserted within the window.
        // Without this transition a genuinely missed wake would sit Unverified
        // forever and the advertised fail-safe would never engage.
        demoteToPollSlices("Touch INT missed wakes; using poll slices");
      }
    }
  }
}

void EnduranceGovernor::demoteToPollSlices(const char* reason) {
  wakeVerdict_.store(WakeVerdict::DemotedToPoll, std::memory_order_relaxed);
  lockState();
  pollSlicesDemoted_.store(true, std::memory_order_relaxed);
  idlePollSlices_ = false;
  // Cleared inside the same lock applyStrategy() holds across its read+publish,
  // so the two publishes are serialised and neither can strand the other's value.
  powerManager.setIdlePollSlicesEnabled(false);
  unlockState();
  // A device that is already idle needs the cadence pushed explicitly. The
  // obvious setPowerSaving(false)/(true) pair cannot do it: the first call
  // refreshes lastNormalMs, so the second is rejected by the dwell guard and the
  // demotion would silently never take effect. Push the polling half only, which
  // touches no clock and so needs no dwell.
  powerManager.applyIdlePolling();
  LOG_ERR("PWR", "%s", reason);
}

void EnduranceGovernor::beginSleepWindow() {
  // Stage the reading for the boot that follows the sleep. No in-boot sleep
  // window is opened: every sleep path here ends in deep sleep, so the window
  // would never close before millis() restarts.
  _preSleepPct = drain_.lastPercent();
  _persistedNaps = naps_;
  _preSleepEpoch = trustedtime::trustedNow();
  bool known = false;
  _preSleepUsb = powerManager.isExternalPowerPresent(&known) ? 1 : (known ? 0 : 0xFF);
}

void EnduranceGovernor::endSleepWindow() {
  // Nothing to close in RAM: a nap is accounted at the next boot's begin(),
  // which is the only point where the sleep actually ended.
  (void)0;
}

void EnduranceGovernor::notePageTurn(const unsigned long totalMs) {
  if (pageRenders_ < 0xFFFFFFFFU) pageRenders_++;
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
