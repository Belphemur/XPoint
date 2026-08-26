#include "BookReadingStats.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

namespace {
// Binary layout v5 (73 bytes) — byte-compatible with crossink's stats_v5.bin:
//   [0]      version (= 5)
//   [1-2]    sessionCount              uint16_t LE
//   [3-6]    totalReadingSeconds       uint32_t LE
//   [7-10]   totalPagesTurned          uint32_t LE
//   [11]     isCompleted               uint8_t
//   [12-13]  avgSecondsPerForwardPage  uint16_t LE
//   [14-15]  paceSampleCount           uint16_t LE
//   [16]     flags bit0=startDateManual bit1=finishedDateManual
//   [17-18]  startDate.year            uint16_t LE
//   [19]     startDate.month           uint8_t
//   [20]     startDate.day             uint8_t
//   [21-22]  finishedDate.year         uint16_t LE
//   [23]     finishedDate.month        uint8_t
//   [24]     finishedDate.day          uint8_t
//   [25-40]  timeOfDaySeconds[4]       uint32_t LE each
//   [41-68]  dayOfWeekSeconds[7]       uint32_t LE each
//   [69-72]  estimatedTimeLeftSeconds  uint32_t LE, 0 means unavailable
//
// The record lives INSIDE the book cache dir, so its lifetime matches the
// cache dir exactly (created/deleted/moved with the book — no orphan cleanup,
// no key migration needed; move-to-/read renames the whole dir).
constexpr uint8_t STATS_FILE_VERSION = 5;
constexpr int STATS_FILE_SIZE = 73;
constexpr int STATS_FILE_SIZE_V4 = 69;
constexpr size_t MAX_PACE_SAMPLE_COUNT = 1000;
constexpr uint8_t FLAG_START_DATE_MANUAL = 1u << 0;
constexpr uint8_t FLAG_FINISHED_DATE_MANUAL = 1u << 1;
constexpr const char* LEGACY_STATS_FILE_NAME = "stats.bin";

std::string statsFileNameForVersion(const uint8_t version) {
  char buf[16];
  snprintf(buf, sizeof(buf), "stats_v%u.bin", version);
  return std::string(buf);
}

// Current file first, then the previous versioned name (for future bumps),
// then crossink's original unversioned stats.bin. The caller validates each
// opened candidate; openCandidateNames() just enumerates the names in order.
std::vector<std::string> openCandidateNames() {
  return {statsFileNameForVersion(STATS_FILE_VERSION), statsFileNameForVersion(STATS_FILE_VERSION - 1),
          LEGACY_STATS_FILE_NAME};
}

uint16_t readLe16(const uint8_t* data, const int offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t readLe32(const uint8_t* data, const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void writeLe16(uint8_t* data, const int offset, const uint16_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
}

void writeLe32(uint8_t* data, const int offset, const uint32_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
  data[offset + 2] = (value >> 16) & 0xFF;
  data[offset + 3] = (value >> 24) & 0xFF;
}

void readCommonStats(const uint8_t* data, BookReadingStats& stats) {
  stats.sessionCount = readLe16(data, 1);
  stats.totalReadingSeconds = readLe32(data, 3);
  stats.totalPagesTurned = readLe32(data, 7);
}

ReadingStatsDate readDate(const uint8_t* data, const int offset) {
  ReadingStatsDate date;
  date.year = readLe16(data, offset);
  date.month = data[offset + 2];
  date.day = data[offset + 3];
  if (!date.isValid()) {
    date.clear();
  }
  return date;
}
// Decodes a v4 record (69 bytes, same layout as v5 without the trailing
// estimatedTimeLeftSeconds). Returns false if size/version don't match v4.
bool decodeV4(const uint8_t* data, const int n, BookReadingStats& stats) {
  if (n != STATS_FILE_SIZE_V4 || data[0] != STATS_FILE_VERSION - 1) return false;
  readCommonStats(data, stats);
  stats.isCompleted = data[11] != 0;
  stats.avgSecondsPerForwardPage = readLe16(data, 12);
  stats.paceSampleCount = readLe16(data, 14);
  const uint8_t flags = data[16];
  stats.startDateManual = (flags & FLAG_START_DATE_MANUAL) != 0;
  stats.finishedDateManual = (flags & FLAG_FINISHED_DATE_MANUAL) != 0;
  stats.startDate = readDate(data, 17);
  stats.finishedDate = readDate(data, 21);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    stats.timeOfDaySeconds[i] = readLe32(data, 25 + static_cast<int>(i) * 4);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    stats.dayOfWeekSeconds[i] = readLe32(data, 41 + static_cast<int>(i) * 4);
  }
  return true;
}

// Decodes a v5 record (73 bytes). Returns false on size/version mismatch.
bool decodeV5(const uint8_t* data, const int n, BookReadingStats& stats) {
  if (n != STATS_FILE_SIZE || data[0] != STATS_FILE_VERSION) return false;
  readCommonStats(data, stats);
  stats.isCompleted = data[11] != 0;
  stats.avgSecondsPerForwardPage = readLe16(data, 12);
  stats.paceSampleCount = readLe16(data, 14);
  const uint8_t flags = data[16];
  stats.startDateManual = (flags & FLAG_START_DATE_MANUAL) != 0;
  stats.finishedDateManual = (flags & FLAG_FINISHED_DATE_MANUAL) != 0;
  stats.startDate = readDate(data, 17);
  stats.finishedDate = readDate(data, 21);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    stats.timeOfDaySeconds[i] = readLe32(data, 25 + static_cast<int>(i) * 4);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    stats.dayOfWeekSeconds[i] = readLe32(data, 41 + static_cast<int>(i) * 4);
  }
  stats.estimatedTimeLeftSeconds = readLe32(data, 69);
  return true;
}
}  // namespace

