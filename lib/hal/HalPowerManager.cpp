#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <InputManager.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_rtc_time.h>
#include <esp_sleep.h>
#include <soc/rtc.h>
#include <soc/soc_caps.h>

#include <cassert>

#include "HalFrontlight.h"
#include "HalGPIO.h"
#include "HalStorage.h"

// X4 Pro cell capacity (mAh) used to convert the mV/h sleep-drain rate into a
// signed current (mA) for the status-bar readout. Board-specific; override per
// build with -DX4PRO_BATTERY_MAH=<n>. Defaults to 1100 (typical X4 Pro Li-Po).
#ifndef X4PRO_BATTERY_MAH
#define X4PRO_BATTERY_MAH 1100
#endif
static constexpr double kX4ProBatteryMah = static_cast<double>(X4PRO_BATTERY_MAH);
// Li-Po usable voltage span (full ~4.2V -> empty ~3.0V). Used to map a voltage
// rate to a current: I[mA] = dV/dt[mV/h] / (span[mV] / capacity[mAh]).
static constexpr double kLipoSpanMv = 1200.0;

#if FREEINK_DEVICE_PAPERMONO
#include <M5Pm1.h>
#endif

HalPowerManager powerManager;  // Singleton instance

// Dev-only battery-drain tracing across deep sleep. RTC slow memory survives
// deep sleep, so we stash the mV reading and the raw RTC seconds
// (esp_rtc_get_time_us()/1e6 — an *elapsed* counter, NOT the Unix epoch) just
// before sleeping and compare them on wake. Storing the raw 64-bit counter is
// unnecessary: we only ever subtract the two snapshots, so a uint32_t of seconds
// is plenty and far smaller in RTC slow memory. A 0 stored timestamp means
// "never slept" (cold boot) — skip the computation then.
//
// Bump SLEEP_TRACE_VERSION whenever the layout/meaning of the RTC_DATA block
// below changes. RTC slow memory is NOT cleared by a firmware update, so a stale
// record from an older build would otherwise be misread after an OTA.
static constexpr uint8_t SLEEP_TRACE_VERSION = 6;
static RTC_DATA_ATTR uint8_t _sleepTraceVersion = 0;
static RTC_DATA_ATTR uint16_t _sleepEntryMv = 0;
// Raw RTC_SLOW_CLK tick count (rtc_time_get()) captured just before sleep. The
// counter is monotonic across deep sleep (only its calibration factor is re-based,
// which we don't use — we convert ticks with the live slow-clock frequency at wake),
// so the delta to the wake reading is the true sleep duration in ticks.
static RTC_DATA_ATTR uint64_t _sleepEntryTicks = 0;
static RTC_DATA_ATTR uint32_t _lastWakeS = 0;  // monotonic-seconds timestamp of the most recent boot/wake
// Dev-only SD trace: which trigger put the device to sleep, and a monotonic
// cycle counter so the CSV on the SD card reads top-to-bottom in order.
// reason: 0 = auto-timeout, 1 = power-button (short-click SLEEP / hold), 2 = quick-resume.
static RTC_DATA_ATTR uint8_t _sleepReason = 0;
static RTC_DATA_ATTR uint32_t _sleepSeq = 0;
static constexpr uint16_t SLEEP_BATTERY_INVALID = 0;

// Persisted result of the last sleep-drain computation, so the UI can display it
// after wake (when the serial port is back up). Stays a small struct in RTC slow
// memory.
static RTC_DATA_ATTR HalPowerManager::SleepDrain _lastSleepDrain{};

