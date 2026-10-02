#include "WifiLeakGuard.h"

#include <Arduino.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <WiFi.h>

#include <atomic>

namespace {
// Session state is touched from whatever task owns the activity, so the
// refcount and deadline go through atomics rather than the power-manager mutex.
std::atomic<int> s_sessions{0};
std::atomic<unsigned long> s_deadlineMs{0};
// The span the current deadline was armed with. Re-arming must use the SESSION's
// configured timeout, not the default: a guard built as WifiLeakGuard(60000)
// would otherwise silently get a five-minute window back whenever it re-armed.
std::atomic<unsigned long> s_deadlineSpanMs{WifiLeakGuard::DEFAULT_LEAK_TIMEOUT_MS};
std::atomic<uint32_t> s_leaksStopped{0};
std::atomic<WifiLeakGuard::StopReason> s_lastStopReason{WifiLeakGuard::StopReason::ScopeExit};
// millis() of the last poll that saw the radio DOWN (or of the last forced
// shutdown). An unowned radio is only "leaked" once it has been up for longer
// than the timeout, which is what keeps a legitimately-running radio that simply
// has no RAII session around it from being cut off mid-transfer.
std::atomic<unsigned long> s_radioDownSinceMs{0};
bool s_haveRadioBaseline = false;

// True once `spanMs` has elapsed on the millis() timeline. Signed subtraction
// keeps this correct across the 49-day wrap, which a plain `now > deadline`
// comparison is not: a wrapped deadline fires immediately, and a late one is
// deferred by almost a full wrap period.
bool elapsed(unsigned long now, unsigned long since, unsigned long spanMs) {
  const long delta = static_cast<long>(now - since);
  return delta >= 0 && static_cast<unsigned long>(delta) >= spanMs;
}

void stopRadio() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}
}  // namespace

WifiLeakGuard::WifiLeakGuard() : WifiLeakGuard(DEFAULT_LEAK_TIMEOUT_MS) {}

WifiLeakGuard::WifiLeakGuard(const unsigned long leakTimeoutMs) : leakTimeoutMs_(leakTimeoutMs) {
  const unsigned long now = millis();
  if (s_sessions.fetch_add(1, std::memory_order_relaxed) == 0) {
    s_deadlineSpanMs.store(leakTimeoutMs_, std::memory_order_relaxed);
    s_deadlineMs.store(now + leakTimeoutMs_, std::memory_order_relaxed);
  }
}

WifiLeakGuard::~WifiLeakGuard() {
  if (s_sessions.fetch_sub(1, std::memory_order_relaxed) != 1) {
    // An outer scope still needs the radio; do not stop it here.
    return;
  }
  s_deadlineMs.store(0, std::memory_order_relaxed);
  s_lastStopReason.store(StopReason::ScopeExit, std::memory_order_relaxed);
  stopRadio();
}

void WifiLeakGuard::keepAlive(const unsigned long nowMs) {
  if (s_sessions.load(std::memory_order_relaxed) > 0) {
    s_deadlineSpanMs.store(leakTimeoutMs_, std::memory_order_relaxed);
    s_deadlineMs.store(nowMs + leakTimeoutMs_, std::memory_order_relaxed);
  }
}

bool WifiLeakGuard::poll(const unsigned long nowMs, const bool allowUnownedShutdown) {
  const bool radioUp = WiFi.getMode() != WIFI_MODE_NULL;

  if (!radioUp) {
    s_haveRadioBaseline = true;
    s_radioDownSinceMs.store(nowMs, std::memory_order_relaxed);
    if (s_sessions.load(std::memory_order_relaxed) <= 0) {
      s_deadlineMs.store(0, std::memory_order_relaxed);
    } else {
      // A live session may deliberately take the radio down and bring it back
      // (Wi-Fi off between transfers). Re-arm from the moment the radio was
      // observed down rather than leaving the old deadline in place: a session
      // that spent longer than its window with the radio down would otherwise
      // be force-stopped the instant it came back up.
      s_deadlineMs.store(nowMs + s_deadlineSpanMs.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return false;
  }

  if (s_sessions.load(std::memory_order_relaxed) > 0) {
    const unsigned long deadline = s_deadlineMs.load(std::memory_order_relaxed);
    if (deadline == 0) {
      // A session is live but the deadline is disarmed — which happens whenever
      // the radio was observed DOWN while a guard already existed (the
      // radio-down branch clears it). Zero means "not armed", NOT "expired", so
      // stopping here would kill a session the moment it brought the radio back
      // up. Re-arm from now instead.
      s_deadlineMs.store(nowMs + s_deadlineSpanMs.load(std::memory_order_relaxed), std::memory_order_relaxed);
      return false;
    }
    // Still inside the window? A negative signed delta means nowMs < deadline,
    // and the comparison stays correct across the millis() wrap.
    if (static_cast<long>(nowMs - deadline) < 0) {
      return false;
    }
    // Past the deadline. NOTE: the session refcount is deliberately NOT zeroed.
    // Live WifiLeakGuard objects still exist, and their destructors would then
    // decrement past zero, after which no future guard would ever arm a
    // deadline or stop the radio — the guard would fail permanently, which is
    // the leak it exists to prevent.
    s_lastStopReason.store(StopReason::LeakTimeout, std::memory_order_relaxed);
    const uint32_t count = s_leaksStopped.fetch_add(1, std::memory_order_relaxed) + 1;
    LOG_ERR("PWR", "Wi-Fi leak guard: session %u past deadline, stopping radio (leak #%u)",
            static_cast<unsigned>(s_sessions.load(std::memory_order_relaxed)), static_cast<unsigned>(count));
    stopRadio();
    s_haveRadioBaseline = true;
    s_radioDownSinceMs.store(nowMs, std::memory_order_relaxed);
    return true;
  }

  // No session owns the radio. Only the caller knows whether the app currently
  // expects the radio to be up, so the no-session detector runs solely when it
  // says nothing owns it (the home screen).
  if (!allowUnownedShutdown) {
    s_haveRadioBaseline = false;
    return false;
  }

  // Baseline first so a device that boots with the radio already up is given a
  // full timeout before being judged a leak.
  if (!s_haveRadioBaseline) {
    s_haveRadioBaseline = true;
    s_radioDownSinceMs.store(nowMs, std::memory_order_relaxed);
    return false;
  }
  if (!elapsed(nowMs, s_radioDownSinceMs.load(std::memory_order_relaxed), DEFAULT_LEAK_TIMEOUT_MS)) {
    return false;
  }

  // Up, unowned, and well past any legitimate use: this is the "something left
  // it on by mistake" case the release note describes.
  s_lastStopReason.store(StopReason::NoSession, std::memory_order_relaxed);
  const uint32_t count = s_leaksStopped.fetch_add(1, std::memory_order_relaxed) + 1;
  LOG_ERR("PWR", "Wi-Fi leak guard: radio left on with no session, stopping it (leak #%u)",
          static_cast<unsigned>(count));
  stopRadio();
  s_radioDownSinceMs.store(nowMs, std::memory_order_relaxed);
  return true;
}

bool WifiLeakGuard::sessionActive() { return s_sessions.load(std::memory_order_relaxed) > 0; }

uint32_t WifiLeakGuard::leaksStopped() { return s_leaksStopped.load(std::memory_order_relaxed); }

WifiLeakGuard::StopReason WifiLeakGuard::lastStopReason() { return s_lastStopReason.load(std::memory_order_relaxed); }
