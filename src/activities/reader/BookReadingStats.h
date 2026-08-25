#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "ReadingStatsUtils.h"

// Per-book reading statistics, persisted in the SQLite reading_stats.db
// `book_stats` table keyed by the EPUB cache path string (see
// lib/SQLite/ReadingStatsStore.h).
struct BookReadingStats {
  uint16_t sessionCount = 0;
  uint32_t totalReadingSeconds = 0;
  uint32_t totalPagesTurned = 0;
  bool isCompleted = false;
  uint16_t avgSecondsPerForwardPage = 0;
  uint16_t paceSampleCount = 0;
  uint32_t estimatedTimeLeftSeconds = 0;
  bool startDateManual = false;
  bool finishedDateManual = false;
  ReadingStatsDate startDate;
  ReadingStatsDate finishedDate;
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};

  static BookReadingStats load(const std::string& cachePath);
  void save(const std::string& cachePath) const;
  static bool remove(const std::string& cachePath);

  void recordForwardPageRead(uint32_t seconds);
  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  static void formatDuration(uint32_t seconds, char* buf, size_t len);
};
