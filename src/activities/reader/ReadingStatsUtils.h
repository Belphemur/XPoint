#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

struct BookReadingStats;
struct GlobalReadingStats;

constexpr size_t READING_TIME_BUCKET_COUNT = 4;
constexpr uint16_t MIN_BOOK_PACE_SAMPLES = 10;
constexpr uint32_t MIN_GLOBAL_PACE_PAGE_TURNS = 50;
constexpr size_t READING_DAY_OF_WEEK_COUNT = 7;
constexpr size_t READING_HISTORY_DAYS = 730;
constexpr size_t READING_HISTORY_BYTES = (READING_HISTORY_DAYS + 7) / 8;

constexpr size_t WPM_WINDOW_SIZE = 15;
constexpr size_t WPM_TRIM_COUNT = 2;
constexpr uint16_t WPM_HARD_CAP = 900;  // discard samples above this: not a plausible reading speed
constexpr uint16_t WPM_FLOOR = 80;      // clamp slower samples to this
constexpr uint16_t CALIBRATED_WORDS_PER_PAGE = 220;

enum class ReadingTimeBucket : uint8_t { Morning = 0, Afternoon, Evening, Night };

// Rolling 15-sample reading-speed window (words per minute) with a trimmed
// mean: the two fastest and two slowest samples are dropped so a single
// anomalous page (a glance away, an unusually easy page) doesn't skew the
// estimate. Samples are contiguous in [0, count); the window wraps only once
// full, so the circular buffer is never read with a gap.
struct WpmWindow {
  std::array<uint16_t, WPM_WINDOW_SIZE> samples{};
  uint16_t avg = 0;   // trimmed mean, 0 until the first sample
  uint8_t pos = 0;    // next insertion slot
  uint8_t count = 0;  // valid samples, 0..WPM_WINDOW_SIZE

  void record(uint32_t seconds, uint16_t wordsOnPage);
  uint16_t trimmedMean() const;
  // Clamps count/pos loaded from untrusted binary data (samples[pos] must stay
  // in bounds) and recomputes the average from the window.
  void normalize();
  void clear() {
    samples.fill(0);
    avg = 0;
    pos = 0;
    count = 0;
  }
  bool full() const { return count >= WPM_WINDOW_SIZE; }
};

struct ReadingStatsDate {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;

  bool isValid() const;
  void clear();
};

struct ReadingStatsDateTime {
  ReadingStatsDate date;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;

  bool isValid() const;
};

bool isLeapYear(uint16_t year);
uint8_t daysInMonth(uint16_t year, uint8_t month);
bool isValidReadingStatsDate(const ReadingStatsDate& date);
int compareReadingStatsDate(const ReadingStatsDate& lhs, const ReadingStatsDate& rhs);
void addDaysToReadingStatsDate(ReadingStatsDate& date, int delta);
void addSecondsToReadingStatsDateTime(ReadingStatsDateTime& dt, uint32_t seconds);
uint32_t readingStatsDayIndex(const ReadingStatsDate& date);
bool readingStatsDateFromDayIndex(uint32_t dayIndex, ReadingStatsDate& outDate);
uint8_t readingStatsDayOfWeekIndex(const ReadingStatsDate& date);  // Monday = 0
ReadingTimeBucket readingTimeBucketForHour(uint8_t hour);
bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime& outDateTime);

// Lightweight accessor for CrossPointSettings::shouldTrackReadingStats().
// Defined in ReadingStatsUtils.cpp so library code (e.g. lib/SQLite) can gate
// on the setting without including the heavy CrossPointSettings.h header.
bool readingStatsTrackingEnabled();

uint16_t readingSpanDaysInclusive(const ReadingStatsDate& start, const ReadingStatsDate& end);
uint16_t readingSpanDaysElapsed(const ReadingStatsDate& start, const ReadingStatsDate& end);
void formatReadingStatsShortDate(const ReadingStatsDate& date, char* buf, size_t len);
void formatReadingStatsMonthToken(const ReadingStatsDate& date, char* buf, size_t len);
void formatCompactReadingDuration(uint32_t seconds, char* buf, size_t len);

std::optional<uint32_t> resolveReadingPaceSecondsPerPage(const BookReadingStats& bookStats,
                                                         const GlobalReadingStats& globalStats);
std::optional<uint32_t> estimateChapterTimeLeftSeconds(const BookReadingStats& bookStats,
                                                       const GlobalReadingStats& globalStats, uint16_t pagesRemaining);
std::optional<uint32_t> estimateBookTimeLeftSeconds(const BookReadingStats& bookStats,
                                                    const GlobalReadingStats& globalStats,
                                                    uint32_t estimatedRemainingPages);
void formatChapterTimeLeft(uint32_t seconds, char* buf, size_t len);

void recordReadingSpanIntoBuckets(std::array<uint32_t, READING_TIME_BUCKET_COUNT>& timeOfDaySeconds,
                                  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT>& dayOfWeekSeconds,
                                  const ReadingStatsDateTime& localStart, uint32_t seconds);
void markReadingHistoryDay(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits, uint32_t dayIndex);
void recordReadingSpanIntoHistory(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                  const ReadingStatsDateTime& localStart, uint32_t seconds);
void mergeReadingHistory(uint32_t& targetAnchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& targetBits,
                         uint32_t sourceAnchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& sourceBits);
uint16_t computeReadingHistoryLongestStreak(uint32_t anchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits);
uint16_t computeReadingHistoryCurrentStreak(uint32_t anchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                            const ReadingStatsDate* today);

// Lean, dependency-free database key for a book's reading stats.
// [Dead code since the binary-store migration: the store keys records by the
// cache path directly. Kept only because ReadingStatsUtilsTest still exercises
// it; remove together with that test when doing a cleanup pass.]
std::string bookStatsDbKey(const std::string& cachePath);
