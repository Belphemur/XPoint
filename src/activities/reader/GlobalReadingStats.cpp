#include "GlobalReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

bool GlobalReadingStats::s_blockDestructiveSave = false;

namespace {
// Binary layout v3 (159 bytes) — byte-compatible with crossink's global_stats.bin:
//   [0]       version (= 3)
//   [1-4]     totalSessions       uint32_t LE
//   [5-8]     totalReadingSeconds uint32_t LE
//   [9-12]    totalPagesTurned    uint32_t LE
//   [13-16]   completedBooks      uint32_t LE
//   [17-32]   timeOfDaySeconds[4] uint32_t LE each
//   [33-60]   dayOfWeekSeconds[7] uint32_t LE each
//   [61-64]   readingHistoryAnchorDay uint32_t LE
//   [65-156]  readingHistoryBits[92]
//   [157-158] longestReadingStreak uint16_t LE
constexpr uint8_t GLOBAL_STATS_VERSION = 3;
constexpr int GLOBAL_STATS_FILE_SIZE = 159;

// /.crosspoint/global_stats.bin aggregates every book; a torn write would lose
// all history, so saves go through tmp -> verify -> rotate .bak -> rename.
constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
constexpr char GLOBAL_STATS_BAK_PATH[] = "/.crosspoint/global_stats.bin.bak";

enum class StatsLoadResult : uint8_t { Ok, Invalid, NewerFormat };

struct StatsLoadOutcome {
  StatsLoadResult result = StatsLoadResult::Invalid;
  uint8_t version = 0;
  size_t fileSize = 0;
};

uint16_t readLe16(const uint8_t* data, const int offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t readLe32(const uint8_t* data, const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void writeLe32(uint8_t* data, const int offset, const uint32_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
  data[offset + 2] = (value >> 16) & 0xFF;
  data[offset + 3] = (value >> 24) & 0xFF;
}

void loadCommonFields(const uint8_t* data, GlobalReadingStats& out) {
  out.totalSessions = readLe32(data, 1);
  out.totalReadingSeconds = readLe32(data, 5);
  out.totalPagesTurned = readLe32(data, 9);
}

void serializeStats(const GlobalReadingStats& stats, uint8_t* data) {
  memset(data, 0, GLOBAL_STATS_FILE_SIZE);
  data[0] = GLOBAL_STATS_VERSION;
  writeLe32(data, 1, stats.totalSessions);
  writeLe32(data, 5, stats.totalReadingSeconds);
  writeLe32(data, 9, stats.totalPagesTurned);
  writeLe32(data, 13, stats.completedBooks);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    writeLe32(data, 17 + static_cast<int>(i) * 4, stats.timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    writeLe32(data, 33 + static_cast<int>(i) * 4, stats.dayOfWeekSeconds[i]);
  }
  writeLe32(data, 61, stats.readingHistoryAnchorDay);
  memcpy(data + 65, stats.readingHistoryBits.data(), stats.readingHistoryBits.size());
  data[157] = stats.longestReadingStreak & 0xFF;
  data[158] = (stats.longestReadingStreak >> 8) & 0xFF;
}

StatsLoadOutcome loadFromOpenFile(HalFile& f, GlobalReadingStats& out) {
  StatsLoadOutcome outcome;
  outcome.fileSize = f.fileSize();

  // A file smaller than the current record cannot plausibly be from a newer
  // format: it is a torn write. Report Invalid so load() falls through to the
  // backup instead of latching the destructive-save guard.
  if (outcome.fileSize < static_cast<size_t>(GLOBAL_STATS_FILE_SIZE)) {
    return outcome;
  }

  uint8_t data[GLOBAL_STATS_FILE_SIZE] = {};
  const int n = f.read(data, GLOBAL_STATS_FILE_SIZE);
  if (n != GLOBAL_STATS_FILE_SIZE) return outcome;
  outcome.version = data[0];

  // A newer build's format must never be clobbered by this one.
  if (outcome.version > GLOBAL_STATS_VERSION) {
    outcome.result = StatsLoadResult::NewerFormat;
    return outcome;
  }

  loadCommonFields(data, out);
  if (outcome.version >= 2) {
    out.completedBooks = readLe32(data, 13);
  }
  if (outcome.version >= 3) {
    for (size_t i = 0; i < out.timeOfDaySeconds.size(); ++i) {
      out.timeOfDaySeconds[i] = readLe32(data, 17 + static_cast<int>(i) * 4);
    }
    for (size_t i = 0; i < out.dayOfWeekSeconds.size(); ++i) {
      out.dayOfWeekSeconds[i] = readLe32(data, 33 + static_cast<int>(i) * 4);
    }
    out.readingHistoryAnchorDay = readLe32(data, 61);
    memcpy(out.readingHistoryBits.data(), data + 65, out.readingHistoryBits.size());
    out.longestReadingStreak = readLe16(data, 157);
  }
  outcome.result = StatsLoadResult::Ok;
  return outcome;
}

bool verifyFileSize(const char* path, const size_t expectedSize) {
  HalFile file;
  if (!Storage.openFileForRead("GSTATS", path, file)) return false;
  const size_t actualSize = file.fileSize();
  file.close();
  return actualSize == expectedSize;
}

bool saveToFile(const GlobalReadingStats& stats, const char* path, const char* backupPath) {
  const std::string tmpPath = std::string(path) + ".tmp";

  HalFile f;
  if (!Storage.openFileForWrite("GSTATS", tmpPath.c_str(), f)) {
    LOG_ERR("GSTATS", "Could not write stats temp file: %s", tmpPath.c_str());
    return false;
  }

  uint8_t data[GLOBAL_STATS_FILE_SIZE];
  serializeStats(stats, data);
  if (f.write(data, GLOBAL_STATS_FILE_SIZE) != GLOBAL_STATS_FILE_SIZE) {
    LOG_ERR("GSTATS", "Short write for stats temp file %s", tmpPath.c_str());
    f.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  f.flush();
  if (!f.sync()) {
    LOG_ERR("GSTATS", "Failed to sync stats temp file: %s", tmpPath.c_str());
    f.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  f.close();

  if (!verifyFileSize(tmpPath.c_str(), GLOBAL_STATS_FILE_SIZE)) {
    LOG_ERR("GSTATS", "Stats temp file has unexpected size: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  // FatFile::rename fails when the destination exists (O_EXCL semantics), so
  // the old files must be removed before each rename.
  if (backupPath != nullptr) {
    if (Storage.exists(backupPath) && !Storage.remove(backupPath)) {
      LOG_ERR("GSTATS", "Could not remove old stats backup: %s", backupPath);
      Storage.remove(tmpPath.c_str());
      return false;
    }
    if (Storage.exists(path) && !Storage.rename(path, backupPath)) {
      LOG_ERR("GSTATS", "Could not rotate stats backup: %s", path);
      Storage.remove(tmpPath.c_str());
      return false;
    }
  } else if (Storage.exists(path) && !Storage.remove(path)) {
    LOG_ERR("GSTATS", "Could not replace stats file: %s", path);
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!Storage.rename(tmpPath.c_str(), path)) {
    LOG_ERR("GSTATS", "Could not replace stats file: %s", path);
    if (backupPath != nullptr && Storage.exists(backupPath) && !Storage.exists(path)) {
      Storage.rename(backupPath, path);
    }
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}
}  // namespace

GlobalReadingStats GlobalReadingStats::load() {
  GlobalReadingStats stats;
  StatsLoadOutcome primary{};
  {
    HalFile f;
    if (Storage.openFileForRead("GSTATS", GLOBAL_STATS_PATH, f)) {
      primary = loadFromOpenFile(f, stats);
      f.close();
    }
  }
  if (primary.result == StatsLoadResult::Ok) return stats;
  if (primary.result == StatsLoadResult::NewerFormat) {
    LOG_ERR("GSTATS", "On-disk stats are from a newer build (v%u, %u bytes); refusing to overwrite", primary.version,
            static_cast<unsigned>(primary.fileSize));
    s_blockDestructiveSave = true;
    return stats;
  }

  StatsLoadOutcome backup{};
  {
    HalFile f;
    if (Storage.openFileForRead("GSTATS", GLOBAL_STATS_BAK_PATH, f)) {
      backup = loadFromOpenFile(f, stats);
      f.close();
    }
  }
  if (backup.result == StatsLoadResult::Ok) {
    LOG_DBG("GSTATS", "Recovered global stats from backup");
    return stats;
  }
  if (backup.result == StatsLoadResult::NewerFormat) {
    LOG_ERR("GSTATS", "Backup stats are from a newer build (v%u, %u bytes); refusing to overwrite", backup.version,
            static_cast<unsigned>(backup.fileSize));
    s_blockDestructiveSave = true;
    return stats;
  }

  LOG_DBG("GSTATS", "Global stats missing or corrupt, starting fresh");
  return stats;
}

void GlobalReadingStats::save() const {
  if (s_blockDestructiveSave) {
    LOG_ERR("GSTATS", "Refusing to overwrite on-disk stats after newer-format file was detected");
    return;
  }
  saveToFile(*this, GLOBAL_STATS_PATH, GLOBAL_STATS_BAK_PATH);
}

bool GlobalReadingStats::resetLocal() {
  // Deliberately bypasses the backup rotation AND the destructive-save guard:
  // this is the explicit "wipe my stats" action.
  return saveToFile(GlobalReadingStats{}, GLOBAL_STATS_PATH, nullptr);
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
  return std::max(longestReadingStreak,
                  computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits));
}
