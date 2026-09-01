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
// v6 (109 bytes) appends the reading-speed window (v5 fields unchanged):
//   [73-74]  wpm.avg                   uint16_t LE, trimmed mean WPM (0 = none)
//   [75-76]  wpm.count                 uint16_t LE, samples in window (0-15)
//   [77-106] wpm.samples[15]           uint16_t LE each
//   [107]    wpm.pos                   uint8_t
//   [108]    reserved (0)              uint8_t
//
// The record lives INSIDE the book cache dir, so its lifetime matches the
// cache dir exactly (created/deleted/moved with the book — no orphan cleanup,
// no key migration needed; move-to-/read renames the whole dir).
// On the first save of a loaded v5 record, the writer transparently upgrades
// it to v6 in place and removes the legacy stats_v5.bin so the next load
// picks up the new file directly. v4 (69 bytes) and crossink's unversioned
// stats.bin are no longer recognized — the build expects v5 or newer.
constexpr uint8_t STATS_FILE_VERSION = 6;
constexpr int STATS_FILE_SIZE = 109;
constexpr int STATS_FILE_SIZE_V5 = 73;
constexpr uint8_t FLAG_START_DATE_MANUAL = 1u << 0;
constexpr uint8_t FLAG_FINISHED_DATE_MANUAL = 1u << 1;

std::string statsFileNameForVersion(const uint8_t version) {
  char buf[16];
  snprintf(buf, sizeof(buf), "stats_v%u.bin", version);
  return std::string(buf);
}

// Current file first, then the previous versioned name. v5 is the only
// recognized legacy version; v4 and crossink's unversioned stats.bin are
// not honored (see layout comment).
std::vector<std::string> openCandidateNames() {
  return {statsFileNameForVersion(STATS_FILE_VERSION), statsFileNameForVersion(STATS_FILE_VERSION - 1)};
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
// Reads the v5 layout's bookkeeping fields. The legacy
// {avgSecondsPerForwardPage, paceSampleCount} pair (bytes 12-15) is
// intentionally NOT populated into BookReadingStats — those fields are
// reserved in v6 and reading speed now comes from the WPM window alone.
void readV5Fields(const uint8_t* data, BookReadingStats& stats) {
  readCommonStats(data, stats);
  stats.isCompleted = data[11] != 0;
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
}

// Decodes a v5 record (73 bytes). Returns false on size/version mismatch.
bool decodeV5(const uint8_t* data, const int n, BookReadingStats& stats) {
  if (n != STATS_FILE_SIZE_V5 || data[0] != STATS_FILE_VERSION - 1) return false;
  readV5Fields(data, stats);
  return true;
}

// Reads the v6-only trailing WPM fields and normalizes them: a corrupt count
// or cursor is clamped (samples[pos] must stay in bounds) and the average is
// recomputed from the window rather than trusted.
void readWpmWindow(const uint8_t* data, WpmWindow& wpm) {
  wpm.avg = readLe16(data, 73);
  wpm.count = static_cast<uint8_t>(readLe16(data, 75));
  for (size_t i = 0; i < wpm.samples.size(); ++i) {
    wpm.samples[i] = readLe16(data, 77 + static_cast<int>(i) * 2);
  }
  wpm.pos = data[107];
  wpm.normalize();
}

// Decodes a v6 record (109 bytes = v5 plus the WPM window). Returns false on
// size/version mismatch.
bool decodeV6(const uint8_t* data, const int n, BookReadingStats& stats) {
  if (n != STATS_FILE_SIZE || data[0] != STATS_FILE_VERSION) return false;
  // v5 fields are a prefix of the v6 record (same byte offsets 1-72), so the
  // v5 layout can be parsed directly without re-checking the version byte.
  readV5Fields(data, stats);
  readWpmWindow(data, stats.wpm);
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
    if (decodeV6(data, n, candidate)) return candidate;
    if (decodeV5(data, n, candidate)) {
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
  // Bytes 12-15 are reserved (legacy avgSecondsPerForwardPage /
  // paceSampleCount). Reading speed is now sourced from the WPM window at
  // bytes 73-108; these slots stay zero to keep the v5 field prefix stable
  // without resurrecting the dropped logic. memset(0) above already writes
  // zero here.
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
  writeLe16(data, 73, wpm.avg);
  writeLe16(data, 75, wpm.count);
  for (size_t i = 0; i < wpm.samples.size(); ++i) {
    writeLe16(data, 77 + static_cast<int>(i) * 2, wpm.samples[i]);
  }
  data[107] = wpm.pos;
  if (f.write(data, STATS_FILE_SIZE) != STATS_FILE_SIZE) {
    LOG_ERR("STATS", "Short write for %s", statsFileName.c_str());
  }
  f.close();

  // One-time v5 → v6 migration: if a legacy stats_v5.bin still sits next to
  // the new file (the load path that fed us the in-memory v5 record), delete
  // it now that the upgraded data is safely on disk. The very-old v4 / crossink
  // unversioned files are not migrated — they are not recognized on load and
  // are left in place (the user can clear them via "Delete book stats" if
  // desired).
  const std::string legacyV5Path = cachePath + "/" + statsFileNameForVersion(STATS_FILE_VERSION - 1);
  if (Storage.exists(legacyV5Path.c_str())) {
    Storage.remove(legacyV5Path.c_str());
    LOG_DBG("STATS", "Migrated %s -> %s", statsFileNameForVersion(STATS_FILE_VERSION - 1), statsFileName.c_str());
  }
}

bool BookReadingStats::remove(const std::string& cachePath) {
  bool ok = true;
  // Remove current + the still-recognized v5 fallback so a later load cannot
  // resurrect old data. Very old v4 / crossink unversioned files, if any, are
  // not touched here — they are no longer loaded and will simply be left in
  // the cache dir until the next manual cleanup.
  const std::string names[] = {statsFileNameForVersion(STATS_FILE_VERSION),
                               statsFileNameForVersion(STATS_FILE_VERSION - 1)};
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

void BookReadingStats::recordForwardPageRead(uint32_t seconds, uint16_t wordsOnPage) {
  if (seconds == 0) {
    return;
  }
  // The trimmed-mean WPM window is the sole source of reading speed; the
  // legacy seconds-per-page average is no longer computed or stored.
  wpm.record(seconds, wordsOnPage);
}

void BookReadingStats::clearWpmStats() {
  // Zeros the WPM window only. Sessions, totals, dates, and bucket history
  // are kept — "clear reading speed" should not erase the user's reading
  // history.
  wpm.clear();
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
