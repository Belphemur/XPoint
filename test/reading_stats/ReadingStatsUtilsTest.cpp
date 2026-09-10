#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingStatsUtils.h"

TEST(ReadingStatsUtilsTest, ResolvePacePrefersBookPace) {
  BookReadingStats book;
  GlobalReadingStats global;
  global.totalPagesTurned = 100;
  global.totalReadingSeconds = 3000;  // 30 s/page global
  for (int i = 0; i < 15; ++i) {
    book.recordForwardPageRead(60, 220);  // 220 WPM -> 220*60/220 = 60 s/page
  }
  ASSERT_EQ(book.wpm.count, 15u);
  ASSERT_EQ(book.wpm.avg, 220u);

  const auto pace = resolveReadingPaceSecondsPerPage(book, global);
  ASSERT_TRUE(pace.has_value());
  EXPECT_EQ(*pace, 60u);
}

TEST(ReadingStatsUtilsTest, ResolvePaceFallsBackToGlobal) {
  BookReadingStats book;
  // Book window not full -> book WPM unavailable, fall back to global WPM.
  GlobalReadingStats global;
  for (int i = 0; i < 15; ++i) {
    global.recordGlobalPageRead(60, 110);  // 110 WPM -> 220*60/110 = 120 s/page
  }
  global.totalPagesTurned = 50;
  global.totalReadingSeconds = 1500;  // 30 s/page global (fallback after WPM)

  const auto pace = resolveReadingPaceSecondsPerPage(book, global);
  ASSERT_TRUE(pace.has_value());
  EXPECT_EQ(*pace, 120u);
}

TEST(ReadingStatsUtilsTest, ResolvePaceNulloptWhenInsufficientData) {
  BookReadingStats book;
  // Book window not full, global window not full, global page count below
  // threshold -> no estimate.
  GlobalReadingStats global;
  global.totalPagesTurned = 49;
  global.totalReadingSeconds = 1470;

  const auto pace = resolveReadingPaceSecondsPerPage(book, global);
  EXPECT_FALSE(pace.has_value());
}

TEST(ReadingStatsUtilsTest, ResolvePaceGuardsDivisionByZero) {
  BookReadingStats book;
  GlobalReadingStats global;
  global.totalPagesTurned = 50;
  global.totalReadingSeconds = 0;

  const auto pace = resolveReadingPaceSecondsPerPage(book, global);
  EXPECT_FALSE(pace.has_value());
}

TEST(ReadingStatsUtilsTest, EstimateChapterTimeLeftBasic) {
  BookReadingStats book;
  for (int i = 0; i < 15; ++i) {
    book.recordForwardPageRead(60, 440);  // 440 WPM -> 30 s/page
  }
  GlobalReadingStats global;

  const auto estimate = estimateChapterTimeLeftSeconds(book, global, 10);
  ASSERT_TRUE(estimate.has_value());
  EXPECT_EQ(*estimate, 300u);
}

TEST(ReadingStatsUtilsTest, EstimateChapterTimeLeftGlobalFallback) {
  BookReadingStats book;
  GlobalReadingStats global;
  global.totalPagesTurned = 100;
  global.totalReadingSeconds = 6000;  // 60 s/page

  const auto estimate = estimateChapterTimeLeftSeconds(book, global, 5);
  ASSERT_TRUE(estimate.has_value());
  EXPECT_EQ(*estimate, 300u);
}

TEST(ReadingStatsUtilsTest, EstimateChapterTimeLeftNulloptWhenNoPace) {
  BookReadingStats book;
  GlobalReadingStats global;

  const auto estimate = estimateChapterTimeLeftSeconds(book, global, 5);
  EXPECT_FALSE(estimate.has_value());
}

TEST(ReadingStatsUtilsTest, EstimateChapterTimeLeftZeroWhenNoPagesRemaining) {
  BookReadingStats book;
  for (int i = 0; i < 15; ++i) {
    book.recordForwardPageRead(60, 440);
  }
  GlobalReadingStats global;

  const auto estimate = estimateChapterTimeLeftSeconds(book, global, 0);
  ASSERT_TRUE(estimate.has_value());
  EXPECT_EQ(*estimate, 0u);
}

