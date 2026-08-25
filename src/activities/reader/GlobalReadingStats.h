#pragma once

#include <array>
#include <cstdint>

#include "ReadingStatsUtils.h"

// Aggregate reading statistics across all books, persisted in the SQLite
// reading_stats.db `global_stats` table (see lib/SQLite/ReadingStatsStore.h).
struct GlobalReadingStats {
  uint32_t totalSessions = 0;
  uint32_t totalReadingSeconds = 0;
  uint32_t totalPagesTurned = 0;
  uint32_t completedBooks = 0;
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};
  uint32_t readingHistoryAnchorDay = 0;
  std::array<uint8_t, READING_HISTORY_BYTES> readingHistoryBits{};
  uint16_t longestReadingStreak = 0;

  static GlobalReadingStats load();
  void save() const;
  static bool resetLocal();
  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  uint16_t currentReadingStreak(const ReadingStatsDate* today) const;
  uint16_t displayLongestReadingStreak() const;
};
