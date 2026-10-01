#include "PowerDrainMonitor.h"

#include <cstdio>

// %/h = (before - after) * 3600 / elapsedSeconds, carried as %/h * 1000.
//
// 64-bit throughout: dPct(<=100) * 3600 * 1000 * 1000 (ms denominator) reaches
// 3.6e11, far past a 32-bit accumulator, and an intermediate wrap would show a
// plausible-looking but wrong rate.
uint32_t PowerDrainMonitor::percentPerHourMilli(const uint8_t fromPct, const uint8_t toPct,
                                                const unsigned long elapsedMs) {
  if (elapsedMs == 0 || fromPct <= toPct) return 0;
  const uint64_t drained = static_cast<uint64_t>(fromPct) - toPct;
  const uint64_t scaled = drained * 3600ULL * 1000ULL * 1000ULL / elapsedMs;
  return scaled > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<uint32_t>(scaled);
}

// avg_mA = %/h * assumed_capacity_mAh / 100, carried as mA * 1000.
// milliPctPerHour already carries the *1000, so the capacity divides by 100
// once (not 100*1000).
uint32_t PowerDrainMonitor::avgMilliAmpMilli(const uint32_t milliPctPerHour) {
  const uint64_t scaled = static_cast<uint64_t>(milliPctPerHour) * ASSUMED_CAPACITY_MAH / 100ULL;
  return scaled > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<uint32_t>(scaled);
}

// runtime_left = remaining% * capacity / avg_mA, in minutes.
// The mA arrives scaled by 1000, so the division carries a *1000 back out.
unsigned long PowerDrainMonitor::runtimeLeftMinutes(const uint8_t remainingPct, const uint32_t milliAmpMilli) {
  if (milliAmpMilli == 0) return 0;  // no usable rate (see design doc §3.2 gate)
  // The percentage is a FRACTION of the pack, so it divides by 100; the mA
  // arrives scaled by 1000, so the division carries that *1000 back out.
  // Dropping the /100 reports 50% of a 1100 mAh pack at 110 mA as 500 hours
  // instead of 5.
  const uint64_t numerator = static_cast<uint64_t>(remainingPct) * ASSUMED_CAPACITY_MAH * 1000ULL * 60ULL / 100ULL;
  return static_cast<unsigned long>(numerator / milliAmpMilli);
}

const char* PowerDrainMonitor::formatDuration(char* out, const size_t outLen, const unsigned long minutes) {
  if (out == nullptr || outLen == 0) return out;
  if (minutes == 0) {
    std::snprintf(out, outLen, "<1m");
    return out;
  }
  if (minutes < 60) {
    std::snprintf(out, outLen, "%lum", minutes);
  } else if (minutes < 24UL * 60UL) {
    std::snprintf(out, outLen, "%luh %lum", minutes / 60, minutes % 60);
  } else {
    std::snprintf(out, outLen, "%lud %luh", minutes / (24 * 60), (minutes / 60) % 24);
  }
  return out;
}

bool PowerDrainMonitor::sample(const uint8_t percent, const unsigned long nowMs) {
  if (percent == kInvalid) return false;

  if (lastPct_ != kInvalid && percent > lastPct_) {
    // The gauge went UP: the user plugged in, or a stale read recovered. A drain
    // window spanning that discontinuity would average two unrelated states, so
    // restart the window at the new reading instead of recording a sample.
    startPct_ = percent;
    startMs_ = nowMs;
    samples_ = 0;
    lastPct_ = percent;
    refreshEstimate(nowMs);
    return false;
  }

  if (startPct_ == kInvalid) {
    startPct_ = percent;
    startMs_ = nowMs;
    samples_ = 0;
  }
  lastPct_ = percent;
  if (samples_ < 0xFF) samples_++;
  refreshEstimate(nowMs);
  return true;
}

void PowerDrainMonitor::reset() {
  resetWindow();
  lastPct_ = kInvalid;
  lastSleep_ = SleepWindow{};
  estimate_ = Estimate{};
  sleepOpen_ = false;
  sleepStartPct_ = kInvalid;
}

void PowerDrainMonitor::refreshEstimate(const unsigned long nowMs) {
  Estimate e{};
  e.onUsbPower = estimate_.onUsbPower;
  e.samples = samples_;

  if (startPct_ == kInvalid || lastPct_ == kInvalid || samples_ < 2) {
    estimate_ = e;
    return;
  }

  e.windowMs = nowMs - startMs_;
  // The 10-minute gate: until the window is long enough the overlay shows
  // "measuring (N/10 min)" rather than a number that is mostly page-turn noise.
  if (e.windowMs < MEASUREMENT_WINDOW_MS) {
    estimate_ = e;
    return;
  }

  e.milliPctPerHour = percentPerHourMilli(startPct_, lastPct_, e.windowMs);
  e.milliAmp = avgMilliAmpMilli(e.milliPctPerHour);
  e.measured = true;
  estimate_ = e;
}

void PowerDrainMonitor::beginSleep(const uint8_t percent, const unsigned long nowMs, const bool onUsbPower) {
  if (onUsbPower) {
    // A charge cycle makes any pre-sleep percentage meaningless to rate against
    // the post-wake one, so close out without recording a window.
    sleepOpen_ = false;
    sleepStartPct_ = kInvalid;
    lastSleep_ = SleepWindow{};
    resetWindow();
    return;
  }
  sleepOpen_ = true;
  sleepStartMs_ = nowMs;
  sleepStartPct_ = percent;
}

void PowerDrainMonitor::endSleep(const uint8_t percent, const unsigned long nowMs, const bool onUsbPower) {
  if (!sleepOpen_) return;
  sleepOpen_ = false;

  SleepWindow w{};
  w.durationMs = nowMs - sleepStartMs_;
  w.startPct = sleepStartPct_;
  w.endPct = percent;
  // "too short to rate" is the sub-MIN_SLEEP_WINDOW_MS case: too few gauge
  // samples to divide by without the rate swinging wildly.
  w.rateable = w.durationMs >= MIN_SLEEP_WINDOW_MS;
  // USB appearing during the sleep charges the pack, so the pre-sleep
  // percentage cannot be rated against the post-wake one.
  w.valid = !onUsbPower && (sleepStartPct_ != kInvalid && percent != kInvalid && percent <= sleepStartPct_);
  if (w.valid && w.rateable) {
    w.milliPctPerHour = percentPerHourMilli(sleepStartPct_, percent, w.durationMs);
  }
  lastSleep_ = w;
  sleepStartPct_ = kInvalid;
}
