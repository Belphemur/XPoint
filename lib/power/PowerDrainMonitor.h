#pragma once

#include <cstddef>
#include <cstdint>

// Drain-rate estimation behind the Power Stats overlay.
//
// Pure arithmetic + a sampling window, no Arduino/ESP-IDF dependencies, so the
// formulas are host-testable without stubs. The governor feeds it battery
// samples; the overlay reads the formatted strings.
//
// Design source: docs/design/2026-10-01-endurance-governor-power-stats.md §3.2.
//
// All rates are carried as scaled integers (milli-units) rather than float:
// embedded newlib builds do not enable float printf, so a %f format specifier
// is not available to render them, and integer math keeps the display exactly
// reproducible across both build targets.

class PowerDrainMonitor {
 public:
  // Crossfire's assumed pack capacity. This is the constant baked into the
  // Crossfire binary (its drain composer multiplies by 1100.0) — it is an
  // ASSUMPTION carried over from that port, NOT a datasheet value for the X4 Pro
  // pack. Every mA and runtime-left figure the overlay shows is only as
  // trustworthy as this number.
  static constexpr unsigned int ASSUMED_CAPACITY_MAH = 1100;

  // Crossfire withholds a drain figure until a full 10 minutes of samples exist
  // (its overlay reads "Drain: measuring (N/10 min)"). An instantaneous reading
  // on an e-ink reader is meaningless — draw swings between ~5 mA (idle) and
  // ~30 mA (page turn), so a short window mostly measures page-turn noise.
  static constexpr unsigned long MEASUREMENT_WINDOW_MS = 10UL * 60UL * 1000UL;
  static constexpr unsigned long WINDOW_MINUTES = 10;

  // Below this duration a per-sleep window has too few gauge samples to rate.
  // The overlay shows "too short to rate" rather than dividing by ~0.
  static constexpr unsigned long MIN_SLEEP_WINDOW_MS = 60UL * 1000UL;

  // A battery that gained charge is not a drain sample (the user plugged in).
  static constexpr uint8_t kInvalid = 0xFF;

  struct Estimate {
    bool measured = false;           // past MEASUREMENT_WINDOW_MS with usable samples
    unsigned long windowMs = 0;      // length of the measurement window
    uint8_t samples = 0;             // gauge samples folded into the window
    uint32_t milliPctPerHour = 0;    // %/h * 1000
    uint32_t milliAmp = 0;           // mA * 1000
    bool onUsbPower = false;         // charging: rate is meaningless
  };

  struct SleepWindow {
    bool valid = false;              // had a usable start/end sample pair
    unsigned long durationMs = 0;
    uint8_t startPct = 0;
    uint8_t endPct = 0;
    bool rateable = false;           // duration >= MIN_SLEEP_WINDOW_MS
    uint32_t milliPctPerHour = 0;    // valid + rateable only
  };

  // Feed one gauge sample. Non-monotonic readings (charge, or a gauge that went
  // stale and recovered) are ignored rather than recorded as a negative drain.
  // Returns true when the sample was accepted into the window.
  bool sample(uint8_t percent, unsigned long nowMs);

  // Drop the measurement window (USB attach, sleep entry). The next sample
  // restarts the 10-minute gate rather than reporting across the discontinuity.
  void reset();

  // Discard the window but keep the retained percentage, used when returning
  // from deep sleep where millis() restarts near zero, and on any state change
  // that makes the old window meaningless (USB attach). The cleared rate matters
  // as much as the cleared samples: the overlay reads estimate() immediately
  // after the change, so leaving `measured` set would show a rate that describes
  // a window which no longer exists.
  void resetWindow() {
    startMs_ = 0;
    startPct_ = kInvalid;
    samples_ = 0;
    estimate_.measured = false;
    estimate_.windowMs = 0;
    estimate_.samples = 0;
    estimate_.milliPctPerHour = 0;
    estimate_.milliAmp = 0;
  }

  uint8_t lastPercent() const { return lastPct_; }
  const Estimate& estimate() const { return estimate_; }
  const SleepWindow& lastSleep() const { return lastSleep_; }

  void setOnUsbPower(bool onUsb) {
    if (onUsb == estimate_.onUsbPower) return;
    estimate_.onUsbPower = onUsb;
    resetWindow();
  }

  // Open a per-sleep window. onUsbPower closes any open window as invalid
  // rather than letting the pre-sleep percentage rate against post-wake.
  void beginSleep(uint8_t percent, unsigned long nowMs, bool onUsbPower);
  // Close the per-sleep window opened by beginSleep().
  void endSleep(uint8_t percent, unsigned long nowMs, bool onUsbPower);

  // ---- pure helpers, exposed for the overlay and for host tests ----

  // %/h = delta% * 3600 / deltaSeconds. Returns %/h scaled by 1000.
  static uint32_t percentPerHourMilli(uint8_t fromPct, uint8_t toPct, unsigned long elapsedMs);

  // avg_mA = %/h * ASSUMED_CAPACITY_MAH / 100. Takes %/h scaled by 1000 and
  // returns mA scaled by 1000.
  static uint32_t avgMilliAmpMilli(uint32_t milliPctPerHour);

  // runtime_left = remaining% * ASSUMED_CAPACITY_MAH / avg_mA, in minutes.
  // Returns 0 when there is no usable rate (unknown or zero drain).
  static unsigned long runtimeLeftMinutes(uint8_t remainingPct, uint32_t milliAmpMilli);

  // Humanised duration into a caller buffer: "3d 4h", "12h 30m", "45m", "<1m".
  // Never overflows; always NUL-terminates. Returns the buffer for chaining.
  static const char* formatDuration(char* out, size_t outLen, unsigned long minutes);

 private:
  void refreshEstimate(unsigned long nowMs);

  unsigned long startMs_ = 0;
  uint8_t startPct_ = kInvalid;
  uint8_t lastPct_ = kInvalid;
  uint8_t samples_ = 0;
  Estimate estimate_{};
  SleepWindow lastSleep_{};

  // Per-sleep window state.
  bool sleepOpen_ = false;
  unsigned long sleepStartMs_ = 0;
  uint8_t sleepStartPct_ = kInvalid;
};
