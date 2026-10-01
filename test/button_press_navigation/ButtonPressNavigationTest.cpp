#include <gtest/gtest.h>

#include "util/ButtonNavigator.h"
#include "util/RepeatHoldDiscount.h"

unsigned long testNowMs = 1000;

namespace {
constexpr uint8_t PREVIOUS = 1;
constexpr uint8_t NEXT = 2;

class ButtonPressNavigationTest : public ::testing::Test {
 protected:
  MappedInputManager input;
  ButtonNavigator navigator;
  int selected = 0;
  int pages = 0;

  void SetUp() override {
    testNowMs = 1000;
    ButtonNavigator::setMappedInputManager(input);
  }

  // The shared list call order, with the production navigator.
  void navigate() {
    navigator.onNextPress([this] { selected = ButtonNavigator::nextIndex(selected, 10); });
    navigator.onPreviousPress([this] { selected = ButtonNavigator::previousIndex(selected, 10); });
    navigator.onNextContinuous([this] { ++pages; });
    navigator.onPreviousContinuous([this] { --pages; });
  }
};

TEST_F(ButtonPressNavigationTest, PressStepsImmediatelyAndReleaseDoesNotStep) {
  input.frame = {0, NEXT, 0, NEXT};
  navigate();
  EXPECT_EQ(selected, 1);
  input.frame = {90, 0, NEXT, 0};
  navigate();
  EXPECT_EQ(selected, 1);
  EXPECT_EQ(pages, 0);
}

TEST_F(ButtonPressNavigationTest, HoldPagesAfterThresholdAndStopsOnRelease) {
  input.frame = {0, NEXT, 0, NEXT};
  navigate();
  testNowMs += 501;
  input.frame = {501, 0, 0, NEXT};
  navigate();
  EXPECT_EQ(selected, 1);
  EXPECT_EQ(pages, 1);
  testNowMs += 100;
  input.frame.heldMs += 100;
  navigate();
  EXPECT_EQ(pages, 1);
  testNowMs += 1000;
  input.frame = {1601, 0, NEXT, 0};
  navigate();
  EXPECT_EQ(pages, 1);
}

TEST_F(ButtonPressNavigationTest, PressNeverAlsoRepeatsEvenWithOldHeldDuration) {
  input.frame = {2000, NEXT, 0, NEXT};
  navigate();
  EXPECT_EQ(selected, 1);
  EXPECT_EQ(pages, 0);
}

// One physical press = exactly one step, for every button a navigator covers.
// Auto-repeat past continuousStartMs is a deliberate hold affordance; a press
// that has not reached the start threshold must never be answered twice, and
// neither must the composite Next/Previous button lists answer one press once
// per physical button they cover.
TEST_F(ButtonPressNavigationTest, ShortTapOnNextStepsExactlyOnce) {
  input.frame = {0, NEXT, 0, NEXT};
  navigate();
  EXPECT_EQ(selected, 1);
  // Still held, no further edge, but short of the continuous start threshold.
  input.frame = {300, 0, 0, NEXT};
  navigate();
  EXPECT_EQ(selected, 1);
  EXPECT_EQ(pages, 0);
}

TEST_F(ButtonPressNavigationTest, ShortTapOnPreviousStepsExactlyOnce) {
  input.frame = {0, PREVIOUS, 0, PREVIOUS};
  navigate();
  EXPECT_EQ(selected, 9);
  input.frame = {300, 0, 0, PREVIOUS};
  navigate();
  EXPECT_EQ(selected, 9);
  EXPECT_EQ(pages, 0);
}

TEST_F(ButtonPressNavigationTest, FastDirectionChangeStartsANewRepeatInterval) {
  input.frame = {501, 0, 0, NEXT};
  navigate();
  EXPECT_EQ(pages, 1);
  input.frame = {0, PREVIOUS, NEXT, PREVIOUS};
  navigate();
  EXPECT_EQ(selected, 9);
  EXPECT_EQ(pages, 1);
}

// X3/X4 (C3) double input regression: no async poll task, so the page-turn
// refresh blocks the loop task for ~1-2 s. On the first tick after the render
// the button is still held (the user could not see step 1 yet), wall-clock
// held time is past the repeat threshold, and the interval gate also passes.
// The render stall must not count as hold time, or one press steps twice.
TEST_F(ButtonPressNavigationTest, RenderStallWhileStillHeldDoesNotRepeatThePress) {
  input.frame = {0, NEXT, 0, NEXT};
  navigate();
  EXPECT_EQ(selected, 1);
  // The 2 s refresh happened inside the loop; the user never lifted the key.
  testNowMs += 2000;
  input.frame = {2000, 0, 0, NEXT, 2000};
  navigate();
  EXPECT_EQ(selected, 1);
  EXPECT_EQ(pages, 0);
  // Still held through a second render: again exactly one repeat window's
  // worth of real hold is required.
  testNowMs += 2000;
  input.frame = {4000, 0, 0, NEXT, 4000};
  navigate();
  EXPECT_EQ(pages, 0);
}

// The complement: a genuine hold with no stall must still auto-repeat.
TEST_F(ButtonPressNavigationTest, DeliberateHoldWithoutStallStillRepeats) {
  input.frame = {0, NEXT, 0, NEXT};
  navigate();
  EXPECT_EQ(selected, 1);
  testNowMs += 1500;
  input.frame = {1500, 0, 0, NEXT, 0};
  navigate();
  EXPECT_EQ(selected, 1);
  EXPECT_EQ(pages, 1);
}

// A repeat that fired suppresses the release step on every board. On C3-class
// builds the interval floor starts at the edge tick, so this must not depend
// on the floor being zero. (onNextContinuous / onRelease dispatch on the
// CURRENT frame — they are not registrations.)
TEST_F(ButtonPressNavigationTest, ReleaseAfterARepeatDoesNotStepAgain) {
  int steps = 0;
  input.frame = {0, NEXT, 0, NEXT};
  navigate();  // the press tick steps the selection, not the release
  testNowMs += 1500;
  input.frame = {1500, 0, 0, NEXT};
  navigator.onNextContinuous([&steps] { ++steps; });
  EXPECT_EQ(steps, 1);  // one repeat
  input.frame = {1600, 0, NEXT, 0};
  navigator.onRelease(ButtonNavigator::getNextButtons(), [&steps] { ++steps; });
  EXPECT_EQ(steps, 1);  // the release must not answer the same contact again
}

// A stall longer than any plausible discount cap must still cover the WHOLE
// stall: the hold time it is subtracted from is uncapped wall clock, so a
// capped discount re-arms the repeat on the first tick after a multi-second
// chapter build (qodo #1 / copilot).
TEST_F(ButtonPressNavigationTest, VeryLongStallStillCoversTheWholeHold) {
  input.frame = {0, NEXT, 0, NEXT};
  navigate();
  EXPECT_EQ(selected, 1);
  testNowMs += 30000;
  input.frame = {30000, 0, 0, NEXT, 30000};
  navigate();
  EXPECT_EQ(pages, 0);
}

TEST(RepeatHoldDiscountTest, NormalTicksAccrueNothing) {
  EXPECT_EQ(repeathold::stallFor(0), 0u);
  EXPECT_EQ(repeathold::stallFor(10), 0u);
  EXPECT_EQ(repeathold::stallFor(repeathold::kStallThresholdMs), 0u);
}

TEST(RepeatHoldDiscountTest, OnlyTheExcessOverTheThresholdIsAStall) {
  EXPECT_EQ(repeathold::stallFor(repeathold::kStallThresholdMs + 1), 1u);
  EXPECT_EQ(repeathold::stallFor(2000), 2000u - repeathold::kStallThresholdMs);
}

TEST(RepeatHoldDiscountTest, AccumulationSaturatesInsteadOfCapping) {
  const unsigned long fifteenSeconds = 15000;
  const unsigned long accumulated = repeathold::addSaturated(0, fifteenSeconds);
  EXPECT_EQ(repeathold::discount(fifteenSeconds, accumulated), 0u);
  EXPECT_GE(accumulated, fifteenSeconds);
  EXPECT_EQ(repeathold::addSaturated(ULONG_MAX - 5, 10), ULONG_MAX);
}

TEST(RepeatHoldDiscountTest, DiscountSaturatesAtZero) {
  EXPECT_EQ(repeathold::discount(300, 0), 300u);
  EXPECT_EQ(repeathold::discount(300, 300), 0u);
  EXPECT_EQ(repeathold::discount(300, 900), 0u);
}

// The SDK's held clock is aggregate: it starts at the first button down. A
// second button pressed while the navigation button is still held must NOT
// open a new hold window (qodo #2 / kody).
TEST(RepeatHoldDiscountTest, OnlyAnOpeningPressEdgeStartsAContact) {
  EXPECT_TRUE(repeathold::startsNewContact(true, false));
  EXPECT_FALSE(repeathold::startsNewContact(true, true));    // second press mid-hold
  EXPECT_FALSE(repeathold::startsNewContact(false, false));  // no edge
  EXPECT_FALSE(repeathold::startsNewContact(false, true));
}

// The wiring, not just the predicate: StallDiscountWindow is what
// MappedInputManager::update() drives (kody round 2), so the frame-by-frame
// sequence the C3 actually runs is covered here.
TEST(StallDiscountWindowTest, NormalTicksThenARenderStallDiscountsTheHold) {
  repeathold::StallDiscountWindow window;
  // Opening press: nothing was held on the previous frame.
  window.sampleFrame(1000);
  window.deliverFrame(/*hasPressEdge=*/true, /*anyHeldThisFrame=*/true);
  EXPECT_EQ(window.heldMs(0), 0u);
  // 12 ordinary 10 ms ticks.
  unsigned long now = 1000;
  for (int i = 0; i < 12; ++i) {
    now += 10;
    window.sampleFrame(now);
    window.deliverFrame(false, true);
  }
  EXPECT_EQ(window.accumulatedStallMs(), 0u);
  EXPECT_EQ(window.heldMs(120), 120u);  // hold time passes through untouched
  // The e-ink refresh blocks the loop for 2 s; the button is still down.
  now += 2000;
  window.sampleFrame(now);
  window.deliverFrame(false, true);
  EXPECT_EQ(window.accumulatedStallMs(), 2000u - repeathold::kStallThresholdMs);
  // 2120 ms of wall-clock hold minus the 1900 ms stall: the 12 real ticks plus
  // the stall gap's own kStallThresholdMs, which stays credited as loop time.
  // Far below the 500 ms repeat threshold, so the press cannot answer twice.
  EXPECT_EQ(window.heldMs(now - 1000), 120u + repeathold::kStallThresholdMs);
  EXPECT_LT(window.heldMs(now - 1000), 500u);
}

TEST(StallDiscountWindowTest, ASecondPressMidHoldKeepsTheDiscount) {
  repeathold::StallDiscountWindow window;
  window.sampleFrame(1000);
  window.deliverFrame(true, true);
  unsigned long now = 3000;
  window.sampleFrame(now);
  window.deliverFrame(false, true);
  const unsigned long afterStall = window.accumulatedStallMs();
  ASSERT_GT(afterStall, 0u);
  // The user taps a second button while the navigation button is still down:
  // the aggregate SDK hold clock did not restart, so the discount must stay.
  window.sampleFrame(now += 20);
  window.deliverFrame(/*hasPressEdge=*/true, /*anyHeldThisFrame=*/true);
  EXPECT_EQ(window.accumulatedStallMs(), afterStall);
  EXPECT_EQ(window.heldMs(now - 1000), 20u + repeathold::kStallThresholdMs);
  EXPECT_LT(window.heldMs(now - 1000), 500u);
}

TEST(StallDiscountWindowTest, ANewContactAfterFullReleaseStartsClean) {
  repeathold::StallDiscountWindow window;
  window.sampleFrame(1000);
  window.deliverFrame(true, true);
  unsigned long now = 5000;
  window.sampleFrame(now);
  window.deliverFrame(false, true);
  ASSERT_GT(window.accumulatedStallMs(), 0u);
  // Release everything, then press again: the SDK restarts its held clock, so
  // the old contact's stalls must not discount the new one.
  window.sampleFrame(now += 100);
  window.deliverFrame(false, false);
  window.sampleFrame(now += 10);
  window.deliverFrame(/*hasPressEdge=*/true, /*anyHeldThisFrame=*/true);
  EXPECT_EQ(window.accumulatedStallMs(), 0u);
  EXPECT_EQ(window.heldMs(5), 5u);
}

TEST(StallDiscountWindowTest, MultiSecondBuildStallStillDiscountsTheWholeHold) {
  repeathold::StallDiscountWindow window;
  window.sampleFrame(100);
  window.deliverFrame(true, true);
  // A cold large-chapter build on the C3: tens of seconds, no dispatch.
  window.sampleFrame(30100);
  window.deliverFrame(false, true);
  EXPECT_GE(window.accumulatedStallMs(), 30000u - repeathold::kStallThresholdMs);
  // 30 s of wall-clock hold is covered by the 29.9 s of stall; only the gap's
  // own threshold survives, so no cap ever re-arms the repeat here.
  EXPECT_EQ(window.heldMs(30000), repeathold::kStallThresholdMs);
  EXPECT_LT(window.heldMs(30000), 500u);
}

}  // namespace