TEST(ReadingStatsUtilsTest, EstimateChapterTimeLeftHandlesMaxValues) {
  BookReadingStats book;
  // WPM = words*60/seconds. To land at 900 WPM exactly, use 15 words in 1 s.
  for (int i = 0; i < 15; ++i) {
    book.recordForwardPageRead(1, 15);  // 900 WPM
  }
  ASSERT_EQ(book.wpm.avg, 900u);
  GlobalReadingStats global;

  const auto estimate = estimateChapterTimeLeftSeconds(book, global, std::numeric_limits<uint16_t>::max());
  ASSERT_TRUE(estimate.has_value());
  // pace = 220*60/900 = 14 (integer division); pagesRemaining = 65535
  EXPECT_GT(*estimate, 0u);
}

TEST(ReadingStatsUtilsTest, EstimateBookTimeLeftBasic) {
  BookReadingStats book;
  for (int i = 0; i < 15; ++i) {
    book.recordForwardPageRead(60, 440);  // 30 s/page
  }
  GlobalReadingStats global;

  const auto estimate = estimateBookTimeLeftSeconds(book, global, 100);
  ASSERT_TRUE(estimate.has_value());
  EXPECT_EQ(*estimate, 3000u);
}

TEST(ReadingStatsUtilsTest, EstimateBookTimeLeftZeroWhenNoPagesRemaining) {
  BookReadingStats book;
  for (int i = 0; i < 15; ++i) {
    book.recordForwardPageRead(60, 440);
  }
  GlobalReadingStats global;

  const auto estimate = estimateBookTimeLeftSeconds(book, global, 0);
  ASSERT_TRUE(estimate.has_value());
  EXPECT_EQ(*estimate, 0u);
}

TEST(ReadingStatsUtilsTest, EstimateBookTimeLeftNulloptWhenNoPace) {
  BookReadingStats book;
  GlobalReadingStats global;

  const auto estimate = estimateBookTimeLeftSeconds(book, global, 100);
  EXPECT_FALSE(estimate.has_value());
}

TEST(ReadingStatsUtilsTest, EstimateBookTimeLeftClampsOnOverflow) {
  BookReadingStats book;
  for (int i = 0; i < 15; ++i) {
    book.recordForwardPageRead(60, 440);
  }
  GlobalReadingStats global;

  const auto estimate = estimateBookTimeLeftSeconds(book, global, std::numeric_limits<uint32_t>::max());
  ASSERT_TRUE(estimate.has_value());
  EXPECT_EQ(*estimate, std::numeric_limits<uint32_t>::max());
}

TEST(ReadingStatsUtilsTest, FormatChapterTimeLeftLessThanOneMinute) {
  char buf[24];
  formatChapterTimeLeft(0, buf, sizeof(buf));
  EXPECT_STREQ(buf, "< 1 min left");

  formatChapterTimeLeft(59, buf, sizeof(buf));
  EXPECT_STREQ(buf, "< 1 min left");
}

TEST(ReadingStatsUtilsTest, FormatChapterTimeLeftOneMinute) {
  char buf[24];
  formatChapterTimeLeft(60, buf, sizeof(buf));
  EXPECT_STREQ(buf, "~1 min left");
}

TEST(ReadingStatsUtilsTest, FormatChapterTimeLeftRoundsToTwoMinutes) {
  char buf[24];
  formatChapterTimeLeft(90, buf, sizeof(buf));
  EXPECT_STREQ(buf, "~2 min left");
}

TEST(ReadingStatsUtilsTest, FormatChapterTimeLeftTwoHours) {
  char buf[24];
  formatChapterTimeLeft(7200, buf, sizeof(buf));
  EXPECT_STREQ(buf, "~120 min left");
}

TEST(ReadingStatsUtilsTest, WpmHardCapDiscard) {
  WpmWindow w;
  w.record(60, 901);  // 901 WPM > cap -> discarded
  w.record(1, 1000);  // 60000 WPM -> discarded
  EXPECT_EQ(w.count, 0u);
  EXPECT_EQ(w.avg, 0u);
}

TEST(ReadingStatsUtilsTest, WpmTrimmedMean) {
  WpmWindow w;
  w.record(60, 500);  // outliers, shuffled order to exercise sorting
  w.record(60, 100);
  for (int i = 0; i < 11; ++i) {
    w.record(60, 200);
  }
  w.record(60, 100);
  w.record(60, 500);
  ASSERT_EQ(w.count, 15u);
  // Trimmed mean drops the 2 fastest + 2 slowest -> eleven 200s remain.
  EXPECT_EQ(w.avg, 200u);
}

TEST(ReadingStatsUtilsTest, WpmFloor) {
  WpmWindow w;
  w.record(100, 100);  // 60 WPM -> clamped up to the 80 WPM floor
  EXPECT_EQ(w.count, 1u);
  EXPECT_EQ(w.avg, 80u);
}

