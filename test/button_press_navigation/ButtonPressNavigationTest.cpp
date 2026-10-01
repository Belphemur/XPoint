#include <gtest/gtest.h>

#include "util/ButtonNavigator.h"

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

}  // namespace
