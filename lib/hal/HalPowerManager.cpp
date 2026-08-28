#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_rtc_time.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>

#include "HalGPIO.h"

#if FREEINK_DEVICE_PAPERMONO
#include <M5Pm1.h>
#endif

HalPowerManager powerManager;  // Singleton instance

// Dev-only battery-drain tracing across deep sleep. RTC slow memory survives
// deep sleep, so we stash the mV reading and a truncated sleep-entry timestamp
// (seconds since a 2026 base) just before sleeping and compare them on wake.
// We store the truncated RTC seconds (uint32_t) rather than the raw 64-bit RTC
// epoch to keep the RTC_DATA footprint small; the delta we actually use cancels
// any epoch anyway, so the 2026 base is only there to keep the stored number
// small. 0 means "never slept" (cold boot) — skip then.
static RTC_DATA_ATTR uint16_t _sleepEntryMv = 0;
static RTC_DATA_ATTR uint32_t _sleepEntryS = 0;
static constexpr uint16_t SLEEP_BATTERY_INVALID = 0;

// Persisted result of the last sleep-drain computation, so the UI can display it
// after wake (when the serial port is back up). Stays a small struct in RTC slow
// memory.
static RTC_DATA_ATTR HalPowerManager::SleepDrain _lastSleepDrain{};

// 2026-01-01T00:00:00Z in microseconds, used only to truncate the RTC epoch so
// the stored sleep-entry timestamp stays a small uint32_t of seconds.
static constexpr uint64_t RTC_2026_BASE_US = 1767225600000000ULL;

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
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
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
  // Dev-only: snapshot the battery before sleeping so we can compute drain on
  // wake. The X4 Pro CW2017 gauge (I2C 0x63) reports mV via BatteryMonitor.
  {
    const BatteryMonitor battery;
    const uint16_t mv = battery.readMillivolts();
    if (mv != 0) {
      _sleepEntryMv = mv;
      // Truncate the RTC epoch to 2026 so the stored value stays a small uint32_t.
      const uint64_t rtcUs = esp_rtc_get_time_us();
      _sleepEntryS = static_cast<uint32_t>((rtcUs > RTC_2026_BASE_US) ? (rtcUs - RTC_2026_BASE_US) / 1000000ULL : 0);
      LOG_DBG("PWR", "sleep entry: %u mV / %u%%", mv, battery.readPercentage());
    }
  }
#endif
  freeink::PowerManager::deepSleepUntilPowerButton();
}

void HalPowerManager::logSleepBattery() const {
#if LOG_LEVEL >= 2
  if (_sleepEntryMv == SLEEP_BATTERY_INVALID) {
    // Cold boot (or first run): nothing to compare against.
    return;
  }
  const BatteryMonitor battery;
  const uint16_t mvNow = battery.readMillivolts();
  if (mvNow == 0) return;

  // RTC slow-clock counter keeps running through deep sleep. We truncated the
  // epoch to 2026 at both snapshot and wake, so the difference is the actual
  // seconds asleep and stays a small uint32_t.
  const uint64_t nowUs = esp_rtc_get_time_us();
  const uint32_t nowS = static_cast<uint32_t>((nowUs > RTC_2026_BASE_US) ? (nowUs - RTC_2026_BASE_US) / 1000000ULL : 0);
  const uint32_t sleptS = (nowS > _sleepEntryS) ? (nowS - _sleepEntryS) : 0;
  const uint64_t sleptUs = static_cast<uint64_t>(sleptS) * 1000000ULL;
  const int deltaMv = static_cast<int>(_sleepEntryMv) - static_cast<int>(mvNow);
  // mV per hour: delta over sleptUs microseconds. Guard against a 0 sleep.
  double mvPerHour = 0.0;
  if (sleptUs > 0) {
    mvPerHour = static_cast<double>(deltaMv) / (static_cast<double>(sleptUs) / 3.6e9);
  }
  LOG_DBG("PWR", "woke after %u s: %u mV now (was %u mV), %s%u mV, ~%.2f mV/h", sleptS, mvNow, _sleepEntryMv,
          deltaMv >= 0 ? "-" : "+", deltaMv >= 0 ? deltaMv : -deltaMv, mvPerHour);

  // Persist so the UI can show it after wake (when the serial port is back up).
  _lastSleepDrain.valid = true;
  _lastSleepDrain.sleptSeconds = sleptS;
  _lastSleepDrain.deltaMv = deltaMv;
  _lastSleepDrain.mvPerHour = mvPerHour;

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
