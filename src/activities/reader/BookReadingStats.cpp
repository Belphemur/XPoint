#include "BookReadingStats.h"

#include <cstdio>

#include <I18n.h>
#include <ReadingStatsStore.h>

namespace {
constexpr uint16_t MAX_PACE_SAMPLE_COUNT = 1000;
}

BookReadingStats BookReadingStats::load(const std::string& cachePath) {
  BookReadingStats stats;
  ReadingStats.loadBook(cachePath, stats);
  return stats;
}

void BookReadingStats::save(const std::string& cachePath) const {
  ReadingStats.saveBook(cachePath, *this);
}

bool BookReadingStats::remove(const std::string& cachePath) {
  return ReadingStats.removeBook(cachePath);
}

void BookReadingStats::recordForwardPageRead(uint32_t seconds) {
  if (seconds == 0) {
    return;
  }
  if (seconds > UINT16_MAX) {
    seconds = UINT16_MAX;
  }
  const uint16_t sample = static_cast<uint16_t>(seconds);
  if (paceSampleCount == 0 || avgSecondsPerForwardPage == 0) {
    avgSecondsPerForwardPage = sample;
    paceSampleCount = 1;
    return;
  }
  const uint32_t weight = paceSampleCount < MAX_PACE_SAMPLE_COUNT ? paceSampleCount : MAX_PACE_SAMPLE_COUNT;
  const uint32_t nextAverage = (static_cast<uint32_t>(avgSecondsPerForwardPage) * weight + sample) / (weight + 1);
  avgSecondsPerForwardPage = static_cast<uint16_t>(nextAverage);
  if (paceSampleCount < MAX_PACE_SAMPLE_COUNT) {
    paceSampleCount++;
  }
}

void BookReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
}

void BookReadingStats::formatDuration(uint32_t seconds, char* buf, size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%luh %lu min", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}
