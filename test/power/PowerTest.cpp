// Host coverage for the pure half of the endurance governor: the clock ladder
// and the drain-rate arithmetic. Neither unit touches Arduino/ESP-IDF, so no
// stubs are needed — which is also what keeps them testable on the host.
//
// Design source: docs/design/2026-10-01-endurance-governor-power-stats.md §3.1/§3.2.

#include <gtest/gtest.h>

#include <cstring>

#include "EnduranceLadder.h"
#include "PowerDrainMonitor.h"

using endurance::Profile;
using endurance::StrikeState;

// ---------------------------------------------------------------------------
// Clock ladder (design doc §3.1)
// ---------------------------------------------------------------------------

TEST(EnduranceLadder, StrategiesMatchTheDesignDocTable) {
  ASSERT_EQ(4u, endurance::kStrategyCount);
  EXPECT_EQ(10, endurance::kStrategies[0].idleClockMHz);
  EXPECT_TRUE(endurance::kStrategies[0].idlePollSlices);
  EXPECT_EQ(40, endurance::kStrategies[1].idleClockMHz);
  EXPECT_TRUE(endurance::kStrategies[1].idlePollSlices);
  EXPECT_EQ(40, endurance::kStrategies[2].idleClockMHz);
  EXPECT_FALSE(endurance::kStrategies[2].idlePollSlices);
  EXPECT_EQ(80, endurance::kStrategies[3].idleClockMHz);
  EXPECT_FALSE(endurance::kStrategies[3].idlePollSlices);
}

TEST(EnduranceLadder, EnduranceProfileStartsAtTheFloor) {
  const auto s = endurance::resolveStrategy(Profile::Endurance, 0, 0);
  EXPECT_EQ(10, s.idleClockMHz);
  EXPECT_TRUE(s.idlePollSlices);
}

TEST(EnduranceLadder, PerformanceProfileStartsAtTheStockFloor) {
  const auto s = endurance::resolveStrategy(Profile::Performance, 0, 0);
  EXPECT_EQ(80, s.idleClockMHz);
  EXPECT_FALSE(s.idlePollSlices);
}

TEST(EnduranceLadder, EachStruckDimensionPromotesAtLeastOneLevel) {
  const uint8_t base = endurance::kProfileBaseStrategy[static_cast<uint8_t>(Profile::Endurance)];
  ASSERT_EQ(0u, base);

  EXPECT_EQ(1u, endurance::promote(base, 1, 0)) << "idle strike alone must promote one rung";
  EXPECT_EQ(1u, endurance::promote(base, 0, 1)) << "light-sleep strike alone must promote one rung";
  EXPECT_EQ(2u, endurance::promote(base, 1, 1)) << "both dimensions must add, not saturate at one rung";
}

TEST(EnduranceLadder, PromotionSaturatesAtTheTopRung) {
  EXPECT_EQ(3u, endurance::promote(0, 5, 5));
  EXPECT_EQ(3u, endurance::promote(3, 0, 0));
}

TEST(EnduranceLadder, PromotionIsMonotonic) {
  uint8_t previous = endurance::promote(0, 0, 0);
  for (unsigned int strikes = 1; strikes <= 6; strikes++) {
    const uint8_t current = endurance::promote(0, static_cast<uint8_t>(strikes), 0);
    EXPECT_GE(current, previous) << "adding strikes must never lower the clock target";
    previous = current;
  }
}

TEST(EnduranceLadder, StrikeStateByteUsesTheDocumentedBits) {
  StrikeState none;
  EXPECT_EQ(0x00u, none.toByte());
  none.idleStrikes = 1;
  EXPECT_EQ(0x01u, none.toByte()) << "bit0 must be the idle strike";
  none.lightSleepStrikes = 1;
  EXPECT_EQ(0x03u, none.toByte()) << "bit1 must be the light-sleep strike";

  const auto decoded = endurance::strikeStateFromByte(0x03);
  EXPECT_EQ(1u, decoded.idleStrikes);
  EXPECT_EQ(1u, decoded.lightSleepStrikes);
}

