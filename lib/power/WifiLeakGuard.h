#pragma once

#include <cstdint>

// Radio lifetime accounting.
//
// HalPowerManager::setPowerSaving() refuses to drop the clock while
// WiFi.getMode() != WIFI_MODE_NULL — a started radio holds APB at max frequency
// for its whole life. That makes a single missed teardown a permanent battery
// tax with no symptom, which is exactly what Crossfire's release note calls
// "the Wi-Fi leak guard: Wi-Fi is switched off if something leaves it on by
// mistake".
//
// A session is RAII so a normal return path cannot leak one, and a watchdog
// deadline catches the paths that do: a long-running activity that dies without
// unwinding (task abort, activity deleted mid-network-op) or a code path that
// simply forgets WiFi.mode(WIFI_OFF).
//
// Design source: docs/design/2026-10-01-endurance-governor-power-stats.md §3.4.

class WifiLeakGuard {
 public:
  // A session left open longer than this has its radio force-stopped. Generous
  // enough that a legitimate long sync/OTA never trips it.
  static constexpr unsigned long DEFAULT_LEAK_TIMEOUT_MS = 5UL * 60UL * 1000UL;

  // RAII radio session. Starts the STA interface and arms the leak deadline;
  // stops the radio and disarms it on destruction. Nested sessions are counted,
  // so an inner scope cannot stop a radio an outer scope still needs.
  WifiLeakGuard();
  explicit WifiLeakGuard(unsigned long leakTimeoutMs);
  ~WifiLeakGuard();
  WifiLeakGuard(const WifiLeakGuard&) = delete;
  WifiLeakGuard& operator=(const WifiLeakGuard&) = delete;
  WifiLeakGuard(WifiLeakGuard&&) = delete;
  WifiLeakGuard& operator=(WifiLeakGuard&&) = delete;

  // Extend the deadline. A server loop that is actively making progress calls
  // this so a legitimately long session is never mistaken for a leak.
  void keepAlive(unsigned long nowMs);

  // Main-loop tick (called from EnduranceGovernor::tick()). Force-stops the
  // radio when an armed session has run past its deadline. Returns true when a
  // leak was detected and shut down on this call.
  static bool poll(unsigned long nowMs);

  // True while at least one RAII session is open.
  static bool sessionActive();
  // Number of times poll() has force-stopped a leaked radio this boot.
  static uint32_t leaksStopped();

  // Test/log seam: the reason a session ended, so a shutdown can be logged.
  enum class StopReason : uint8_t { ScopeExit = 0, LeakTimeout = 1 };
  static StopReason lastStopReason();

 private:
  unsigned long leakTimeoutMs_ = DEFAULT_LEAK_TIMEOUT_MS;
};
