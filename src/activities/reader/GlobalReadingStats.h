#pragma once

#include <array>
#include <cstdint>

#include "ReadingStatsUtils.h"

// Aggregate reading statistics across all books, persisted to
// /.crosspoint/global_stats.bin (195-byte versioned record; see
// docs/design/reading-stats-binary-files.md).
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
  // Rolling reading-speed window in words per minute (v4 fields).
  WpmWindow wpm;

  static GlobalReadingStats load();
  void save() const;
  static bool resetLocal();

 private:
  // Set when a newer-format file was detected on load: all saves are refused
  // until the user explicitly resets stats, so this build can never clobber
  // data written by a future firmware.
  static bool s_blockDestructiveSave;

 public:
  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  void recordGlobalPageRead(uint32_t seconds, uint16_t wordsOnPage);
  // Zeros the WPM window only; sessions, totals, buckets and streaks survive.
  void clearWpmStats();
  // Consecutive days with recorded reading ending at today (or the given
  // anchor date in tests).
  uint16_t currentReadingStreakDays(const ReadingStatsDate* today = nullptr) const;
  // Longest run of consecutive reading days ever recorded.
  uint16_t longestReadingStreakDays() const;
};