TEST(EnduranceLadder, EscalateClimbsPastTheBaseRungOnRepeatStrikes) {
  // The bug this pins: strike counters are boolean flags, so a promotion
  // recomputed from the profile base lands on base+1 every time and an Endurance
  // device (base 0) can never reach rung 3. Escalating from the CURRENT rung is
  // what makes the ladder a ladder.
  uint8_t rung = endurance::kProfileBaseStrategy[static_cast<uint8_t>(Profile::Endurance)];
  ASSERT_EQ(0u, rung);

  rung = endurance::escalate(rung, 1, 0);
  EXPECT_EQ(1u, rung);
  rung = endurance::escalate(rung, 1, 0);
  EXPECT_EQ(2u, rung) << "a second idle strike must climb past base+1";
  rung = endurance::escalate(rung, 1, 0);
  EXPECT_EQ(3u, rung) << "the ladder must reach the stock 80 MHz floor";
  rung = endurance::escalate(rung, 1, 1);
  EXPECT_EQ(3u, rung) << "and saturate there, never overrun";
}

TEST(EnduranceLadder, EscalateFromTheBaseIsIdempotentWhichIsTheBug) {
  // Documents the defect shape so a future refactor cannot quietly reintroduce
  // it: re-deriving from the base yields the same rung no matter how many times
  // the device fails.
  const uint8_t base = endurance::kProfileBaseStrategy[static_cast<uint8_t>(Profile::Endurance)];
  EXPECT_EQ(endurance::promote(base, 1, 0), endurance::promote(base, 1, 0));
  EXPECT_NE(endurance::escalate(endurance::escalate(base, 1, 0), 1, 0), endurance::promote(base, 1, 0))
      << "escalating from the current rung must differ from re-deriving from the base";
}

TEST(EnduranceLadder, CrashStrikeStateIsRepresentable) {
  // reportInstability(Crash) strikes both dimensions, so the persisted byte must
  // carry both bits; a crash that sets neither is the defect kody/coderabbit
  // flagged as a silently-dropped strike.
  endurance::StrikeState crash;
  crash.idleStrikes = 1;
  crash.lightSleepStrikes = 1;
  EXPECT_EQ(0x03u, crash.toByte());
  const auto decoded = endurance::strikeStateFromByte(crash.toByte());
  EXPECT_EQ(1u, decoded.idleStrikes);
  EXPECT_EQ(1u, decoded.lightSleepStrikes);
}

TEST(EnduranceLadder, ClampProfileRejectsOutOfRangeBytes) {
  EXPECT_EQ(Profile::Endurance, endurance::clampProfile(0));
  EXPECT_EQ(Profile::Balanced, endurance::clampProfile(1));
  EXPECT_EQ(Profile::Performance, endurance::clampProfile(2));
  // A corrupt settings byte must never index past the tables above.
  EXPECT_EQ(Profile::Endurance, endurance::clampProfile(3));
  EXPECT_EQ(Profile::Endurance, endurance::clampProfile(0xFF));
}

TEST(EnduranceLadder, DefaultsForTakesTheStrategyClockNotTheProfileNominal) {
  endurance::Strategy promoted{40, false};
  const auto defaults = endurance::defaultsFor(promoted, Profile::Endurance);
  EXPECT_EQ(40, defaults.idleClockMHz) << "a promoted strategy must win over the profile's nominal 10 MHz";
  EXPECT_FALSE(defaults.idlePollSlices);
  // The render clock is never promoted away.
  EXPECT_GE(defaults.renderClockMHz, 80);
}

// ---------------------------------------------------------------------------
// Drain math (design doc §3.2)
// ---------------------------------------------------------------------------

TEST(PowerDrain, PercentPerHourFormula) {
  // 5% over 10 minutes => 5 * 3600 / 600 = 30 %/h => 30000 milli.
  EXPECT_EQ(30000u, PowerDrainMonitor::percentPerHourMilli(80, 75, 600000UL));
  // 10% over 1 hour => 10 %/h.
  EXPECT_EQ(10000u, PowerDrainMonitor::percentPerHourMilli(90, 80, 3600000UL));
}

TEST(PowerDrain, PercentPerHourIsZeroForNonDrain) {
  // A rising gauge is a charge, not a drain.
  EXPECT_EQ(0u, PowerDrainMonitor::percentPerHourMilli(70, 80, 600000UL));
  EXPECT_EQ(0u, PowerDrainMonitor::percentPerHourMilli(80, 80, 600000UL));
  // A zero-length window cannot be divided by.
  EXPECT_EQ(0u, PowerDrainMonitor::percentPerHourMilli(80, 75, 0UL));
}

