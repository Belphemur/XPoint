#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingStatsUtils.h"

TEST(ReadingStatsUtilsTest, ResolvePacePrefersBookPace) {
  BookReadingStats book;
  book.paceSampleCount = 10;
  book.avgSecondsPerForwardPage = 45;
  GlobalReadingStats global;
  global.totalPagesTurned = 100;
  global.totalReadingSeconds = 3000;  // 30 s/page global

  const auto pace = resolveReadingPaceSecondsPerPage(book, global);
  ASSERT_TRUE(pace.has_value());
  EXPECT_EQ(*pace, 45u);
}

TEST(ReadingStatsUtilsTest, ResolvePaceFallsBackToGlobal) {
  BookReadingStats book;
  book.paceSampleCount = 5;
  book.avgSecondsPerForwardPage = 45;
  GlobalReadingStats global;
  global.totalPagesTurned = 50;
  global.totalReadingSeconds = 1500;  // 30 s/page global

  const auto pace = resolveReadingPaceSecondsPerPage(book, global);
  ASSERT_TRUE(pace.has_value());
  EXPECT_EQ(*pace, 30u);
}

TEST(ReadingStatsUtilsTest, ResolvePaceNulloptWhenInsufficientData) {
  BookReadingStats book;
  book.paceSampleCount = 9;
  book.avgSecondsPerForwardPage = 45;
  GlobalReadingStats global;
  global.totalPagesTurned = 49;
  global.totalReadingSeconds = 1470;

  const auto pace = resolveReadingPaceSecondsPerPage(book, global);
  EXPECT_FALSE(pace.has_value());
}

TEST(ReadingStatsUtilsTest, ResolvePaceGuardsDivisionByZero) {
  BookReadingStats book;
  book.paceSampleCount = 0;
  GlobalReadingStats global;
  global.totalPagesTurned = 50;
  global.totalReadingSeconds = 0;

  const auto pace = resolveReadingPaceSecondsPerPage(book, global);
  EXPECT_FALSE(pace.has_value());
}

TEST(ReadingStatsUtilsTest, EstimateChapterTimeLeftBasic) {
  BookReadingStats book;
  book.paceSampleCount = 10;
  book.avgSecondsPerForwardPage = 30;
  GlobalReadingStats global;

  const auto estimate = estimateChapterTimeLeftSeconds(book, global, 10);
  ASSERT_TRUE(estimate.has_value());
  EXPECT_EQ(*estimate, 300u);
}

TEST(ReadingStatsUtilsTest, EstimateChapterTimeLeftGlobalFallback) {
  BookReadingStats book;
  book.paceSampleCount = 0;
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
  book.paceSampleCount = 10;
  book.avgSecondsPerForwardPage = 30;
  GlobalReadingStats global;

  const auto estimate = estimateChapterTimeLeftSeconds(book, global, 0);
  ASSERT_TRUE(estimate.has_value());
  EXPECT_EQ(*estimate, 0u);
}

TEST(ReadingStatsUtilsTest, EstimateChapterTimeLeftHandlesMaxValues) {
  BookReadingStats book;
  book.paceSampleCount = 10;
  book.avgSecondsPerForwardPage = std::numeric_limits<uint16_t>::max();  // 65535
  GlobalReadingStats global;

  const auto estimate = estimateChapterTimeLeftSeconds(book, global, std::numeric_limits<uint16_t>::max());  // 65535
  ASSERT_TRUE(estimate.has_value());
  EXPECT_EQ(*estimate, static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) *
                           static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()));
}

TEST(ReadingStatsUtilsTest, EstimateBookTimeLeftBasic) {
  BookReadingStats book;
  book.paceSampleCount = 10;
  book.avgSecondsPerForwardPage = 30;
  GlobalReadingStats global;

  const auto estimate = estimateBookTimeLeftSeconds(book, global, 100);
  ASSERT_TRUE(estimate.has_value());
  EXPECT_EQ(*estimate, 3000u);
}

TEST(ReadingStatsUtilsTest, EstimateBookTimeLeftZeroWhenNoPagesRemaining) {
  BookReadingStats book;
  book.paceSampleCount = 10;
  book.avgSecondsPerForwardPage = 30;
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
  book.paceSampleCount = 10;
  book.avgSecondsPerForwardPage = std::numeric_limits<uint16_t>::max();
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
