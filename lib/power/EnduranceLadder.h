#pragma once

#include <cstdint>

// Pure clock-ladder resolution for the endurance governor.
//
// Deliberately free of Arduino/ESP-IDF includes so the promotion arithmetic —
// the part that actually decides whether an unstable device runs at 10 MHz or
// backs off to 80 MHz — is host-testable without stubs. EnduranceGovernor owns
// the FreeRTOS/NVS side and resolves its clock target through here.
//
// Design source: docs/design/2026-10-01-endurance-governor-power-stats.md §3.1.

namespace endurance {

// User-selectable power profile. Endurance is the shipped default, matching the
// Crossfire binary's first-boot profile name.
enum class Profile : uint8_t {
  Endurance = 0,
  Balanced = 1,
  Performance = 2,
  Count
};

// One rung of the ladder. idlePollSlices is Crossfire's "light sleep enabled"
// dimension (see lib/power/README notes in the design doc's divergence list):
// when set, the idle loop may sleep in poll slices and the input manager may
// run its low-power polling cadence.
struct Strategy {
  int idleClockMHz;
  bool idlePollSlices;
};

// Per-profile defaults, used until strikes promote the device up the ladder.
// renderClockMHz is the clock a heavy job (page render) pre-raises to.
struct ProfileDefaults {
  int idleClockMHz;
  int renderClockMHz;
  bool idlePollSlices;
};

// The four rungs, verbatim from design doc §3.1.
static constexpr Strategy kStrategies[4] = {
    {10, true},   // strategy 0 — idle 10 MHz + light sleep enabled (fully Endurance)
    {40, true},   // strategy 1 — idle raised (40 MHz) + light sleep enabled
    {40, false},  // strategy 2 — idle raised (40 MHz), no light sleep
    {80, false},  // strategy 3 — idle 80 MHz, no light sleep (stock floor)
};

// The floor each profile starts at. Strikes only ever move a device UP the
// ladder (toward the safer higher clocks), so this is the lowest strategy the
// profile can ever occupy.
//
// The design doc names the three profiles but does not pin their base rungs;
// these are the choices made in the port and recorded as a divergence.
static constexpr uint8_t kProfileBaseStrategy[static_cast<uint8_t>(Profile::Count)] = {
    0,  // Endurance   — fully Endurance
    1,  // Balanced    — one rung off the floor
    3,  // Performance — stock clock, no sleep class (nothing left to promote into)
};

static constexpr ProfileDefaults kProfileDefaults[static_cast<uint8_t>(Profile::Count)] = {
    {10, 80, true},   // Endurance
    {40, 80, true},   // Balanced
    {80, 80, false},  // Performance
};

static constexpr uint8_t kStrategyCount = 4;

// A strike on either dimension promotes at least one rung; the two dimensions
// are additive and the ladder saturates at its top rung.
inline uint8_t promote(uint8_t baseStrategy, uint8_t idleStrikes, uint8_t lightSleepStrikes) {
  const unsigned int sum = static_cast<unsigned int>(baseStrategy) + idleStrikes + lightSleepStrikes;
  return static_cast<uint8_t>(sum >= kStrategyCount ? kStrategyCount - 1 : sum);
}

inline Strategy resolveStrategy(Profile profile, uint8_t idleStrikes, uint8_t lightSleepStrikes) {
  const uint8_t base = kProfileBaseStrategy[static_cast<uint8_t>(profile)];
  return kStrategies[promote(base, idleStrikes, lightSleepStrikes)];
}

inline uint8_t strategyIndex(Profile profile, uint8_t idleStrikes, uint8_t lightSleepStrikes) {
  return promote(kProfileBaseStrategy[static_cast<uint8_t>(profile)], idleStrikes, lightSleepStrikes);
}

// Clamp a stored profile byte. A corrupt/migrated value must never index past
// the table above, so every read path runs through this first.
inline Profile clampProfile(uint8_t raw) {
  return raw < static_cast<uint8_t>(Profile::Count) ? static_cast<Profile>(raw) : Profile::Endurance;
}

// Per-profile idle/render clocks. Once a strike has promoted the device, the
// ladder's idle clock wins over the profile's nominal one; the render clock is
// never promoted because a render that cannot keep up is the failure the strike
// ladder is protecting against elsewhere.
inline ProfileDefaults defaultsFor(Strategy strategy, Profile profile) {
  ProfileDefaults d = kProfileDefaults[static_cast<uint8_t>(profile)];
  d.idleClockMHz = strategy.idleClockMHz;
  d.idlePollSlices = strategy.idlePollSlices;
  return d;
}

// Crash-strike byte persisted under the NVS key `endurance.state`.
//   bit0 = idle clock strikes  >= 1
//   bit1 = light sleep strikes >= 1
struct StrikeState {
  static constexpr uint8_t kIdleStrikeBit = 0x01;
  static constexpr uint8_t kLightSleepStrikeBit = 0x02;

  uint8_t idleStrikes = 0;
  uint8_t lightSleepStrikes = 0;

  uint8_t toByte() const { return static_cast<uint8_t>((idleStrikes ? kIdleStrikeBit : 0) |
                                                       (lightSleepStrikes ? kLightSleepStrikeBit : 0)); }
};

inline StrikeState strikeStateFromByte(uint8_t raw) {
  StrikeState s;
  s.idleStrikes = (raw & StrikeState::kIdleStrikeBit) ? 1 : 0;
  s.lightSleepStrikes = (raw & StrikeState::kLightSleepStrikeBit) ? 1 : 0;
  return s;
}

}  // namespace endurance