TEST(PowerDrain, PercentPerHourDoesNotOverflowOnAFullPackOverTheWindow) {
  // 100% over the minimum window is the arithmetic worst case the sampling code
  // can produce. A 32-bit accumulator wraps here (100 * 3600 * 1000 * 1000 =
  // 3.6e11) and reports a plausible-looking but tiny rate.
  const uint32_t rate = PowerDrainMonitor::percentPerHourMilli(100, 0, PowerDrainMonitor::MEASUREMENT_WINDOW_MS);
  EXPECT_EQ(600000u, rate) << "100% in 10 minutes is 600 %/h";
  EXPECT_GT(rate, 300000u) << "a wrapped 32-bit accumulator would land far below this";
}

TEST(PowerDrain, AvgMilliAmpFormula) {
  // avg_mA = %/h * 1100 / 100 = %/h * 11.
  EXPECT_EQ(110000u, PowerDrainMonitor::avgMilliAmpMilli(10000u)) << "10 %/h => 110 mA";
  EXPECT_EQ(330000u, PowerDrainMonitor::avgMilliAmpMilli(30000u)) << "30 %/h => 330 mA";
  EXPECT_EQ(0u, PowerDrainMonitor::avgMilliAmpMilli(0u));
}

TEST(PowerDrain, RuntimeLeftFormula) {
  // 50% of a 1100 mAh pack is 550 mAh; at 110 mA that is 5 hours = 300 minutes.
  // Missing the /100 that turns the percentage into a fraction reported 500
  // hours here, which the old expectation silently blessed.
  EXPECT_EQ(5UL * 60UL, PowerDrainMonitor::runtimeLeftMinutes(50, 110000u));
  EXPECT_EQ(10UL * 60UL, PowerDrainMonitor::runtimeLeftMinutes(100, 110000u));
  // A drained pack has no runtime left.
  EXPECT_EQ(0UL, PowerDrainMonitor::runtimeLeftMinutes(0, 110000u));
}

TEST(PowerDrain, Regression_PercentageIsAFractionNotAMultiplier) {
  // Half the pack must be half the runtime. A whole-pack reading would make
  // runtimeLeftMinutes(50, ...) == runtimeLeftMinutes(100, ...).
  const unsigned long half = PowerDrainMonitor::runtimeLeftMinutes(50, 110000u);
  const unsigned long full = PowerDrainMonitor::runtimeLeftMinutes(100, 110000u);
  EXPECT_GT(full, 0UL);
  EXPECT_EQ(full / 2, half) << "50% remaining must be half the runtime of 100%";
}

TEST(PowerDrain, RuntimeLeftIsZeroWithoutAUsableRate) {
  // Division by a zero rate is the case the overlay must not attempt.
  EXPECT_EQ(0UL, PowerDrainMonitor::runtimeLeftMinutes(80, 0u));
}

TEST(PowerDrain, AssumedCapacityIsTheDocumentedCrossfireConstant) {
  EXPECT_EQ(1100u, PowerDrainMonitor::ASSUMED_CAPACITY_MAH);
}

namespace {
// The monitor stays free of the i18n layer, so the unit labels are injected. The
// overlay passes tr() strings; the test pins the shape with literals.
constexpr PowerDrainMonitor::DurationLabels kLabels{"<1m", "%lum", "%luh %lum", "%lud %luh"};
}  // namespace

TEST(PowerDrain, FormatDurationIsHumanised) {
  char buf[32];
  EXPECT_STREQ("<1m", PowerDrainMonitor::formatDuration(buf, sizeof(buf), 0, kLabels));
  EXPECT_STREQ("45m", PowerDrainMonitor::formatDuration(buf, sizeof(buf), 45, kLabels));
  EXPECT_STREQ("1h 5m", PowerDrainMonitor::formatDuration(buf, sizeof(buf), 65, kLabels));
  EXPECT_STREQ("2h 0m", PowerDrainMonitor::formatDuration(buf, sizeof(buf), 120, kLabels));
  EXPECT_STREQ("3d 4h", PowerDrainMonitor::formatDuration(buf, sizeof(buf), 3 * 24 * 60 + 4 * 60, kLabels));
}