// Battery-gauge health tracking, persisted in RTC slow memory: the last
// known-good percentage and read state survive deep sleep, so a gauge that
// starts failing across a sleep cycle reports the pre-sleep good value instead
// of a frozen or 0% cache. Reset when SLEEP_TRACE_VERSION changes (RTC slow
// memory is NOT cleared by a firmware update — see logSleepBattery()).
static RTC_DATA_ATTR uint16_t _batteryHealthLastKnownPct = 0;
static RTC_DATA_ATTR uint32_t _batteryHealthLastValidMs = 0;  // millis() of last successful gauge read
static RTC_DATA_ATTR uint8_t _batteryHealthFails = 0;         // consecutive failed gauge reads
static RTC_DATA_ATTR bool _batteryHealthKnownGood = false;    // a real gauge sample has ever succeeded
static RTC_DATA_ATTR uint8_t _batteryHealthState = 0;         // HalPowerManager::BatteryHealthState
// Gauge counts as STALE after this many consecutive failed reads...
static constexpr uint8_t BATTERY_HEALTH_MAX_FAILS = 3;
// ...or when no successful read happened for this long (ms).
static constexpr unsigned long BATTERY_HEALTH_STALE_MS = 60000;

// GPIO13 controls the X4 battery latch and the X3 SD power rail on the C3
// Xteink boards. Other boards use it for unrelated signals, including the
// X4 Pro display chip select.
static constexpr gpio_num_t XTEINK_C3_GPIO13 = GPIO_NUM_13;

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
  logSleepBattery();
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    InputManager::setLowPowerPolling(true);
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    InputManager::setLowPowerPolling(false);
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio, const uint64_t autoPowerOffTimerUs) {
#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice()) {
    // GPIO13 gates the battery MOSFET on both Xteink C3 boards; driving it low
    // is the battery power-off (the SDK wake source still handles USB power).
    // Release any surviving pad hold first: hold_en survives deep sleep via
    // the SDK's deepSleep() (esp_sleep_config_gpio_isolate +
    // gpio_deep_sleep_hold_en), and a held pad silently ignores the drive.
    gpio_hold_dis(XTEINK_C3_GPIO13);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_set_level(XTEINK_C3_GPIO13, 0);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }
#endif

  // Hold every configured power-latch pin HIGH through deep sleep. These are
  // keep-alive enables (the X4 Pro's master peripheral rail on GPIO1, the
  // Sticky's PWR_HOLD/PWR_LOCK): deepSleep() isolates all pads
  // (esp_sleep_config_gpio_isolate), so a latch without an armed hold loses its
  // output driver and floats — on the X4 Pro the latch drops as soon as
  // external power leaves (serial/pogo adapter unplugged), and the next power-
  // button press cold-boots instead of fast-waking. holdPowerRails() asserted
  // the latches at boot but arms no sleep hold; arm it here instead. Skips
  // XTEINK_C3_GPIO13: it IS power.latch0 on the C3 Xteink boards, where the
  // block above drives it LOW on purpose (battery power-off).
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || static_cast<gpio_num_t>(pin) == XTEINK_C3_GPIO13) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // Release any surviving pad hold first: a held pad silently ignores the
    // drive below (same trap as the GPIO13 block above).
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    gpio_hold_en(g);
  }

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

  // Park the frontlight pads so they don't leak current through deep sleep.
  // On the X4 Pro the master rail is held up (PR #3215 keeps power.latch0 / GPIO1
  // HIGH for fast-wake) and the frontlight LEDs use LEDC_SLEEP_MODE_KEEP_ALIVE,
  // so without this the frontlight driver keeps drawing quiescent current. park()
  // drives GPIO8/9 LOW and holds them, and releases the LEDC KEEP_ALIVE clock;
  // releaseOnWake() (called at boot in HalFrontlight::begin) undoes the hold so
  // the LEDC channels re-attach cleanly. Guarded by FREEINK_FRONTLIGHT_LS so it is
  // a no-op on boards without a frontlight (e.g. papermono).
#if FREEINK_FRONTLIGHT_LS
  Frontlight.park();
#endif

#if FREEINK_DEVICE_PAPERMONO
  // Its power button is behind the M5PM1 PMIC rather than an ESP GPIO, so
  // normal GPIO deep sleep would have no wake source. Ask the PMIC to shut the
  // device down; a button click then restarts it through a cold boot.
  if (freeink::m5pm1::requestShutdown()) {
    delay(1000);  // allow the PMIC firmware time to drop power
  }
