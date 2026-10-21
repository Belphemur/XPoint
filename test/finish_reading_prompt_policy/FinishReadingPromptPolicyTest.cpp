// FinishReadingPromptPolicyTest — host suites for the pure finish-reading
// threshold helper (docs/design/2026-10-20-finish-reading-prompt.md). The
// helper is stateless: progress percent + latch + setting -> decision. The
// runtime callers (EpubReaderActivity / XtcReaderActivity) own overlay,
// end-of-book, and page-turn-render gating; those live on device.

#include <gtest/gtest.h>

#include "activities/reader/FinishReadingPromptPolicy.h"

namespace {

using finish_prompt::Decision;
using finish_prompt::evaluate;

TEST(FinishReadingPromptPolicyTest, BelowZoneNeverFires) {
  // 0..89% read: outside the 10%-left zone.
  for (int progress = 0; progress < 90; ++progress) {
    EXPECT_EQ(evaluate(progress, true, false, false), Decision::None) << "progress=" << progress;
  }
}

TEST(FinishReadingPromptPolicyTest, ZoneBandsSelectHighestCrossed) {
  // 90-94% read -> 10% left; 95-98% -> 5% left; 99-100% -> 1% left.
  for (int progress = 90; progress < 95; ++progress) {
    EXPECT_EQ(evaluate(progress, true, false, false), Decision::Fire10) << "progress=" << progress;
  }
  for (int progress = 95; progress < 99; ++progress) {
    EXPECT_EQ(evaluate(progress, true, false, false), Decision::Fire5) << "progress=" << progress;
  }
  for (int progress = 99; progress <= 100; ++progress) {
    EXPECT_EQ(evaluate(progress, true, false, false), Decision::Fire1) << "progress=" << progress;
  }
}

TEST(FinishReadingPromptPolicyTest, LatchSilencesEveryBand) {
  // ADR-2: once dismissed the modal never fires again — no forward-page-turn
  // reset, no later threshold (5%/1%) either. Property sweep across the whole
  // progress range.
  for (int progress = 0; progress <= 100; ++progress) {
    EXPECT_EQ(evaluate(progress, true, false, true), Decision::Latched) << "progress=" << progress;
  }
}

TEST(FinishReadingPromptPolicyTest, CompletedBookNeverFires) {
  for (int progress = 0; progress <= 100; ++progress) {
    EXPECT_EQ(evaluate(progress, true, true, false), Decision::None) << "progress=" << progress;
    EXPECT_EQ(evaluate(progress, true, true, true), Decision::Latched) << "progress=" << progress;
  }
}

TEST(FinishReadingPromptPolicyTest, SettingOffNeverFires) {
  // ADR-3: the toggle gates only the new modals; the latch state is reported
  // even when the setting is off so callers can skip the persist.
  for (int progress = 90; progress <= 100; ++progress) {
    EXPECT_EQ(evaluate(progress, false, false, false), Decision::None) << "progress=" << progress;
    EXPECT_EQ(evaluate(progress, false, false, true), Decision::Latched) << "progress=" << progress;
  }
}

TEST(FinishReadingPromptPolicyTest, LatchBeatsSettingAndCompletion) {
  // The latch is the strongest signal: it reports Latched regardless of the
  // setting or the completion state, so callers never re-latch on save.
  EXPECT_EQ(evaluate(97, false, false, true), Decision::Latched);
  EXPECT_EQ(evaluate(97, true, true, true), Decision::Latched);
  EXPECT_EQ(evaluate(100, false, true, true), Decision::Latched);
}

}  // namespace
