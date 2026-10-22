// FinishReadingPromptPolicyTest — host suites for the pure finish-reading
// threshold helper (docs/design/2026-10-20-finish-reading-prompt.md). The
// helper is stateless: unrounded progress percent + latch + setting ->
// decision. The runtime callers (EpubReaderActivity / XtcReaderActivity) own
// overlay, end-of-book, and page-turn-render gating; those live on device.

#include <gtest/gtest.h>

#include "activities/reader/FinishReadingPromptPolicy.h"

namespace {

using finish_prompt::Decision;
using finish_prompt::evaluate;

TEST(FinishReadingPromptPolicyTest, BelowZoneNeverFires) {
  // 0..89% read: outside the 10%-left zone.
  for (float progress = 0.0f; progress < 90.0f; ++progress) {
    EXPECT_EQ(evaluate(progress, true, false, false), Decision::None) << "progress=" << progress;
  }
}

TEST(FinishReadingPromptPolicyTest, ZoneBandsSelectHighestCrossed) {
  // 90-94.9% read -> 10% left; 95-98.9% -> 5% left; 99-99.4% -> 1% left.
  // Unrounded float input: fractional percentages select the crossed band.
  for (float progress = 90.0f; progress < 95.0f; ++progress) {
    EXPECT_EQ(evaluate(progress, true, false, false), Decision::Fire10) << "progress=" << progress;
  }
  for (float progress = 95.0f; progress < 99.0f; ++progress) {
    EXPECT_EQ(evaluate(progress, true, false, false), Decision::Fire5) << "progress=" << progress;
  }
  for (float progress = 99.0f; progress < 99.5f; progress += 0.1f) {
    EXPECT_EQ(evaluate(progress, true, false, false), Decision::Fire1) << "progress=" << progress;
  }
  // Fractional (unrounded) input selects the band of the real percentage:
  // just below a boundary stays in the lower band, at/above it moves up.
  EXPECT_EQ(evaluate(94.4f, true, false, false), Decision::Fire10);
  EXPECT_EQ(evaluate(94.9f, true, false, false), Decision::Fire10);
  EXPECT_EQ(evaluate(95.0f, true, false, false), Decision::Fire5);
  EXPECT_EQ(evaluate(98.4f, true, false, false), Decision::Fire5);
  EXPECT_EQ(evaluate(98.9f, true, false, false), Decision::Fire5);
  EXPECT_EQ(evaluate(99.0f, true, false, false), Decision::Fire1);
  EXPECT_EQ(evaluate(99.4f, true, false, false), Decision::Fire1);
}

TEST(FinishReadingPromptPolicyTest, CompletionBoundaryNeverFires) {
  // ADR-6: at/past the 99.5% completion boundary the existing end-of-book
  // flow owns the finish UX (final page / "Mark as finished?" prompt) — the
  // threshold modal never fires there, so the two prompts cannot stack and
  // the last readable page never shows "1% left to read".
  for (float progress = 99.5f; progress <= 100.0f; progress += 0.1f) {
    EXPECT_EQ(evaluate(progress, true, false, false), Decision::None) << "progress=" << progress;
  }
  EXPECT_EQ(evaluate(100.0f, true, false, false), Decision::None);
}

TEST(FinishReadingPromptPolicyTest, LatchSilencesEveryBand) {
  // ADR-2: once dismissed the modal never fires again — no forward-page-turn
  // reset, no later threshold (5%/1%) either. Property sweep across the whole
  // progress range, including the completion boundary.
  for (float progress = 0.0f; progress <= 100.0f; ++progress) {
    EXPECT_EQ(evaluate(progress, true, false, true), Decision::Latched) << "progress=" << progress;
  }
  EXPECT_EQ(evaluate(100.0f, true, false, true), Decision::Latched);
}

TEST(FinishReadingPromptPolicyTest, CompletedBookNeverFires) {
  for (float progress = 0.0f; progress <= 100.0f; ++progress) {
    EXPECT_EQ(evaluate(progress, true, true, false), Decision::None) << "progress=" << progress;
    EXPECT_EQ(evaluate(progress, true, true, true), Decision::Latched) << "progress=" << progress;
  }
  EXPECT_EQ(evaluate(100.0f, true, true, false), Decision::None);
}

TEST(FinishReadingPromptPolicyTest, SettingOffNeverFires) {
  // ADR-3: the toggle gates only the new modals; the latch state is reported
  // even when the setting is off so callers can skip the persist.
  for (float progress = 90.0f; progress <= 100.0f; ++progress) {
    EXPECT_EQ(evaluate(progress, false, false, false), Decision::None) << "progress=" << progress;
    EXPECT_EQ(evaluate(progress, false, false, true), Decision::Latched) << "progress=" << progress;
  }
  EXPECT_EQ(evaluate(100.0f, false, false, false), Decision::None);
}

TEST(FinishReadingPromptPolicyTest, LatchBeatsSettingAndCompletion) {
  // The latch is the strongest signal: it reports Latched regardless of the
  // setting or the completion state, so callers never re-latch on save.
  EXPECT_EQ(evaluate(97.0f, false, false, true), Decision::Latched);
  EXPECT_EQ(evaluate(97.0f, true, true, true), Decision::Latched);
  EXPECT_EQ(evaluate(100.0f, false, true, true), Decision::Latched);
}

}  // namespace
