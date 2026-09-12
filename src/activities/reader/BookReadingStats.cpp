#include "BookReadingStats.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

std::set<std::string> BookReadingStats::s_blockDestructiveSavePaths;

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
//   [108]    lastBookProgressPercent   uint8_t, 0-100 (0xFF = unknown)
//
// v7 (134 bytes) appends the session-duration window (v6 fields unchanged):
//   [109-110] sessionWindow.avg        uint16_t LE, trimmed mean seconds (0 = none)
//   [111-112] sessionWindow.count      uint16_t LE, samples in window (0-10)
//   [113-132] sessionWindow.samples    uint16_t LE each
//   [133]     sessionWindow.pos        uint8_t
//
// v8 (135 bytes) appends the completion-flow flags (v7 fields unchanged):
//   [134]     completionFlags           bit0=achievementPending
//                                       bit1=completionPromptDismissedAtHundred
//
// The record lives INSIDE the book cache dir, so its lifetime matches the
// cache dir exactly (created/deleted/moved with the book — no orphan cleanup,
// no key migration needed; move-to-/read renames the whole dir).
// On the first save of a loaded v5/v6 record, the writer transparently
// upgrades it to the current version in place and removes the legacy file so
// the next load picks up the new file directly. v4 (69 B) and crossink's
// unversioned stats.bin are NOT supported in this build: they predate the
// per-book cache dir convention and have not been seen on shipped devices. A
// v4 file left on a user's SD is silently treated as missing — the loader
// finds no candidate and the book starts a fresh stat record on next save.
constexpr uint8_t STATS_FILE_VERSION = 8;
constexpr int STATS_FILE_SIZE = 135;
constexpr int STATS_FILE_SIZE_V7 = 134;
constexpr int STATS_FILE_SIZE_V6 = 109;
constexpr int STATS_FILE_SIZE_V5 = 73;
constexpr uint8_t STATS_FILE_VERSION_V7 = 7;
constexpr uint8_t STATS_FILE_VERSION_V6 = 6;
constexpr uint8_t STATS_FILE_VERSION_V5 = 5;
constexpr uint8_t FLAG_START_DATE_MANUAL = 1u << 0;
constexpr uint8_t FLAG_FINISHED_DATE_MANUAL = 1u << 1;
constexpr uint8_t FLAG_COMPLETION_ACHIEVEMENT_PENDING = 1u << 0;
constexpr uint8_t FLAG_COMPLETION_PROMPT_DISMISSED_AT_HUNDRED = 1u << 1;

std::string statsFileNameForVersion(const uint8_t version) {
  char buf[16];
  snprintf(buf, sizeof(buf), "stats_v%u.bin", version);
  return std::string(buf);
}

// Recognized book-stat records, newest first. v5-v8 remain recognized so a
// v5 record can still upgrade in one save hop; older formats are not loaded.
std::vector<std::string> openCandidateNames() {
  // STATS_FILE_VERSION + 1 is recognized on load only as a forward-format
  // guard; it is never decoded as statistics data.
  return {statsFileNameForVersion(STATS_FILE_VERSION), statsFileNameForVersion(STATS_FILE_VERSION - 1),
          statsFileNameForVersion(STATS_FILE_VERSION - 2), statsFileNameForVersion(STATS_FILE_VERSION - 3),
          statsFileNameForVersion(STATS_FILE_VERSION + 1)};
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
  if (n != STATS_FILE_SIZE_V5 || data[0] != STATS_FILE_VERSION_V5) return false;
  readV5Fields(data, stats);
  return true;
}

// Reads the v6-only trailing fields and normalizes them: a corrupt count
// or cursor is clamped (samples[pos] must stay in bounds), the average is
// recomputed from the window rather than trusted, and a progress byte above
// 100 (legacy records carry 0 here, and torn writes can carry anything) is
// folded to the unknown sentinel. Version-agnostic: shared by the v6 and v7
// decoders (the v6 fields sit at the same offsets in both layouts).
void readWpmWindow(const uint8_t* data, WpmWindow& wpm, uint8_t& lastBookProgressPercent) {
  wpm.avg = readLe16(data, 73);
  wpm.count = static_cast<uint8_t>(readLe16(data, 75));
  for (size_t i = 0; i < wpm.samples.size(); ++i) {
    wpm.samples[i] = readLe16(data, 77 + static_cast<int>(i) * 2);
  }
  wpm.pos = data[107];
  wpm.normalize();
  const uint8_t rawProgress = data[108];
  lastBookProgressPercent = (rawProgress <= 100) ? rawProgress : UNKNOWN_BOOK_PROGRESS_PERCENT;
}

