// Host tests for the X4 Pro power click-window policy
// (src/util/PowerClickWindow.h) — the combination rows of
// docs/design/2026-09-22-input-coverage.md §3 marked ✅.
#include <gtest/gtest.h>

#include "util/PowerClickWindow.h"

namespace {

using P = PowerClickWindow;
using Verdict = P::Verdict;

// Defaults used across the suite: confirmHoldMs=400 mirrors
// CrossPointSettings::getPowerButtonDuration() with the PWR_CONFIRM binding;
// 0 = the binding is off.
constexpr uint32_t kNoConfirm = 0;
constexpr uint32_t kConfirm = 400;

struct TickResult {
  P::Result result;
  uint32_t windowStart;
};

class PowerClickWindowTest : public ::testing::Test {
 protected:
  // One tick against a member window state.
  TickResult tick(const bool release, const bool combo, const uint32_t now, const uint32_t held,
                  const uint32_t confirmHold = kNoConfirm) {
    const auto r = P::tick(windowStart_, release, combo, now, held, confirmHold);
    return {r, windowStart_};
  }

  uint32_t windowStart_ = 0;
};

// A single short click arms the window and holds its release; the window
// expires after 500ms and delivers the deferred release with the Confirm
// edge (the PWR_CONFIRM shortcut keys off it).
TEST_F(PowerClickWindowTest, SingleClickArmsThenExpiryDelivers) {
  const auto armed = tick(true, false, 1000, 120);
  EXPECT_EQ(armed.result.serveRelease, false);
  EXPECT_EQ(armed.result.holdRelease, true);
  EXPECT_EQ(armed.windowStart, 1000u);

  const auto held = tick(false, false, 1400, 0);
  EXPECT_EQ(held.result.serveRelease, false);

  const auto expired = tick(false, false, 1501, 0);
  EXPECT_EQ(expired.result.serveRelease, true);
  EXPECT_EQ(expired.result.confirmEdge, true);
  EXPECT_EQ(expired.windowStart, 0u);
}

// Window expiry exactly at the boundary (<= 500ms) is NOT expiry.
TEST_F(PowerClickWindowTest, ExpiryUsesStrictlyGreaterThan) {
  EXPECT_EQ(tick(true, false, 1000, 100).windowStart, 1000u);
  const auto atBoundary = tick(false, false, 1500, 0);
  EXPECT_EQ(atBoundary.result.serveRelease, false);
  const auto pastBoundary = tick(false, false, 1501, 0);
  EXPECT_EQ(pastBoundary.result.serveRelease, true);
}

// Two short clicks inside the window: the second resolves the double-click
// and both releases stay swallowed (no serveRelease on either tick).
TEST_F(PowerClickWindowTest, DoubleClickSwallowsBoth) {
  EXPECT_EQ(tick(true, false, 1000, 120).result.holdRelease, true);
  const auto second = tick(true, false, 1250, 110);
  EXPECT_EQ(second.result.doubleClick, true);
  EXPECT_EQ(second.result.serveRelease, false);
  EXPECT_EQ(second.result.holdRelease, false);
  EXPECT_EQ(second.windowStart, 0u);
}

// A hold past the click window but inside the Confirm carve-out raises the
// Confirm edge and delivers nothing (the activity reads wasPowerConfirmClick).
TEST_F(PowerClickWindowTest, ConfirmCarveOut) {
  const auto r = tick(true, false, 1000, 350, kConfirm);
  EXPECT_EQ(r.result.confirmEdge, true);
  EXPECT_EQ(r.result.serveRelease, false);
  EXPECT_EQ(r.result.doubleClick, false);
}

// The carve-out requires the PWR_CONFIRM binding: with confirmHoldMs=0 the
// same hold delivers the release to the Power handlers.
TEST_F(PowerClickWindowTest, ConfirmCarveOutRequiresBinding) {
  const auto r = tick(true, false, 1000, 350, kNoConfirm);
  EXPECT_EQ(r.result.serveRelease, true);
  EXPECT_EQ(r.result.confirmEdge, false);
}

// A hold beyond BOTH the click window and the carve-out delivers the release
// (the hold-power-off grammar handles it; the click window adds nothing).
TEST_F(PowerClickWindowTest, ConfirmCarveOutExceededDelivers) {
  const auto r = tick(true, false, 1000, 450, kConfirm);
  EXPECT_EQ(r.result.serveRelease, true);
  EXPECT_EQ(r.result.confirmEdge, false);
}

// F2: a Power+Down release on the same tick (screenshot combo ending) is
// served immediately — never armed, so no short-power action can fire at
// +500ms.
TEST_F(PowerClickWindowTest, ComboReleaseNotArmed) {
  const auto r = tick(true, true, 1000, 120);
  EXPECT_EQ(r.result.serveRelease, true);
  EXPECT_EQ(r.result.holdRelease, false);
  EXPECT_EQ(r.result.confirmEdge, false);
  EXPECT_EQ(r.windowStart, 0u);
}

// F2 (staggered): with a window already open, the combo release closes it and
// serves (cancel semantics the main.cpp combo-end path relies on via
// cancelPowerClickWindow()).
TEST_F(PowerClickWindowTest, ComboReleaseClosesOpenWindow) {
  EXPECT_EQ(tick(true, false, 1000, 120).windowStart, 1000u);
  const auto combo = tick(true, true, 1100, 130);
  EXPECT_EQ(combo.result.serveRelease, true);
  EXPECT_EQ(combo.result.doubleClick, false);
  EXPECT_EQ(combo.windowStart, 0u);
}

// F3: a new physical release on the expiry tick re-arms for its own window
// (a double-click spanning the expiry boundary) while the deferred release is
// still delivered.
TEST_F(PowerClickWindowTest, ExpiryPreservesNewRelease) {
  windowStart_ = 500;  // expires at 1001
  const auto expiry = tick(true, false, 1100, 100);
  EXPECT_EQ(expiry.result.serveRelease, true);
  EXPECT_EQ(expiry.result.confirmEdge, true);
  EXPECT_EQ(expiry.result.holdRelease, true);
  EXPECT_EQ(expiry.windowStart, 1100u);

  // The re-armed click still resolves as a double-click when its own second
  // click arrives inside the new window.
  const auto second = tick(true, false, 1200, 90);
  EXPECT_EQ(second.result.doubleClick, true);
  EXPECT_EQ(second.windowStart, 0u);
}

// A held (long) release on the expiry tick does not re-arm: it delivers
// through the deferred publish and is classified on its own (carve-out here).
TEST_F(PowerClickWindowTest, ExpiryWithHoldReleaseDeliversWithoutArm) {
  windowStart_ = 500;
  const auto expiry = tick(true, false, 1100, 350, kConfirm);
  EXPECT_EQ(expiry.result.serveRelease, true);
  EXPECT_EQ(expiry.result.confirmEdge, true);
  EXPECT_EQ(expiry.result.holdRelease, false);
  EXPECT_EQ(expiry.windowStart, 0u);
}

// Combo release on the expiry tick: only the deferred delivery, no re-arm.
TEST_F(PowerClickWindowTest, ExpiryWithComboReleaseDoesNotArm) {
  windowStart_ = 500;
  const auto expiry = tick(true, true, 1100, 100);
  EXPECT_EQ(expiry.result.serveRelease, true);
  EXPECT_EQ(expiry.result.holdRelease, false);
  EXPECT_EQ(expiry.windowStart, 0u);
}

// No release and no open window: inert.
TEST_F(PowerClickWindowTest, QuietTickIsInert) {
  const auto r = tick(false, false, 1000, 0);
  EXPECT_EQ(r.result.serveRelease, false);
  EXPECT_EQ(r.result.confirmEdge, false);
  EXPECT_EQ(r.result.doubleClick, false);
  EXPECT_EQ(r.windowStart, 0u);
}

// The classify contract the carve-out and window share (verdict matrix).
TEST(PowerClickClassify, VerdictMatrix) {
  EXPECT_EQ(P::classify(false, 100, kNoConfirm), Verdict::Arm);
  EXPECT_EQ(P::classify(true, 100, kNoConfirm), Verdict::DoubleClick);
  EXPECT_EQ(P::classify(true, 300, kNoConfirm), Verdict::DoubleClick);
  EXPECT_EQ(P::classify(false, 301, kNoConfirm), Verdict::Deliver);
  EXPECT_EQ(P::classify(true, 301, kNoConfirm), Verdict::Deliver);
  EXPECT_EQ(P::classify(true, 350, kConfirm), Verdict::Confirm);
  EXPECT_EQ(P::classify(true, 400, kConfirm), Verdict::Confirm);
  EXPECT_EQ(P::classify(true, 401, kConfirm), Verdict::Deliver);
  EXPECT_EQ(P::classify(true, 100, kConfirm), Verdict::DoubleClick);
}

// The carve-out boundary: exactly the confirm duration still confirms.
TEST(PowerClickClassify, CarveOutBoundaries) {
  EXPECT_EQ(P::classify(false, 400, kConfirm), Verdict::Confirm);
  EXPECT_EQ(P::classify(false, 401, kConfirm), Verdict::Deliver);
  EXPECT_EQ(P::classify(false, 300, kConfirm), Verdict::Arm);
}

}  // namespace
