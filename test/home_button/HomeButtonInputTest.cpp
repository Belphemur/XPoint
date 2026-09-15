#include <gtest/gtest.h>

#include <cstdint>

#include "util/HomeButtonInput.h"

namespace {

class HomeButtonInputTest : public ::testing::Test {
 protected:
  using A = HomeButtonAction;

  HomeButtonAction tick(uint32_t now, bool tap = false, bool hold = false, bool swipe = false, bool press = false) {
    return input.update(now, tap, hold, swipe, press, A::Home, A::ToggleFrontlight, A::ReaderMenu);
  }

  HomeButtonInput input;
};

TEST_F(HomeButtonInputTest, RecognizesConfiguredGestures) {
  EXPECT_EQ(tick(0, true), A::Ignore);
  EXPECT_EQ(tick(350), A::Ignore);
  EXPECT_EQ(tick(351), A::Home);
  EXPECT_EQ(tick(352), A::Ignore);
  EXPECT_EQ(tick(1000, true), A::Ignore);
  EXPECT_EQ(tick(1350, true), A::ToggleFrontlight);
  EXPECT_EQ(tick(1701), A::Ignore);
  EXPECT_EQ(tick(2000, false, true), A::ReaderMenu);
  EXPECT_EQ(tick(2001), A::Ignore);
}

TEST_F(HomeButtonInputTest, SwipeCancelsPendingTapAndHoldIsConsumed) {
  EXPECT_EQ(tick(0, true), A::Ignore);
  EXPECT_EQ(tick(100, false, false, true), A::Ignore);
  EXPECT_EQ(tick(500), A::Ignore);

  EXPECT_EQ(tick(1000, false, true), A::ReaderMenu);
  EXPECT_EQ(tick(1001), A::Ignore);

  EXPECT_EQ(tick(2000, false, true), A::ReaderMenu);
  EXPECT_EQ(tick(2001), A::Ignore);
}

TEST_F(HomeButtonInputTest, ReportsGestureProvenance) {
  EXPECT_EQ(tick(0, true), A::Ignore);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::None);

  EXPECT_EQ(tick(351), A::Home);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::Tap);

  EXPECT_EQ(tick(1000, true), A::Ignore);
  EXPECT_EQ(tick(1350, true), A::ToggleFrontlight);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::DoubleTap);

  EXPECT_EQ(tick(2000, false, true), A::ReaderMenu);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::Hold);
  EXPECT_EQ(tick(2001), A::Ignore);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::None);
}

TEST_F(HomeButtonInputTest, TapThenHoldSuppressesEarlySingleTap) {
  EXPECT_EQ(tick(3000, true), A::Ignore);
  EXPECT_EQ(tick(3200, false, false, false, true), A::Ignore);
  EXPECT_EQ(tick(3400), A::Ignore);
  EXPECT_EQ(tick(3900, false, true), A::ReaderMenu);
  EXPECT_EQ(tick(4000), A::Ignore);
}

TEST_F(HomeButtonInputTest, ResetClearsPendingTap) {
  EXPECT_EQ(tick(5000, true), A::Ignore);
  EXPECT_EQ(tick(5100, true, false, true), A::Ignore);
  EXPECT_EQ(tick(5500), A::Ignore);
  EXPECT_EQ(tick(6000, true), A::Ignore);
  input.reset();
  EXPECT_EQ(tick(6500), A::Ignore);
}

TEST_F(HomeButtonInputTest, MillisecondClockWrapUsesUnsignedSubtraction) {
  EXPECT_EQ(tick(UINT32_MAX - 100, true), A::Ignore);
  EXPECT_EQ(tick(100, true), A::ToggleFrontlight);
  EXPECT_EQ(tick(UINT32_MAX - 100, true), A::Ignore);
  EXPECT_EQ(tick(251), A::Home);
}

TEST_F(HomeButtonInputTest, SeparatedTapsEachProduceOneAction) {
  EXPECT_EQ(tick(7000, true), A::Ignore);
  EXPECT_EQ(tick(7400, true), A::Home);
  EXPECT_EQ(tick(7751), A::Home);
}

TEST_F(HomeButtonInputTest, DisabledDoubleTapRemovesDelay) {
  EXPECT_EQ(input.update(8000, true, false, false, false, A::Bookmark, A::Ignore, A::ReaderMenu), A::Bookmark);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::Tap);
}

TEST(HomeButtonValues, UpstreamAndForkIndicesAreStable) {
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Home), 0);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Ignore), 1);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::NextPage), 2);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Refresh), 3);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Footnotes), 4);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Confirm), 5);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Sync), 6);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Bookmark), 7);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Dictionary), 8);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::ReaderMenu), 9);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::ToggleFrontlight), 10);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Sleep), 11);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Screenshot), 12);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::GoBack), 13);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Count), 14);
}

}  // namespace
