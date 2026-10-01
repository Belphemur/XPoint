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
// Two detectors, because "left on by mistake" has two shapes:
//
//  1. No RAII session is open at all. Every pre-existing Wi-Fi path in this
//     firmware starts the radio directly (WiFi.mode(WIFI_STA) / esp_wifi_start)
//     and tears it down by hand, so a session-based guard alone would watch an
//     empty counter. A radio that is still up this long after the governor last
//     saw it DOWN is the leak, and is switched off.
//  2. An RAII session is open but past its deadline — a caller that forgot to
//     keepAlive() on a long-running sync/OTA.
//
// Design source: docs/design/2026-10-01-endurance-governor-power-stats.md §3.4.

class WifiLeakGuard {
 public:
  // A radio left running this long with no owner is treated as leaked. Long
  // enough that every legitimate path (web server, OTA, font download, OPDS)
  // finishes well inside it.
  static constexpr unsigned long DEFAULT_LEAK_TIMEOUT_MS = 5UL * 60UL * 1000UL;

  // RAII radio session. Nested sessions are counted, so an inner scope cannot
  // stop a radio an outer scope still needs, and the deadline is re-armed on
  // construction so a long session can keep it fresh with keepAlive().
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

  // Main-loop tick (called from EnduranceGovernor::tick()). Force-stops a
  // leaked radio. Returns true when a leak was detected and shut down on this
  // call.
  static bool poll(unsigned long nowMs);

  // True while at least one RAII session is open.
  static bool sessionActive();
  // Number of times poll() has force-stopped a leaked radio this boot.
  static uint32_t leaksStopped();

  enum class StopReason : uint8_t { ScopeExit = 0, LeakTimeout = 1, NoSession = 2 };
  static StopReason lastStopReason();

 private:
  unsigned long leakTimeoutMs_ = DEFAULT_LEAK_TIMEOUT_MS;
};