// Reads the v7-only trailing session window and normalizes it (same
// untrusted-data rules as the WPM window).
void readSessionWindow(const uint8_t* data, SessionWindow& sessionWindow) {
  sessionWindow.avg = readLe16(data, 109);
  sessionWindow.count = static_cast<uint8_t>(readLe16(data, 111));
  for (size_t i = 0; i < sessionWindow.samples.size(); ++i) {
    sessionWindow.samples[i] = readLe16(data, 113 + static_cast<int>(i) * 2);
  }
  sessionWindow.pos = data[133];
  sessionWindow.normalize();
}

// Reads the v8-only completion flags. Unknown bits are ignored so a future
// compatible extension can reuse the byte without failing this decode.
void readCompletionFlags(const uint8_t flags, BookReadingStats& stats) {
  stats.completionAchievementPending = (flags & FLAG_COMPLETION_ACHIEVEMENT_PENDING) != 0;
  stats.completionPromptDismissedAtHundred = (flags & FLAG_COMPLETION_PROMPT_DISMISSED_AT_HUNDRED) != 0;
}

// Decodes a v6 record (109 bytes = v5 plus the WPM window). Returns false on
// size/version mismatch.
bool decodeV6(const uint8_t* data, const int n, BookReadingStats& stats) {
  if (n != STATS_FILE_SIZE_V6 || data[0] != STATS_FILE_VERSION_V6) return false;
  // v5 fields are a prefix of the v6 record (same byte offsets 1-72), so the
  // v5 layout can be parsed directly without re-checking the version byte.
  readV5Fields(data, stats);
  readWpmWindow(data, stats.wpm, stats.lastBookProgressPercent);
  return true;
}

// Decodes a v7 record (134 bytes = v6 plus the session window). Returns false
// on size/version mismatch.
bool decodeV7(const uint8_t* data, const int n, BookReadingStats& stats) {
  if (n != STATS_FILE_SIZE_V7 || data[0] != STATS_FILE_VERSION_V7) return false;
  // The v6 fields are a byte-identical prefix of the v7 record, so the v6
  // layout is parsed directly without re-checking the version byte. Do NOT
  // route this through decodeV6 — it re-checks the version byte and would
  // reject a v7 record.
  readV5Fields(data, stats);
  readWpmWindow(data, stats.wpm, stats.lastBookProgressPercent);
  readSessionWindow(data, stats.sessionWindow);
  return true;
}

// Decodes a v8 record (135 bytes = v7 plus the completion flags). Returns
// false on size/version mismatch.
bool decodeV8(const uint8_t* data, const int n, BookReadingStats& stats) {
  if (n != STATS_FILE_SIZE || data[0] != STATS_FILE_VERSION) return false;
  // Same prefix rule as decodeV7: parse the shared fields directly and only
  // check the v8 version/size here.
  readV5Fields(data, stats);
  readWpmWindow(data, stats.wpm, stats.lastBookProgressPercent);
  readSessionWindow(data, stats.sessionWindow);
  readCompletionFlags(data[134], stats);
  return true;
}
}  // namespace

BookReadingStats BookReadingStats::load(const std::string& cachePath) {
  // Try each candidate in order and stop at the first one that decodes. A
  // corrupt current-version file must not shadow a valid older record. No
  // rename happens here — the next save() writes the current version in place;
  // the LOG_DBG lines only record which legacy source was picked up.
  const std::string forwardName = statsFileNameForVersion(STATS_FILE_VERSION + 1);
  for (const std::string& name : openCandidateNames()) {
    HalFile f;
    if (!Storage.openFileForRead("STATS", cachePath + "/" + name, f)) continue;
    uint8_t data[STATS_FILE_SIZE] = {};
    const int n = f.read(data, STATS_FILE_SIZE);
    f.close();

    // A version beyond this build means a forward firmware owns this book's
    // history. Only the forward-format candidate latches the destructive-save
    // guard: a corrupt current/legacy record with a garbage version byte is
    // skipped by the decoders, never fatal.
    if (name == forwardName && n >= STATS_FILE_SIZE && data[0] > STATS_FILE_VERSION) {
      LOG_ERR("STATS", "On-disk book stats are from a newer build (v%u, %d bytes); refusing to overwrite", data[0], n);
      s_blockDestructiveSavePaths.insert(cachePath);
      return BookReadingStats{};
    }

    BookReadingStats candidate;
    if (decodeV8(data, n, candidate)) return candidate;
    if (decodeV7(data, n, candidate)) {
      LOG_DBG("STATS", "Loaded %s (older version); next save writes v%u", name.c_str(), STATS_FILE_VERSION);
      return candidate;
    }
    if (decodeV6(data, n, candidate)) {
      LOG_DBG("STATS", "Loaded %s (older version); next save writes v%u", name.c_str(), STATS_FILE_VERSION);
      return candidate;
    }
    if (decodeV5(data, n, candidate)) {
      LOG_DBG("STATS", "Loaded %s (older version); next save writes v%u", name.c_str(), STATS_FILE_VERSION);
      return candidate;
    }
  }
  LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
  return BookReadingStats{};
}