#endif

  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
#if LOG_LEVEL >= 2
  // Dev-only: snapshot the battery voltage AND the raw RTC tick count before
  // sleeping. Duration is derived at wake from rtc_time_get() (monotonic across
  // deep sleep) — see logSleepBattery. The X4 Pro CW2017 gauge (I2C 0x63) reports
  // mV via BatteryMonitor.
  {
    const BatteryMonitor battery;
    const uint16_t mv = battery.readMillivolts();
    if (mv != 0) {
      _sleepEntryMv = mv;
      _sleepEntryTicks = rtc_time_get();
      LOG_DBG("PWR", "sleep entry: %u mV / %u%%", mv, battery.readPercentage());
    }
  }
#endif
  // Auto power off: arm the dwell timer so the device wakes (and shuts down)
  // if it is left in deep sleep this long.
  if (autoPowerOffTimerUs > 0) {
    esp_sleep_enable_timer_wakeup(autoPowerOffTimerUs);
  }
  freeink::PowerManager::deepSleepUntilPowerButton();
}

// Final software power-off for auto power off: drop the master rail latches
// LOW so everything but the wake logic loses power, then deep sleep until the
// power button is pressed (next press = normal cold boot). Assumes the
// shutdown screen has already been rendered.
[[noreturn]] void HalPowerManager::enterPowerOffSleep(HalGPIO& gpio) {
#ifdef ENABLE_SERIAL_LOG
  logSerial.end();
#endif

  // Cut the gated peripheral rails and park the frontlight pads while the
  // master rail is still up (same ordering as startDeepSleep()).
  freeink::PowerManager::powerDownRailsForSleep();
#if FREEINK_FRONTLIGHT_LS
  Frontlight.park();
#endif

  // Drive the keep-alive latches LOW and hold them through sleep. This is the
  // power-off itself, not a keep-alive: deepSleep() runs
  // esp_sleep_config_gpio_isolate(), which strips every pad WITHOUT an armed
  // hold — a merely-driven (unheld) latch loses its output driver and FLOATS,
  // and a floating rail enable on X4 Pro does not reliably stay LOW (it drops
  // only when external power leaves; on USB/pogo it can drift back up).
  // hold_en pins the OFF level through the isolation, so the master rail is
  // deterministically dead for the whole sleep.
  // Stock-parity note (ghidra_poweroff_report.md, FINAL CONCLUSION): stock's
  // power-off is a deep-sleep transaction — wake-config arm (16-byte 0x101
  // CRC32'd record, mask 0x0101010101010101), "SRCX" marker + reason byte to
  // RTC slow RAM (0x50000004/0x50000000), ownership quiesce poll, then commit
  // into the IDF sleep core. Stock holds NO rail (it lets the master rail
  // collapse), our LOW+hold is the deterministic variant of the same end
  // state. The portable stock delta for this sink: write the same RTC RAM
  // marker (magic 0x58435253 + reason byte) before sleeping, and read/clear
  // it at boot for shutdown-reason reporting. Skips XTEINK_C3_GPIO13 —
  // it IS power.latch0 on the C3 Xteink boards, where driving it low is the
  // battery power-off and must not be clobbered here.
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || static_cast<gpio_num_t>(pin) == XTEINK_C3_GPIO13) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // Release any surviving pad hold first: a held pad silently ignores the drive.
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    gpio_hold_en(g);
  }

  // The RTC timer that woke us has served its purpose; make sure it cannot
  // wake this power-off sleep.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

  freeink::PowerManager::deepSleepUntilPowerButton();
}

void HalPowerManager::setSleepReason(uint8_t reason) {
#if LOG_LEVEL >= 2
  _sleepReason = reason;
#else
  (void)reason;
#endif
}

