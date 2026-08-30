#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <InputManager.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_rtc_time.h>
#include <esp_sleep.h>
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
static constexpr uint8_t SLEEP_TRACE_VERSION = 5;
static RTC_DATA_ATTR uint8_t _sleepTraceVersion = 0;
static RTC_DATA_ATTR uint16_t _sleepEntryMv = 0;
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

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
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
  // Dev-only: snapshot the battery voltage before sleeping. Duration is NOT taken
  // here — it is derived at wake from the boot/wake timestamp (see logSleepBattery),
  // because esp_rtc_get_time_us() is re-based across deep sleep and gives bogus
  // durations. The X4 Pro CW2017 gauge (I2C 0x63) reports mV via BatteryMonitor.
  {
    const BatteryMonitor battery;
    const uint16_t mv = battery.readMillivolts();
    if (mv != 0) {
      _sleepEntryMv = mv;
      LOG_DBG("PWR", "sleep entry: %u mV / %u%%", mv, battery.readPercentage());
    }
  }
#endif
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
#if LOG_LEVEL >= 2
  // RTC slow memory survives firmware updates, so a record written by an older
  // build (different layout/meaning) must be discarded before we trust it.
  if (_sleepTraceVersion != SLEEP_TRACE_VERSION) {
    _sleepEntryMv = SLEEP_BATTERY_INVALID;
    _lastWakeS = 0;
    _sleepReason = 0;
    _sleepSeq = 0;
    _sleepTraceVersion = SLEEP_TRACE_VERSION;
    // A record from an older build (pre-signed fields, or any layout change) must
    // not leak into the status bar — clear the persisted result too.
    _lastSleepDrain = {};
  }
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
  // We used to derive slept time from esp_rtc_get_time_us() (RTC_SLOW_CLK counter)
  // taken before sleep and after wake. That counter is NOT monotonic across deep
  // sleep: esp_rtc_get_time_us() scales the raw ticks by a calibration factor kept
  // in s_rtc_timer_retain_mem, which the IDF docs (esp_clk.c) say can be INVALID
  // after deep sleep and gets memset to 0 on wake — re-basing the whole timer. The
  // "before" and "after" readings then live in different calibration eras, so their
  // difference collapses to a tiny bogus number and the rate explodes to ~-3000 mV/h.
  //
  // Fix: remember the monotonic-seconds timestamp of every boot/wake (_lastWakeS,
  // persisted in RTC slow memory). On a deep-sleep wake (ESP_RST_DEEPSLEEP) the SoC
  // was asleep for the ENTIRE interval since the previous wake, so the sleep duration
  // is simply now - _lastWakeS. For a cold boot or SW reset there is no prior sleep,
  // so we skip (no drain to report). millis() is a plain XTAL/APB tick counter that
  // starts at 0 each boot and is NOT re-based by deep sleep, so it is safe here.
  const uint32_t nowS = static_cast<uint32_t>(millis() / 1000ULL);
  uint32_t sleptS = 0;
  bool haveSlept = false;
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP && _lastWakeS != 0 && nowS > _lastWakeS) {
    sleptS = nowS - _lastWakeS;
    haveSlept = true;
  }
  // Update the wake timestamp for next time (always — boot, wake, or SW reset).
  _lastWakeS = nowS;

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
    // Cold boot / SW reset / corrupted timestamp: nothing to compare against.
    _sleepEntryMv = SLEEP_BATTERY_INVALID;
    return;
  }

  const uint64_t sleptUs = static_cast<uint64_t>(sleptS) * 1000000ULL;
  // Signed delta: + = gained charge (was on charger), - = lost (discharged).
  const int deltaMv = static_cast<int>(mvNow) - static_cast<int>(_sleepEntryMv);
  // Signed mV per hour over sleptUs microseconds. Guard against a 0 sleep.
  double mvPerHour = 0.0;
  if (sleptUs > 0) {
    mvPerHour = static_cast<double>(deltaMv) / (static_cast<double>(sleptUs) / 3.6e9);
  }
  LOG_DBG("PWR", "woke after %u s: %u mV now (was %u mV), %+d mV (%+.2f mV/h)", sleptS, mvNow, _sleepEntryMv, deltaMv,
          mvPerHour);

  // Convert the signed mV/h rate to a signed current (mA). A Li-Po's usable span
  // is ~1.2V; a full cell holds kX4ProBatteryMah mAh, so removing the whole span
  // in 1h = kX4ProBatteryMah mA. Scaling: I[mA] = dV/dt[mV/h] * capacity[mAh] /
  // span[mV]. + = gained charge (charging), - = discharging.
  const double milliamps = mvPerHour * kX4ProBatteryMah / kLipoSpanMv;

  // Persist so the UI can show it after wake (when the serial port is back up).
  _lastSleepDrain.valid = true;
  _lastSleepDrain.sleptSeconds = sleptS;
  _lastSleepDrain.deltaMv = deltaMv;
  _lastSleepDrain.mvPerHour = mvPerHour;
  _lastSleepDrain.milliamps = milliamps;
  _lastSleepDrain.wakeCause = static_cast<uint8_t>(wakeCause);
  _lastSleepDrain.resetReason = static_cast<uint8_t>(resetReason);

  // Dev-only: append one raw CSV row to the SD card so the cycle data survives
  // the dead-serial deep sleep and can be pulled for offline analysis. The full
  // row is written here at wake (SD is remounted by now); the sleep-entry side
  // only staged _sleepEntryMv + _sleepReason + bumped _sleepSeq. One append per
  // cycle => clean top-to-bottom dataset. Writes to /.crosspoint/sleep_trace.csv.
  {
    static constexpr char kSleepTracePath[] = "/.crosspoint/sleep_trace.csv";
    const bool spurious = (wakeCause != ESP_SLEEP_WAKEUP_EXT1);
    HalFile f = Storage.open(kSleepTracePath, O_RDWR | O_CREAT | O_APPEND);
    if (f) {
      if (f.size() == 0) {
        // Fresh file: write a header so the columns are self-documenting.
        f.println(F("seq,reason,entry_mv,wake_mv,delta_mv,slept_s,mv_per_h,mA,"
                    "wake_cause,reset_reason,spurious"));
      }
      // reason: 0=timeout 1=button 2=quick-resume; wake_cause/reset_reason are the
      // raw esp_sleep_wakeup_cause_t / esp_reset_reason_t enum values.
      char row[160];
      snprintf(row, sizeof(row),
               "%u,%u,%u,%u,%+d,%u,%.2f,%.2f,%d,%d,%d\n",
               _sleepSeq, _sleepReason, _sleepEntryMv, mvNow, deltaMv, sleptS,
               mvPerHour, milliamps, static_cast<int>(wakeCause),
               static_cast<int>(resetReason), spurious ? 1 : 0);
      f.print(row);
      _sleepSeq++;  // next cycle gets the next sequence number
    }
  }

  // Mark consumed so a double log in the same boot doesn't misreport.
  _sleepEntryMv = SLEEP_BATTERY_INVALID;
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
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
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