TEST(ReadingStatsUtilsTest, WpmZeroInputIgnored) {
  WpmWindow w;
  w.record(0, 100);
  w.record(60, 0);
  EXPECT_EQ(w.count, 0u);
  EXPECT_EQ(w.avg, 0u);
}

TEST(ReadingStatsUtilsTest, SessionWindowMinSecondsGate) {
  SessionWindow w;
  w.record(29);  // below the 30 s gate -> rejected
  EXPECT_EQ(w.count, 0u);
  w.record(30);
  EXPECT_EQ(w.count, 1u);
  EXPECT_EQ(w.avg, 30u);
}

TEST(ReadingStatsUtilsTest, SessionWindowOverflowClamp) {
  SessionWindow w;
  w.record(70000);  // > UINT16_MAX -> clamped to 65535
  EXPECT_EQ(w.count, 1u);
  EXPECT_EQ(w.samples[0], 65535u);
  EXPECT_EQ(w.avg, 65535u);
}

TEST(ReadingStatsUtilsTest, SessionWindowTrimmedMeanFullWindow) {
  SessionWindow w;
  // Two outliers each side (shuffled), eight ordinary 600 s sessions.
  w.record(1200);
  w.record(300);
  for (int i = 0; i < 8; ++i) {
    w.record(600);
  }
  w.record(300);
  w.record(1200);
  ASSERT_EQ(w.count, 10u);
  // Trim-2 drops both 300s and both 1200s -> the middle 6 are all 600.
  EXPECT_EQ(w.avg, 600u);
}

TEST(ReadingStatsUtilsTest, SessionWindowPartialPlainMeanUpToFourSamples) {
  SessionWindow w;
  w.record(100);
  w.record(200);
  w.record(300);
  w.record(400);
  ASSERT_EQ(w.count, 4u);
  // Not enough samples to trim: plain mean of the 4.
  EXPECT_EQ(w.avg, 250u);
}

TEST(ReadingStatsUtilsTest, SessionWindowPartialTrimmedBeyondFourSamples) {
  SessionWindow w;
  w.record(100);
  w.record(200);
  w.record(300);
  w.record(400);
  w.record(500);
  ASSERT_EQ(w.count, 5u);
  // Trim-2 drops 100 and 500 -> mean of 200, 300, 400.
  EXPECT_EQ(w.avg, 300u);
}

TEST(ReadingStatsUtilsTest, SessionWindowNormalizeRepairsStalePartialPos) {
  SessionWindow w;
  w.record(100);
  w.record(200);
  w.record(300);
  ASSERT_EQ(w.count, 3u);
  w.pos = 0;  // stale in-range pos, as a corrupt record could carry
  w.normalize();
  EXPECT_EQ(w.pos, 3u);  // partial-window invariant: pos == count
  EXPECT_EQ(w.avg, 200u);
}

TEST(ReadingStatsUtilsTest, SessionWindowNormalizeResetsOutOfRangeFullPos) {
  SessionWindow w;
  for (int i = 0; i < 10; ++i) {
    w.record(600);
  }
  ASSERT_EQ(w.count, 10u);
  w.pos = 200;  // out of range after a (hypothetical) corrupt full window
  w.normalize();
  EXPECT_EQ(w.pos, 0u);
  EXPECT_EQ(w.avg, 600u);
}

TEST(ReadingStatsUtilsTest, SessionWindowCircularWrapKeepsLastTen) {
  SessionWindow w;
  // 13 sequential records: the two 100s and first 600 fall off as the
  // window wraps; the last 10 are the eleven 600s minus one, all equal.
  w.record(100);
  w.record(100);
  for (int i = 0; i < 11; ++i) {
    w.record(600);
  }
  EXPECT_EQ(w.count, 10u);
  EXPECT_EQ(w.avg, 600u);
}

TEST(ReadingStatsUtilsTest, AvgSessionSecondsHelpers) {
  // Nothing at all -> "-" sentinel (the cells render "-", not "< 1 min").
  EXPECT_FALSE(avgSessionSeconds(0, 0, 0, 0).has_value());
  // Totals without counted sessions (each < 60 s) -> still no value.
  EXPECT_FALSE(avgSessionSeconds(0, 0, 45, 0).has_value());
  // Empty window + legacy totals -> legacy arithmetic mean.
  EXPECT_EQ(avgSessionSeconds(0, 0, 3600, 60).value_or(0), 60u);
  // Window below the display gate (1-3 samples) -> still the legacy mean.
  EXPECT_EQ(avgSessionSeconds(300, 3, 3600, 60).value_or(0), 60u);
  // Window at/above the display gate -> the window's running result wins.
  EXPECT_EQ(avgSessionSeconds(300, 4, 3600, 60).value_or(0), 300u);
  // 64-bit legacy mean: large global totals must not overflow.
  EXPECT_EQ(avgSessionSeconds(0, 0, 5000000000ULL, 1000000ULL).value_or(0), 5000u);
}