void BookReadingStats::save(const std::string& cachePath) const {
  if (s_blockDestructiveSavePaths.count(cachePath) != 0) {
    LOG_ERR("STATS", "Refusing to overwrite on-disk book stats after newer-format file was detected");
    return;
  }
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
  // bytes 73-107; these slots stay zero to keep the v5 field prefix stable
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
  data[108] = lastBookProgressPercent;
  writeLe16(data, 109, sessionWindow.avg);
  writeLe16(data, 111, sessionWindow.count);
  for (size_t i = 0; i < sessionWindow.samples.size(); ++i) {
    writeLe16(data, 113 + static_cast<int>(i) * 2, sessionWindow.samples[i]);
  }
  data[133] = sessionWindow.pos;
  data[134] = (completionAchievementPending ? FLAG_COMPLETION_ACHIEVEMENT_PENDING : 0u) |
              (completionPromptDismissedAtHundred ? FLAG_COMPLETION_PROMPT_DISMISSED_AT_HUNDRED : 0u);
  const size_t written = f.write(data, STATS_FILE_SIZE);
  if (written != STATS_FILE_SIZE) {
    // Do NOT delete the legacy files — the v8 write didn't land, and the
    // v5/v6/v7 record is the only copy of the user's history. The next save
    // will retry; until it succeeds the loader still finds the legacy file
    // and decodes it. A short write here is a serious condition (SD
    // error) that the LOG_ERR makes visible; silently destroying the
    // legacy on top of that would be unrecoverable data loss.
    LOG_ERR("STATS", "Short write for %s: %u of %u bytes", statsFileName.c_str(), static_cast<unsigned>(written),
            static_cast<unsigned>(STATS_FILE_SIZE));
    f.close();
    return;
  }
  f.close();

  // One-time v8 migration: delete every still-recognized legacy record now
  // that the upgraded data is safely on disk. A v5 record upgrades directly
  // to v8 in one save, so all three legacy files are covered.
  for (const int legacyVersion : {STATS_FILE_VERSION - 1, STATS_FILE_VERSION - 2, STATS_FILE_VERSION - 3}) {
    const std::string legacyPath = cachePath + "/" + statsFileNameForVersion(static_cast<uint8_t>(legacyVersion));
    if (Storage.exists(legacyPath.c_str())) {
      Storage.remove(legacyPath.c_str());
      LOG_DBG("STATS", "Migrated %s -> %s", legacyPath.c_str(), statsFileName.c_str());
    }
  }
}

bool BookReadingStats::remove(const std::string& cachePath) {
  bool ok = true;
  // Remove the current record plus all still-recognized legacy versions so
  // a later load cannot resurrect old data. Very old v4 / crossink
  // unversioned files, if any, are not touched here — they are no longer
  // loaded and will simply be left in the cache dir until the next manual
  // cleanup.
  const std::string names[] = {
      statsFileNameForVersion(STATS_FILE_VERSION), statsFileNameForVersion(STATS_FILE_VERSION - 1),
      statsFileNameForVersion(STATS_FILE_VERSION - 2), statsFileNameForVersion(STATS_FILE_VERSION - 3)};
  for (const std::string& name : names) {
    const std::string path = cachePath + "/" + name;
    if (!Storage.exists(path.c_str())) continue;
    if (!Storage.remove(path.c_str())) {
      LOG_ERR("STATS", "Could not delete %s", name.c_str());
      ok = false;
    }
  }
  // Clear the latch only when no forward-format record remains either: while
  // stats_v9.bin exists, save() must stay blocked — a fresh v8 record would
  // shadow it (load() tries v8 before v9) and let this build overwrite data a
  // newer firmware owns.
  const std::string forwardPath = cachePath + "/" + statsFileNameForVersion(STATS_FILE_VERSION + 1);
  if (ok && !Storage.exists(forwardPath.c_str())) s_blockDestructiveSavePaths.erase(cachePath);
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

void BookReadingStats::recordSession(uint32_t seconds) { sessionWindow.record(seconds); }

void BookReadingStats::clearWpmStats() {
  // Zeros both windows ("clear reading speed" also resets the session
  // average). Sessions, totals, dates, and bucket history are kept — the
  // clear action must not erase the user's reading history.
  wpm.clear();
  sessionWindow.clear();
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
