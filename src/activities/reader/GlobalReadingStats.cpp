#include "GlobalReadingStats.h"

#include <algorithm>

#include <ReadingStatsStore.h>

GlobalReadingStats GlobalReadingStats::load() {
  GlobalReadingStats stats;
  ReadingStats.loadGlobal(stats);
  return stats;
}

void GlobalReadingStats::save() const {
  ReadingStats.saveGlobal(*this);
}

bool GlobalReadingStats::resetLocal() {
  const GlobalReadingStats empty;
  ReadingStats.saveGlobal(empty);
  return true;
}

void GlobalReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
  recordReadingSpanIntoHistory(readingHistoryAnchorDay, readingHistoryBits, localStart, seconds);
  const uint16_t historyLongest = computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits);
  if (historyLongest > longestReadingStreak) {
    longestReadingStreak = historyLongest;
  }
}

uint16_t GlobalReadingStats::currentReadingStreak(const ReadingStatsDate* today) const {
  return computeReadingHistoryCurrentStreak(readingHistoryAnchorDay, readingHistoryBits, today);
}

uint16_t GlobalReadingStats::displayLongestReadingStreak() const {
  return std::max(longestReadingStreak, computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits));
}
