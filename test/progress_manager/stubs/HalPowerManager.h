// Host-test stub of HalPowerManager — only what ProgressManager's lowBattery()
// gate reads. Test-controllable so battery branches can be forced.
#pragma once

#include <cstdint>

class HalPowerManager {
 public:
  enum class BatteryHealthState : uint8_t { HEALTHY, DEGRADED, UNKNOWN };

  bool isBatteryCharging() const { return charging_; }
  BatteryHealthState getBatteryHealthState() const { return health_; }
  int getBatteryPercentage() const { return percentage_; }

  // Test control.
  bool charging_ = false;
  BatteryHealthState health_ = BatteryHealthState::HEALTHY;
  int percentage_ = 100;
};

// The firmware links the SDK's lowercase `powerManager` singleton; the host
// stub defines the same symbol (in Stubs.cpp).
extern HalPowerManager powerManager;