TEST(PowerDrain, FormatDurationAlwaysTerminatesInAFixedBuffer) {
  char tiny[5];
  std::memset(tiny, 'X', sizeof(tiny));
  PowerDrainMonitor::formatDuration(tiny, sizeof(tiny), 123456, kLabels);
  EXPECT_EQ('\0', tiny[sizeof(tiny) - 1]) << "must not write past the caller's buffer";
  EXPECT_LE(std::strlen(tiny), sizeof(tiny) - 1);
}

// ---------------------------------------------------------------------------
// 10-minute measurement gate (design doc §3.2)
// ---------------------------------------------------------------------------

TEST(PowerDrain, ReportsMeasuringBeforeTheGateOpens) {
  PowerDrainMonitor mon;
  unsigned long now = 1000;
  mon.sample(90, now);
  // Nine minutes of samples: still inside the gate.
  now += 9 * 60 * 1000;
  mon.sample(89, now);
  EXPECT_FALSE(mon.estimate().measured);
  EXPECT_LT(mon.estimate().windowMs, PowerDrainMonitor::MEASUREMENT_WINDOW_MS);
}

TEST(PowerDrain, ReportsARateOnceTheGateOpens) {
  PowerDrainMonitor mon;
  unsigned long now = 1000;
  mon.sample(90, now);
  now += 10 * 60 * 1000;
  mon.sample(85, now);
  EXPECT_TRUE(mon.estimate().measured);
  EXPECT_EQ(30000u, mon.estimate().milliPctPerHour);
  EXPECT_EQ(330000u, mon.estimate().milliAmp);
}

TEST(PowerDrain, NeedsAtLeastTwoSamples) {
  PowerDrainMonitor mon;
  mon.sample(90, 1000);
  EXPECT_FALSE(mon.estimate().measured) << "a single sample cannot produce a rate";
}

TEST(PowerDrain, ChargeRestartsTheWindowRatherThanReportingAcrossIt) {
  PowerDrainMonitor mon;
  unsigned long now = 1000;
  mon.sample(90, now);
  now += 5 * 60 * 1000;
  mon.sample(88, now);
  // The user plugs in: the gauge goes UP. The 90->88 window must not survive.
  mon.sample(95, now + 1000);
  EXPECT_FALSE(mon.estimate().measured);
  EXPECT_EQ(95u, mon.lastPercent());
  EXPECT_EQ(0u, mon.estimate().samples) << "the rising sample must not be counted as a drain sample";
}

TEST(PowerDrain, InvalidSamplesAreIgnored) {
  PowerDrainMonitor mon;
  EXPECT_FALSE(mon.sample(PowerDrainMonitor::kInvalid, 1000));
  EXPECT_EQ(PowerDrainMonitor::kInvalid, mon.lastPercent());
}

TEST(PowerDrain, UsbPowerFlagIsCarriedForTheOverlay) {
  PowerDrainMonitor mon;
  unsigned long now = 1000;
  mon.sample(90, now);
  now += 20 * 60 * 1000;
  mon.sample(80, now);
  ASSERT_TRUE(mon.estimate().measured);

  // Plugging in mid-session discards the window: the pre-attach drain and the
  // post-attach charge describe two different states.
  mon.setOnUsbPower(true);
  EXPECT_TRUE(mon.estimate().onUsbPower) << "overlay keys 'n/a on USB power' off this flag";
  EXPECT_FALSE(mon.estimate().measured);
}

TEST(PowerDrain, DetachingUsbRestartsTheGate) {
  PowerDrainMonitor mon;
  mon.setOnUsbPower(true);
  unsigned long now = 1000;
  mon.sample(90, now);
  mon.setOnUsbPower(false);
  EXPECT_FALSE(mon.estimate().onUsbPower);

  // The window restarts from the detach: the pre-attach samples are gone, so a
  // full 10-minute window is required again after the first post-detach sample.
  now += 60 * 1000;
  mon.sample(89, now);
  EXPECT_FALSE(mon.estimate().measured) << "one post-detach sample cannot make a rate";
  now += PowerDrainMonitor::MEASUREMENT_WINDOW_MS;
  mon.sample(85, now);
  EXPECT_TRUE(mon.estimate().measured) << "measurement restarts from the detach";
}