BookReadingStats BookReadingStats::load(const std::string& cachePath) {
  // Try each candidate in order and stop at the first one that decodes. A
  // corrupt current-version file must not shadow a valid older record. No
  // rename happens here — the next save() writes the current version in place;
  // the LOG_DBG lines only record which legacy source was picked up.
  for (const std::string& name : openCandidateNames()) {
    HalFile f;
    if (!Storage.openFileForRead("STATS", cachePath + "/" + name, f)) continue;
    uint8_t data[STATS_FILE_SIZE] = {};
    const int n = f.read(data, STATS_FILE_SIZE);
    f.close();

    BookReadingStats candidate;
    if (decodeV5(data, n, candidate)) return candidate;
    if (decodeV4(data, n, candidate)) {
      LOG_DBG("STATS", "Loaded %s (older version); next save writes v%u", name.c_str(), STATS_FILE_VERSION);
      return candidate;
    }
  }
  LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
  return BookReadingStats{};
}

void BookReadingStats::save(const std::string& cachePath) const {
  const std::string statsFileName = statsFileNameForVersion(STATS_FILE_VERSION);
  HalFile f;
  if (!Storage.openFileForWrite("STATS", cachePath + "/" + statsFileName, f)) {
    LOG_ERR("STATS", "Could not write %s", statsFileName.c_str());
    return;
  }
  // Single sequential write of one fixed-size record — the access pattern this
  // SD stack (SdFat over SDMMC) handles reliably. Torn writes self-heal: the
  // loader rejects short/garbage records via the (size, version) check.
  uint8_t data[STATS_FILE_SIZE];
  memset(data, 0, sizeof(data));
  data[0] = STATS_FILE_VERSION;
  writeLe16(data, 1, sessionCount);
  writeLe32(data, 3, totalReadingSeconds);
  writeLe32(data, 7, totalPagesTurned);
  data[11] = isCompleted ? 1 : 0;
  writeLe16(data, 12, avgSecondsPerForwardPage);
  writeLe16(data, 14, paceSampleCount);
  data[16] = (startDateManual ? FLAG_START_DATE_MANUAL : 0u) | (finishedDateManual ? FLAG_FINISHED_DATE_MANUAL : 0u);
  writeLe16(data, 17, startDate.isValid() ? startDate.year : 0);
  data[19] = startDate.isValid() ? startDate.month : 0;
  data[20] = startDate.isValid() ? startDate.day : 0;
  writeLe16(data, 21, finishedDate.isValid() ? finishedDate.year : 0);
  data[23] = finishedDate.isValid() ? finishedDate.month : 0;
  data[24] = finishedDate.isValid() ? finishedDate.day : 0;
  for (size_t i = 0; i < timeOfDaySeconds.size(); ++i) {
    writeLe32(data, 25 + static_cast<int>(i) * 4, timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < dayOfWeekSeconds.size(); ++i) {
    writeLe32(data, 41 + static_cast<int>(i) * 4, dayOfWeekSeconds[i]);
  }
  writeLe32(data, 69, estimatedTimeLeftSeconds);
  if (f.write(data, STATS_FILE_SIZE) != STATS_FILE_SIZE) {
    LOG_ERR("STATS", "Short write for %s", statsFileName.c_str());
  }
  f.close();
}

bool BookReadingStats::remove(const std::string& cachePath) {
  bool ok = true;
  // Remove current + fallback names so a later load cannot resurrect old data.
  const std::string names[] = {statsFileNameForVersion(STATS_FILE_VERSION),
                               statsFileNameForVersion(STATS_FILE_VERSION - 1), LEGACY_STATS_FILE_NAME};
  for (const std::string& name : names) {
    const std::string path = cachePath + "/" + name;
    if (!Storage.exists(path.c_str())) continue;
    if (!Storage.remove(path.c_str())) {
      LOG_ERR("STATS", "Could not delete %s", name.c_str());
      ok = false;
    }
  }
  return ok;
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
    snprintf(buf, len, tr(STR_STATS_DURATION_MIN), static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, tr(STR_STATS_DURATION_HM), static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes));
  }
}