void HalPowerManager::logSleepBattery() const {
  // RTC slow memory survives firmware updates, so a record written by an older
  // build (different layout/meaning) must be discarded before we trust it.
  // Runs on ALL builds: the battery-health fields below are production logic,
  // not dev-only tracing.
  if (_sleepTraceVersion != SLEEP_TRACE_VERSION) {
    _sleepEntryMv = SLEEP_BATTERY_INVALID;
    _lastWakeS = 0;
    _sleepReason = 0;
    _sleepSeq = 0;
    _sleepTraceVersion = SLEEP_TRACE_VERSION;
    // A record from an older build (pre-signed fields, or any layout change) must
    // not leak into the status bar — clear the persisted result too.
    _lastSleepDrain = {};
    _batteryHealthLastKnownPct = 0;
    _batteryHealthLastValidMs = 0;
    _batteryHealthFails = 0;
    _batteryHealthKnownGood = false;
    _batteryHealthState = 0;
  }
#if LOG_LEVEL >= 2
  if (_sleepEntryMv == SLEEP_BATTERY_INVALID) {
    // Cold boot (or first run, or a discarded stale record): nothing to compare against.
    return;
  }
  const BatteryMonitor battery;
  const uint16_t mvNow = battery.readMillivolts();
  if (mvNow == 0) {
    // Read failed: clear the entry so a later zero-reading sleep doesn't merge
    // this sleep with the next one and mis-report the drain.
    _sleepEntryMv = SLEEP_BATTERY_INVALID;
    return;
  }

  // Robust sleep-duration measurement.
  //
  // The sleep duration is the elapsed RTC_SLOW_CLK time between the pre-sleep
  // snapshot (rtc_time_get() captured in enterDeepSleep) and now. rtc_time_get()
  // returns RAW slow-clock ticks, which are monotonic across deep sleep — only the
  // IDF calibration factor is re-based on wake, and we never use it. We convert the
  // tick delta to seconds with the live slow-clock frequency. This is correct where
  // millis() is not: millis() restarts at ~0 on every deep-sleep reboot, so reading
  // it at wake only measures the short boot-to-begin() interval, not the sleep.
  uint64_t sleptTicks = 0;
  uint32_t sleptS = 0;
  bool haveSlept = false;
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP && _sleepEntryTicks != 0) {
    const uint64_t wakeTicks = rtc_time_get();
    if (wakeTicks > _sleepEntryTicks) {
      sleptTicks = wakeTicks - _sleepEntryTicks;
      const uint32_t slowHz = rtc_clk_slow_freq_get_hz();
      if (slowHz != 0) {
        sleptS = static_cast<uint32_t>(sleptTicks / slowHz);
        haveSlept = true;
        // (kept for any future cross-boot logic; no longer gates haveSlept)
        _lastWakeS = static_cast<uint32_t>(millis() / 1000ULL);
        // Re-base the health timestamp onto this boot's millis() so the stale
        // timeout math (now - _batteryHealthLastValidMs) can't wrap after a sleep.
        // The cached known-good percentage is preserved across the rebase.
        if (_batteryHealthKnownGood) {
          _batteryHealthLastValidMs = millis();
        }
      }
    }
  }

  // Diagnostics: how did this sleep segment end? A non-power-button wake cause
  // (or a non-deep-sleep reset) means the device was NOT asleep the whole
  // interval — it woke spuriously and the reported drain averages awake time in.
  // Log at INF (visible) so a spurious wake shows in the post-wake serial trace.
  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  const esp_reset_reason_t resetReason = esp_reset_reason();
  if (haveSlept) {
    LOG_INF("PWR", "sleep segment: woke via cause=%d reset=%d after %u s", static_cast<int>(wakeCause),
            static_cast<int>(resetReason), sleptS);
  }

  if (!haveSlept) {
    // Cold boot / SW reset: no sleep happened, nothing to compare against.
    _sleepEntryMv = SLEEP_BATTERY_INVALID;
    return;
  }

  const uint64_t sleptUs = static_cast<uint64_t>(sleptS) * 1000000ULL;
  // Signed delta: + = gained charge (was on charger), - = lost (discharged).
  const int deltaMv = static_cast<int>(mvNow) - static_cast<int>(_sleepEntryMv);
  // Signed mV per hour, extrapolated from the measured delta over the measured
  // sleep. This is ONLY meaningful for LONG sleeps: a few-mV gauge wobble over a
  // short sleep explodes when divided by the tiny duration (e.g. ~36 mV over 10 s
  // -> ~13,000 mA). The X4 Pro's CW2017 gauge has no current register, so the mV
  // delta is the only charge signal we have; we refuse to report a rate below a
  // sane sleep floor and mark it invalid instead of showing a bogus huge value.
  // deltaMv / sleptSeconds are still recorded for the CSV regardless.
  constexpr uint32_t kMinRateSleepS = 600;  // 10 min: below this, the mV/h rate is noise
  double mvPerHour = 0.0;
  bool rateValid = false;
  if (sleptS >= kMinRateSleepS) {
    mvPerHour = static_cast<double>(deltaMv) / (static_cast<double>(sleptS) / 3600.0);
    rateValid = true;
  }
  LOG_DBG("PWR", "woke after %u s: %u mV now (was %u mV), %+d mV (%+.2f mV/h, %s)", sleptS, mvNow, _sleepEntryMv,
          deltaMv, mvPerHour, rateValid ? "valid" : "below floor -> n/a");

  // Convert the signed mV/h rate to a signed current (mA). A Li-Po's usable span
  // is ~1.2V; a full cell holds kX4ProBatteryMah mAh, so removing the whole span
  // in 1h = kX4ProBatteryMah mA. Scaling: I[mA] = dV/dt[mV/h] * capacity[mAh] /
  // span[mV]. + = gained charge (charging), - = discharging. 0 / invalid when the
  // rate is below the floor (short sleep) — callers must check rateValid.
  const double milliamps = rateValid ? (mvPerHour * kX4ProBatteryMah / kLipoSpanMv) : 0.0;

  // Persist so the UI can show it after wake (when the serial port is back up).
  _lastSleepDrain.valid = true;
  _lastSleepDrain.entryMv = _sleepEntryMv;
  _lastSleepDrain.wakeMv = mvNow;
  _lastSleepDrain.sleptSeconds = sleptS;
  _lastSleepDrain.deltaMv = deltaMv;
  _lastSleepDrain.mvPerHour = mvPerHour;
  _lastSleepDrain.milliamps = milliamps;
  _lastSleepDrain.rateValid = rateValid;
  _lastSleepDrain.wakeCause = static_cast<uint8_t>(wakeCause);
  _lastSleepDrain.resetReason = static_cast<uint8_t>(resetReason);

  // Dev-only: store the computed result in RTC memory. The actual SD CSV append
  // is deferred to flushSleepTrace(), which the main loop calls AFTER Storage.begin()
  // (the card is not mounted yet here, so writing now would silently fail).
  _sleepEntryMv = SLEEP_BATTERY_INVALID;  // mark consumed so a double log doesn't misreport