TEST(ReadingStatsUtilsTest, ResolvePaceFromWpm) {
  BookReadingStats book;
  for (int i = 0; i < 15; ++i) {
    book.recordForwardPageRead(60, 300);  // full window at 300 WPM
  }
  GlobalReadingStats global;
  const auto pace = resolveReadingPaceSecondsPerPage(book, global);
  ASSERT_TRUE(pace.has_value());
  // WPM estimate (220*60/300 = 44 s) wins over the legacy book average (60 s).
  EXPECT_EQ(*pace, 44u);
}

TEST(ReadingStatsUtilsTest, ResolvePaceWpmNeedsFullWindow) {
  BookReadingStats book;
  for (int i = 0; i < 14; ++i) {
    book.recordForwardPageRead(60, 300);
  }
  ASSERT_EQ(book.wpm.count, 14u);
  // 14 samples is one short of the full 15-sample window, so no book WPM
  // estimate. No global WPM and no legacy total page count is set either —
  // resolveReadingPaceSecondsPerPage must report no estimate rather than
  // fall back to something the user did not opt into.
  GlobalReadingStats global;
  const auto pace = resolveReadingPaceSecondsPerPage(book, global);
  EXPECT_FALSE(pace.has_value());
}

TEST(ReadingStatsUtilsTest, ResolvePaceGlobalWpmFallback) {
  BookReadingStats book;  // no book data at all
  GlobalReadingStats global;
  for (int i = 0; i < 15; ++i) {
    global.recordGlobalPageRead(60, 220);  // full window at 220 WPM
  }
  global.totalPagesTurned = 10;  // below the legacy page-turn threshold
  const auto pace = resolveReadingPaceSecondsPerPage(book, global);
  ASSERT_TRUE(pace.has_value());
  EXPECT_EQ(*pace, 60u);  // 220*60/220
}

TEST(ReadingStatsUtilsTest, WpmClearStatsResetsWindow) {
  BookReadingStats book;
  for (int i = 0; i < 15; ++i) {
    book.recordForwardPageRead(60, 220);
  }
  book.recordSession(600);
  ASSERT_EQ(book.wpm.count, 15u);
  ASSERT_EQ(book.sessionWindow.count, 1u);
  book.clearWpmStats();
  EXPECT_EQ(book.wpm.count, 0u);
  EXPECT_EQ(book.wpm.avg, 0u);
  for (const auto sample : book.wpm.samples) {
    EXPECT_EQ(sample, 0u);
  }
  // "Clear reading speed" also resets the session window (D3): the session
  // average falls back to the all-time arithmetic mean.
  EXPECT_EQ(book.sessionWindow.count, 0u);
  EXPECT_EQ(book.sessionWindow.avg, 0u);
  // Totals survive the clear.
  EXPECT_EQ(book.totalReadingSeconds, 0u);  // nothing added in this test
  GlobalReadingStats global;
  EXPECT_FALSE(resolveReadingPaceSecondsPerPage(book, global).has_value());
}

TEST(ReadingStatsUtilsTest, ProgressLineFormatProduces14PercentPlus1h15m) {
  BookReadingStats book;
  book.lastBookProgressPercent = 14;
  book.estimatedTimeLeftSeconds = 75u * 60u;  // 1h 15m

  char buf[48];
  formatHomeProgressLine(book, buf, sizeof(buf));
  EXPECT_STREQ(buf, "14% \xE2\x80\xA2 1h 15m");
}

TEST(ReadingStatsUtilsTest, ProgressLineFormatUnknownReturnsDash) {
  BookReadingStats book;  // default: UNKNOWN_BOOK_PROGRESS_PERCENT

  char buf[48];
  formatHomeProgressLine(book, buf, sizeof(buf));
  EXPECT_STREQ(buf, "-");
}

TEST(ReadingStatsUtilsTest, ProgressLineFormatZeroTimeLeftReturnsDash) {
  BookReadingStats book;
  book.lastBookProgressPercent = 14;
  book.estimatedTimeLeftSeconds = 0;

  char buf[48];
  formatHomeProgressLine(book, buf, sizeof(buf));
  EXPECT_STREQ(buf, "14% \xE2\x80\xA2 --");
}
