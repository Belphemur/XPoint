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
  ASSERT_EQ(book.wpm.count, 15u);
  book.clearWpmStats();
  EXPECT_EQ(book.wpm.count, 0u);
  EXPECT_EQ(book.wpm.avg, 0u);
  for (const auto sample : book.wpm.samples) {
    EXPECT_EQ(sample, 0u);
  }
  GlobalReadingStats global;
  EXPECT_FALSE(resolveReadingPaceSecondsPerPage(book, global).has_value());
}