#endif
}

void HalPowerManager::flushSleepTrace() const {
#if LOG_LEVEL >= 2
  if (!_lastSleepDrain.valid) {
    return;  // nothing to write (cold boot, failed read, or no real sleep)
  }
  // Assert the card is up: without a mount the open() would succeed on a null
  // volume and silently discard the row (the prior bug). Guard on the stored
  // result only — Storage.begin() must have run before this is called.
  static constexpr char kSleepTracePath[] = "/.crosspoint/sleep_trace.csv";
  const bool spurious = (_lastSleepDrain.wakeCause != ESP_SLEEP_WAKEUP_EXT1);
  HalFile f = Storage.open(kSleepTracePath, O_RDWR | O_CREAT | O_APPEND);
  if (f) {
    if (f.size() == 0) {
      // Fresh file: write a header so the columns are self-documenting.
      f.println(
          F("seq,reason,entry_mv,wake_mv,delta_mv,slept_s,mv_per_h,mA,"
            "wake_cause,reset_reason,spurious"));
    }
    // reason: 0=timeout 1=button 2=quick-resume; wake_cause/reset_reason are the
    // raw esp_sleep_wakeup_cause_t / esp_reset_reason_t enum values.
    char row[160];
    snprintf(row, sizeof(row), "%u,%u,%u,%u,%+d,%u,%.2f,%.2f,%d,%d,%d\n", _sleepSeq, _sleepReason,
             _lastSleepDrain.entryMv, _lastSleepDrain.wakeMv, _lastSleepDrain.deltaMv, _lastSleepDrain.sleptSeconds,
             _lastSleepDrain.mvPerHour, _lastSleepDrain.milliamps, static_cast<int>(_lastSleepDrain.wakeCause),
             static_cast<int>(_lastSleepDrain.resetReason), spurious ? 1 : 0);
    f.print(row);
    f.flush();    // commit before the next deep sleep tears the card down
    _sleepSeq++;  // next cycle gets the next sequence number
  } else {
    LOG_ERR("PWR", "sleep_trace.csv open failed (SD not mounted?)");
  }
#endif
}

