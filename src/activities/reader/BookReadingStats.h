#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "ReadingStatsUtils.h"

// Per-book reading statistics, persisted to <cachePath>/stats_v6.bin (109-byte
// versioned record inside the book's cache dir; see
// docs/design/reading-stats-binary-files.md). The record's lifetime matches the
// cache dir exactly: created with it, deleted with it, and moved with it on
// move-to-/read — no key migration or orphan cleanup needed.
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
  // Rolling reading-speed window in words per minute (v6 fields).
  WpmWindow wpm;

  static BookReadingStats load(const std::string& cachePath);
  void save(const std::string& cachePath) const;
  static bool remove(const std::string& cachePath);

  void recordForwardPageRead(uint32_t seconds, uint16_t wordsOnPage);
  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  // Zeros the WPM window only; sessions, totals, dates, buckets and the legacy
  // seconds-per-page average survive.
  void clearWpmStats();
  static void formatDuration(uint32_t seconds, char* buf, size_t len);
};
