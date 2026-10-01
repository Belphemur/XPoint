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
std::atomic<uint32_t> s_leaksStopped{0};
std::atomic<WifiLeakGuard::StopReason> s_lastStopReason{WifiLeakGuard::StopReason::ScopeExit};
}  // namespace

WifiLeakGuard::WifiLeakGuard() : WifiLeakGuard(DEFAULT_LEAK_TIMEOUT_MS) {}

WifiLeakGuard::WifiLeakGuard(const unsigned long leakTimeoutMs) : leakTimeoutMs_(leakTimeoutMs) {
  const unsigned long now = millis();
  if (s_sessions.fetch_add(1, std::memory_order_relaxed) == 0) {
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
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void WifiLeakGuard::keepAlive(const unsigned long nowMs) {
  if (s_sessions.load(std::memory_order_relaxed) > 0) {
    s_deadlineMs.store(nowMs + leakTimeoutMs_, std::memory_order_relaxed);
  }
}

bool WifiLeakGuard::poll(const unsigned long nowMs) {
  if (s_sessions.load(std::memory_order_relaxed) <= 0) return false;

  const unsigned long deadline = s_deadlineMs.load(std::memory_order_relaxed);
  if (deadline == 0 || nowMs < deadline) return false;

  // The deadline has passed with no keepAlive. Whatever still believes it owns
  // the radio is holding the CPU at max frequency for nothing — cut it.
  s_sessions.store(0, std::memory_order_relaxed);
  s_deadlineMs.store(0, std::memory_order_relaxed);
  s_lastStopReason.store(StopReason::LeakTimeout, std::memory_order_relaxed);
  const uint32_t count = s_leaksStopped.fetch_add(1, std::memory_order_relaxed) + 1;
  LOG_ERR("PWR", "Wi-Fi leak guard: stopping a radio left on past its deadline (leak #%u)",
          static_cast<unsigned>(count));
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return true;
}

bool WifiLeakGuard::sessionActive() { return s_sessions.load(std::memory_order_relaxed) > 0; }

uint32_t WifiLeakGuard::leaksStopped() { return s_leaksStopped.load(std::memory_order_relaxed); }

WifiLeakGuard::StopReason WifiLeakGuard::lastStopReason() { return s_lastStopReason.load(std::memory_order_relaxed); }