HalPowerManager::SleepDrain HalPowerManager::getLastSleepDrain() const {
#if LOG_LEVEL >= 2
  return _lastSleepDrain;
#else
  return SleepDrain{};  // empty / invalid on non-debug builds
#endif
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    // _batteryHealthLastValidMs is persisted across deep sleep but millis() restarts
    // at ~0 every boot, so a stale (large) value from before sleep would make
    // (now - _batteryHealthLastValidMs) wrap and wrongly trip STALE. Re-base it onto
    // this boot's clock the first time we see it ahead of now — the known-good
    // percentage is preserved.
    if (_batteryHealthKnownGood && _batteryHealthLastValidMs != 0 && now < _batteryHealthLastValidMs) {
      _batteryHealthLastValidMs = now;
    }
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      // Fresh cache: health was decided on the last real poll, leave it untouched.
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      // Failed gauge read. Until a valid sample has ever succeeded we have no
      // known-good value to fall back to, so report 0 only as an explicit "unknown"
      // (never treat it as a real low battery). Once we have a sample, fall back to
      // the last known-good value so a broken gauge never shows a frozen/0% level.
      if (!_batteryHealthKnownGood) {
        _batteryHealthFails++;
        return 0;
      }
      _batteryHealthFails++;
      if (_batteryHealthFails >= BATTERY_HEALTH_MAX_FAILS ||
          now - _batteryHealthLastValidMs > BATTERY_HEALTH_STALE_MS) {
        if (_batteryHealthState != static_cast<uint8_t>(BatteryHealthState::STALE)) {
          _batteryHealthState = static_cast<uint8_t>(BatteryHealthState::STALE);
          LOG_DBG("PWR", "battery health STALE (fails=%u)", _batteryHealthFails);
        }
        return _batteryHealthLastKnownPct;
      }
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    _batteryHealthLastKnownPct = percent;
    _batteryHealthLastValidMs = now;
    _batteryHealthFails = 0;
    _batteryHealthKnownGood = true;
    _batteryHealthState = static_cast<uint8_t>(BatteryHealthState::HEALTHY);
    return _batteryCachedPercent;
  }

  // ADC boards have no gauge to fail: every read succeeds.
  _batteryHealthState = static_cast<uint8_t>(BatteryHealthState::HEALTHY);

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::BatteryHealthState HalPowerManager::getBatteryHealthState() const {
  return static_cast<BatteryHealthState>(_batteryHealthState);
}

bool HalPowerManager::isBatteryHealthStale() const {
  return _batteryHealthState == static_cast<uint8_t>(BatteryHealthState::STALE);
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
