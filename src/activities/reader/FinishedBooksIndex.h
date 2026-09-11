#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BookReadingStats.h"

struct FinishedBookEntry {
  uint64_t pathKey = 0;
  std::string title;
  std::string author;
  uint32_t totalReadingSeconds = 0;
  ReadingStatsDate startDate;
  ReadingStatsDate finishedDate;
};

#ifdef READING_STATS_TEST
struct FinishedBookRecoveryBook {
  std::string path;
  std::string title;
  std::string author;
};
#endif

class FinishedBooksIndex {
 public:
  static constexpr size_t MAX_ENTRIES = 32;

  static std::vector<FinishedBookEntry> load();
  static bool record(const std::string& path, const std::string& title, const std::string& author,
                     const BookReadingStats& stats);
  static bool recordCanonical(const std::string& bookPath, const std::string& legacyCachePath,
                              const std::string& title, const std::string& author, const BookReadingStats& stats);
  static bool migratePath(const std::string& oldPath, const std::string& newPath);

#ifdef READING_STATS_TEST
  static void setRecentBooksForTest(const std::vector<FinishedBookRecoveryBook>& books);
  static void clearRecentBooksForTest();
#endif
};