// ---------------------------------------------------------------------------
// Per-sleep window (design doc §3.2)
// ---------------------------------------------------------------------------

TEST(PowerDrain, ShortSleepIsTooShortToRate) {
  PowerDrainMonitor mon;
  mon.beginSleep(80, 1000, false);
  // 20 s is under MIN_SLEEP_WINDOW_MS.
  mon.endSleep(79, 1000 + 20 * 1000, false);
  EXPECT_TRUE(mon.lastSleep().valid);
  EXPECT_FALSE(mon.lastSleep().rateable) << "overlay must print 'too short to rate'";
  EXPECT_EQ(0u, mon.lastSleep().milliPctPerHour);
}

TEST(PowerDrain, LongSleepProducesAPerSleepRate) {
  PowerDrainMonitor mon;
  mon.beginSleep(80, 1000, false);
  // 10 hours, 4% drained => 0.4 %/h => 400 milli.
  mon.endSleep(76, 1000 + 36000000UL, false);
  EXPECT_TRUE(mon.lastSleep().rateable);
  EXPECT_EQ(400u, mon.lastSleep().milliPctPerHour);
}

TEST(PowerDrain, SleepOnUsbPowerRecordsNoWindow) {
  PowerDrainMonitor mon;
  mon.beginSleep(80, 1000, true);
  mon.endSleep(70, 1000UL * 60UL * 60UL * 10UL, true);
  EXPECT_FALSE(mon.lastSleep().valid);
  EXPECT_FALSE(mon.lastSleep().rateable);
}

TEST(PowerDrain, SleepWithoutAnOpenWindowIsIgnored) {
  PowerDrainMonitor mon;
  mon.endSleep(50, 100000, false);
  EXPECT_FALSE(mon.lastSleep().valid) << "an unmatched close must not invent a sleep record";
}

TEST(PowerDrain, RisingGaugeDuringSleepIsNotADrain) {
  PowerDrainMonitor mon;
  mon.beginSleep(50, 1000, false);
  mon.endSleep(60, 1000UL * 60UL * 60UL * 10UL, false);
  EXPECT_FALSE(mon.lastSleep().valid);
}

// ---------------------------------------------------------------------------
// Load-bearing checks: reinject the suspected bug and the suite must fail.
// ---------------------------------------------------------------------------

TEST(PowerDrain, Regression_RuntimeLeftIsNotOneHundredTimesTooLong) {
  // Without the /100 that turns a percentage into a fraction, 50% of an 1100 mAh
  // pack at 110 mA reported 500 hours instead of 5. Pin the real value so the
  // defect cannot come back silently through the test.
  EXPECT_EQ(300UL, PowerDrainMonitor::runtimeLeftMinutes(50, 110000u));
  EXPECT_LT(PowerDrainMonitor::runtimeLeftMinutes(50, 110000u), 10UL * 60UL);
}

TEST(PowerDrain, Regression_ZeroDivisionGuardIsLoadBearing) {
  // If the zero-rate guard in runtimeLeftMinutes were removed, a device that
  // has measured a 10-minute window with no drain at all would divide by zero
  // and report an absurd runtime instead of "unknown".
  EXPECT_EQ(0UL, PowerDrainMonitor::runtimeLeftMinutes(100, 0u));
}

TEST(PowerDrain, Regression_TooShortGuardIsLoadBearing) {
  // If the MIN_SLEEP_WINDOW_MS gate were removed, a 20 s sleep would be rated.
  PowerDrainMonitor mon;
  mon.beginSleep(80, 0, false);
  mon.endSleep(70, 20 * 1000, false);
  ASSERT_TRUE(mon.lastSleep().valid);
  EXPECT_FALSE(mon.lastSleep().rateable);
}

TEST(PowerDrain, Regression_TenMinuteGateIsLoadBearing) {
  // If the gate were removed, a 30-second window would be reported as a rate.
  PowerDrainMonitor mon;
  mon.sample(90, 0);
  mon.sample(89, 30 * 1000);
  EXPECT_FALSE(mon.estimate().measured);
}
