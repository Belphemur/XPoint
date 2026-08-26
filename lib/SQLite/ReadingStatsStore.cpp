#include "ReadingStatsStore.h"

#ifdef ARDUINO
#include <Logging.h>
#else
#include <unistd.h>  // ::access, F_OK for removeDbFiles' host branch

#include <cstdio>
#define LOG_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(module, fmt, ...) fprintf(stderr, "[%s] " fmt "\n", module, ##__VA_ARGS__)
#endif

#ifdef READING_STATS_ENABLED

#include <sqlite3.h>

#include <cstring>
#include <functional>

#include "../../src/activities/reader/BookReadingStats.h"
#include "../../src/activities/reader/GlobalReadingStats.h"
#include "../../src/activities/reader/ReadingStatsUtils.h"
#include "SQLiteVfsHal.h"

#ifdef ARDUINO
#include <HalStorage.h>
#endif

namespace {

constexpr const char* kGlobalStatsTable = "global_stats";
constexpr const char* kBookStatsTable = "book_stats";
constexpr const char* kSessionsTable = "reading_sessions";

constexpr int kHistoryBytes = READING_HISTORY_BYTES;

// Runs a SQL statement that returns no rows. Returns false and logs on error.
bool execSql(sqlite3* db, const char* sql) {
  char* errMsg = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    LOG_ERR("STATS", "SQL error: %s (%s)", errMsg ? errMsg : "unknown", sql);
    if (errMsg) sqlite3_free(errMsg);
    return false;
  }
  return true;
}

bool bindU32(sqlite3_stmt* stmt, int idx, uint32_t v) {
  return sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(v)) == SQLITE_OK;
}

uint32_t colU32(sqlite3_stmt* stmt, int idx) { return static_cast<uint32_t>(sqlite3_column_int64(stmt, idx)); }

// Bounded init: one normal attempt plus one clean recreate (see begin()).
constexpr int kMaxInitAttempts = 2;
// Refuse a from-scratch recreate when the card has less than this free: schema
// pages plus the rollback journal need ~tens of KiB, and a nearly-full card
// surfaces as the same opaque SQLITE_IOERR as any other media failure.
constexpr uint64_t kMinFreeBytesForCreate = 64ull * 1024;

// Logs a SQLite error including the extended result code. Plain errmsg
// collapses every SQLITE_IOERR_* variant (WRITE/FSYNC/READ/DELETE/NOMEM...)
// into the same "disk I/O error" string, which made on-device [STATS]
// failures impossible to attribute to a layer; the numeric codes do not.
void logSqliteError(const char* where, void* dbHandle) {
  sqlite3* db = static_cast<sqlite3*>(dbHandle);
  LOG_ERR("STATS", "%s failed: primary=%d extended=%d msg=%s", where, sqlite3_errcode(db), sqlite3_extended_errcode(db),
          sqlite3_errmsg(db));
}

// Removes the database plus its rollback/WAL sidecars (-journal/-wal/-shm).
// Recreation must clear the sidecars too: leaving them behind would hand any
// stale journal to the freshly created database, and the old .db's journal
// blocks are not reclaimed until the sidecar is removed as well.
// Returns true only if every existing file was removed.
bool removeDbFiles(const std::string& dbPath) {
  bool ok = true;
#ifdef ARDUINO
  const char* const suffixes[] = {"", "-journal", "-wal", "-shm"};
  for (const char* suffix : suffixes) {
    const std::string path = dbPath + suffix;
    if (!Storage.exists(path.c_str())) continue;
    if (!Storage.remove(path.c_str())) ok = false;
  }
#else
  const char* const suffixes[] = {"", "-journal", "-wal", "-shm"};
  for (const char* suffix : suffixes) {
    const std::string path = dbPath + suffix;
    if (::access(path.c_str(), F_OK) != 0) continue;
    if (::remove(path.c_str()) != 0) ok = false;
  }
#endif
  return ok;
}

#ifdef ARDUINO
// Logs card usage so "SD full / failing" is distinguishable from a firmware
// bug in the failure log. Error-path only (freeClusterCount scans the FAT).
void logSdSpace(const char* context) {
  const uint64_t total = Storage.sdTotalBytes();
  if (total == 0) {
    LOG_ERR("STATS", "%s: SD capacity unknown", context);
    return;
  }
  const uint64_t used = Storage.sdUsedBytes();
  LOG_ERR("STATS", "%s: SD %llu/%llu KiB used (%llu KiB free)", context, (unsigned long long)(used / 1024),
          (unsigned long long)(total / 1024), (unsigned long long)((total - used) / 1024));
}
#endif

}  // namespace

ReadingStatsStore::ReadingStatsStore() = default;
ReadingStatsStore::~ReadingStatsStore() { close(); }

ReadingStatsStore& ReadingStatsStore::getInstance() {
  static ReadingStatsStore instance;
  return instance;
}

bool ReadingStatsStore::begin(const char* dbPath) {
  if (!readingStatsTrackingEnabled()) {
    return false;
  }
  if (db_ != nullptr) return true;
  dbPath_ = dbPath;

#ifdef ARDUINO
  Storage.ensureDirectoryExists("/.crosspoint");
#endif

  // Register the custom HAL VFS (idempotent on subsequent calls).
  sqlite_vfs_hal::vfs();

  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  for (int attempt = 1; attempt <= kMaxInitAttempts; ++attempt) {
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(dbPath_.c_str(), &db, flags, "hal");
    if (rc != SQLITE_OK) {
      LOG_ERR("STATS", "open failed: primary=%d extended=%d msg=%s", rc, db ? sqlite3_extended_errcode(db) : rc,
              db ? sqlite3_errmsg(db) : "<no db handle>");
      if (db) sqlite3_close(db);
      return false;
    }
    db_ = db;
    // Extended result codes are what distinguish IOERR_WRITE/FSYNC/SEEK...
    // (the whole point of the attribution logging below).
    sqlite3_extended_result_codes(db, 1);

    if (runPragmas() && createSchema() && verifyIntegrity()) {
      LOG_INF("STATS", "opened %s%s", dbPath_.c_str(), attempt > 1 ? " (recreated)" : "");
      return true;
    }

    logSqliteError("init", db_);
    // Tear down ONLY the handle here: close() also clears dbPath_, which
    // would make the retry open "" — SQLite turns an empty filename into a
    // private temp database that "succeeds" while leaving the real (corrupt)
    // file untouched on the card. Observed exactly that on hardware.
    sqlite3_close(static_cast<sqlite3*>(db_));
    db_ = nullptr;
    if (attempt >= kMaxInitAttempts) break;

#ifdef ARDUINO
    logSdSpace("init failed");
    const uint64_t totalBytes = Storage.sdTotalBytes();
    if (totalBytes == 0) return false;  // card gone; recreating cannot help
    const uint64_t freeBytes = totalBytes - Storage.sdUsedBytes();
    if (freeBytes < kMinFreeBytesForCreate) {
      LOG_ERR("STATS", "SD nearly full (%llu KiB free); keeping existing stats db",
              (unsigned long long)(freeBytes / 1024));
      return false;
    }
#endif

    LOG_ERR("STATS", "attempt %d/%d failed; recreating %s", attempt, kMaxInitAttempts, dbPath_.c_str());
    // Sidecars included, so the retry starts from a known-clean state.
    if (!removeDbFiles(dbPath_)) {
      LOG_ERR("STATS", "could not fully clear %s(.db/-journal/-wal/-shm); retry may fail", dbPath_.c_str());
    }
  }
  return false;
}

void ReadingStatsStore::close() {
  if (db_) {
    sqlite3_close(static_cast<sqlite3*>(db_));
    db_ = nullptr;
  }
  dbPath_.clear();
}

bool ReadingStatsStore::isReady() const { return db_ != nullptr; }

bool ReadingStatsStore::ensureOpen() {
  if (db_ != nullptr) return true;
  return begin(dbPath_.empty() ? "/.crosspoint/reading_stats.db" : dbPath_.c_str());
}

bool ReadingStatsStore::runPragmas() {
  sqlite3* db = static_cast<sqlite3*>(db_);
  const char* pragmas[] = {
      "PRAGMA page_size = 4096;",  "PRAGMA journal_mode = DELETE;", "PRAGMA synchronous = NORMAL;",
      "PRAGMA foreign_keys = ON;", "PRAGMA temp_store = MEMORY;",   "PRAGMA mmap_size = 0;",
      "PRAGMA cache_size = -128;", "PRAGMA user_version = 1;",
  };
  for (const char* p : pragmas) {
    if (!execSql(db, p)) return false;
  }
  return true;
}

bool ReadingStatsStore::createSchema() {
  sqlite3* db = static_cast<sqlite3*>(db_);
  const char* globalStats = R"SQL(
CREATE TABLE IF NOT EXISTS global_stats (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  total_sessions INTEGER NOT NULL DEFAULT 0,
  total_reading_seconds INTEGER NOT NULL DEFAULT 0,
  total_pages_turned INTEGER NOT NULL DEFAULT 0,
  completed_books INTEGER NOT NULL DEFAULT 0,
  tod_morning INTEGER NOT NULL DEFAULT 0,
  tod_afternoon INTEGER NOT NULL DEFAULT 0,
  tod_evening INTEGER NOT NULL DEFAULT 0,
  tod_night INTEGER NOT NULL DEFAULT 0,
  dow_mon INTEGER NOT NULL DEFAULT 0,
  dow_tue INTEGER NOT NULL DEFAULT 0,
  dow_wed INTEGER NOT NULL DEFAULT 0,
  dow_thu INTEGER NOT NULL DEFAULT 0,
  dow_fri INTEGER NOT NULL DEFAULT 0,
  dow_sat INTEGER NOT NULL DEFAULT 0,
  dow_sun INTEGER NOT NULL DEFAULT 0,
  reading_history_anchor_day INTEGER NOT NULL DEFAULT 0,
  reading_history_bits BLOB NOT NULL DEFAULT (zeroblob(92)),
  longest_reading_streak INTEGER NOT NULL DEFAULT 0
);
)SQL";
  const char* bookStats = R"SQL(
CREATE TABLE IF NOT EXISTS book_stats (
  book_id TEXT PRIMARY KEY,
  session_count INTEGER NOT NULL DEFAULT 0,
  total_reading_seconds INTEGER NOT NULL DEFAULT 0,
  total_pages_turned INTEGER NOT NULL DEFAULT 0,
  is_completed INTEGER NOT NULL DEFAULT 0,
  avg_seconds_per_forward_page INTEGER NOT NULL DEFAULT 0,
  pace_sample_count INTEGER NOT NULL DEFAULT 0,
  estimated_time_left_seconds INTEGER NOT NULL DEFAULT 0,
  start_date_manual INTEGER NOT NULL DEFAULT 0,
  finished_date_manual INTEGER NOT NULL DEFAULT 0,
  start_date_year INTEGER NOT NULL DEFAULT 0,
  start_date_month INTEGER NOT NULL DEFAULT 0,
  start_date_day INTEGER NOT NULL DEFAULT 0,
  finished_date_year INTEGER NOT NULL DEFAULT 0,
  finished_date_month INTEGER NOT NULL DEFAULT 0,
  finished_date_day INTEGER NOT NULL DEFAULT 0,
  tod_morning INTEGER NOT NULL DEFAULT 0,
  tod_afternoon INTEGER NOT NULL DEFAULT 0,
  tod_evening INTEGER NOT NULL DEFAULT 0,
  tod_night INTEGER NOT NULL DEFAULT 0,
  dow_mon INTEGER NOT NULL DEFAULT 0,
  dow_tue INTEGER NOT NULL DEFAULT 0,
  dow_wed INTEGER NOT NULL DEFAULT 0,
  dow_thu INTEGER NOT NULL DEFAULT 0,
  dow_fri INTEGER NOT NULL DEFAULT 0,
  dow_sat INTEGER NOT NULL DEFAULT 0,
  dow_sun INTEGER NOT NULL DEFAULT 0
);
)SQL";
  const char* sessions = R"SQL(
CREATE TABLE IF NOT EXISTS reading_sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  book_id TEXT NOT NULL REFERENCES book_stats(book_id),
  start_epoch INTEGER NOT NULL DEFAULT 0,
  duration_sec INTEGER NOT NULL DEFAULT 0,
  pages_turned INTEGER NOT NULL DEFAULT 0
);
)SQL";
  const char* index = "CREATE INDEX IF NOT EXISTS idx_sessions_book ON reading_sessions(book_id);";

  return execSql(db, globalStats) && execSql(db, bookStats) && execSql(db, sessions) && execSql(db, index);
}

bool ReadingStatsStore::verifyIntegrity() {
  sqlite3* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, nullptr) != SQLITE_OK) return false;
  bool sawOk = false;
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const auto* txt = sqlite3_column_text(stmt, 0);
    if (!txt || std::strcmp(reinterpret_cast<const char*>(txt), "ok") != 0) {
      sqlite3_finalize(stmt);
      return false;
    }
    sawOk = true;
  }
  sqlite3_finalize(stmt);
  return sawOk && rc == SQLITE_DONE;
}

bool ReadingStatsStore::loadGlobal(GlobalReadingStats& out) {
  if (!ensureOpen()) return false;
  sqlite3* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT total_sessions, total_reading_seconds, total_pages_turned, completed_books, "
      "tod_morning, tod_afternoon, tod_evening, tod_night, "
      "dow_mon, dow_tue, dow_wed, dow_thu, dow_fri, dow_sat, dow_sun, "
      "reading_history_anchor_day, reading_history_bits, longest_reading_streak "
      "FROM global_stats WHERE id = 1;";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return false;
  }
  out = GlobalReadingStats{};
  out.totalSessions = colU32(stmt, 0);
  out.totalReadingSeconds = colU32(stmt, 1);
  out.totalPagesTurned = colU32(stmt, 2);
  out.completedBooks = colU32(stmt, 3);
  out.timeOfDaySeconds[0] = colU32(stmt, 4);
  out.timeOfDaySeconds[1] = colU32(stmt, 5);
  out.timeOfDaySeconds[2] = colU32(stmt, 6);
  out.timeOfDaySeconds[3] = colU32(stmt, 7);
  for (size_t i = 0; i < READING_DAY_OF_WEEK_COUNT; ++i) {
    out.dayOfWeekSeconds[i] = colU32(stmt, static_cast<int>(8 + i));
  }
  out.readingHistoryAnchorDay = colU32(stmt, 15);
  const void* blob = sqlite3_column_blob(stmt, 16);
  const int n = sqlite3_column_bytes(stmt, 16);
  const int copy = n < kHistoryBytes ? n : kHistoryBytes;
  if (blob && copy > 0) memcpy(out.readingHistoryBits.data(), blob, static_cast<size_t>(copy));
  out.longestReadingStreak = static_cast<uint16_t>(colU32(stmt, 17));
  sqlite3_finalize(stmt);
  return true;
}

bool ReadingStatsStore::saveGlobal(const GlobalReadingStats& stats) {
  if (!ensureOpen()) return false;
  sqlite3* db = static_cast<sqlite3*>(db_);
  const char* sql =
      "INSERT INTO global_stats (id, total_sessions, total_reading_seconds, total_pages_turned, completed_books, "
      "tod_morning, tod_afternoon, tod_evening, tod_night, "
      "dow_mon, dow_tue, dow_wed, dow_thu, dow_fri, dow_sat, dow_sun, "
      "reading_history_anchor_day, reading_history_bits, longest_reading_streak) "
      "VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET "
      "total_sessions = excluded.total_sessions, "
      "total_reading_seconds = excluded.total_reading_seconds, "
      "total_pages_turned = excluded.total_pages_turned, "
      "completed_books = excluded.completed_books, "
      "tod_morning = excluded.tod_morning, tod_afternoon = excluded.tod_afternoon, "
      "tod_evening = excluded.tod_evening, tod_night = excluded.tod_night, "
      "dow_mon = excluded.dow_mon, dow_tue = excluded.dow_tue, dow_wed = excluded.dow_wed, "
      "dow_thu = excluded.dow_thu, dow_fri = excluded.dow_fri, dow_sat = excluded.dow_sat, "
      "dow_sun = excluded.dow_sun, "
      "reading_history_anchor_day = excluded.reading_history_anchor_day, "
      "reading_history_bits = excluded.reading_history_bits, "
      "longest_reading_streak = excluded.longest_reading_streak;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  bindU32(stmt, 1, stats.totalSessions);
  bindU32(stmt, 2, stats.totalReadingSeconds);
  bindU32(stmt, 3, stats.totalPagesTurned);
  bindU32(stmt, 4, stats.completedBooks);
  bindU32(stmt, 5, stats.timeOfDaySeconds[0]);
  bindU32(stmt, 6, stats.timeOfDaySeconds[1]);
  bindU32(stmt, 7, stats.timeOfDaySeconds[2]);
  bindU32(stmt, 8, stats.timeOfDaySeconds[3]);
  for (size_t i = 0; i < READING_DAY_OF_WEEK_COUNT; ++i) {
    bindU32(stmt, static_cast<int>(9 + i), stats.dayOfWeekSeconds[i]);
  }
  bindU32(stmt, 16, stats.readingHistoryAnchorDay);
  sqlite3_bind_blob(stmt, 17, stats.readingHistoryBits.data(), kHistoryBytes, SQLITE_TRANSIENT);
  bindU32(stmt, 18, stats.longestReadingStreak);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool ReadingStatsStore::loadBook(const std::string& bookId, BookReadingStats& out) {
  if (!ensureOpen()) return false;
  sqlite3* db = static_cast<sqlite3*>(db_);
  const char* sql =
      "SELECT session_count, total_reading_seconds, total_pages_turned, is_completed, "
      "avg_seconds_per_forward_page, pace_sample_count, estimated_time_left_seconds, "
      "start_date_manual, finished_date_manual, "
      "start_date_year, start_date_month, start_date_day, "
      "finished_date_year, finished_date_month, finished_date_day, "
      "tod_morning, tod_afternoon, tod_evening, tod_night, "
      "dow_mon, dow_tue, dow_wed, dow_thu, dow_fri, dow_sat, dow_sun "
      "FROM book_stats WHERE book_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(stmt, 1, bookId.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return false;
  }
  out = BookReadingStats{};
  out.sessionCount = static_cast<uint16_t>(colU32(stmt, 0));
  out.totalReadingSeconds = colU32(stmt, 1);
  out.totalPagesTurned = colU32(stmt, 2);
  out.isCompleted = sqlite3_column_int(stmt, 3) != 0;
  out.avgSecondsPerForwardPage = static_cast<uint16_t>(colU32(stmt, 4));
  out.paceSampleCount = static_cast<uint16_t>(colU32(stmt, 5));
  out.estimatedTimeLeftSeconds = colU32(stmt, 6);
  out.startDateManual = sqlite3_column_int(stmt, 7) != 0;
  out.finishedDateManual = sqlite3_column_int(stmt, 8) != 0;
  out.startDate.year = static_cast<uint16_t>(colU32(stmt, 9));
  out.startDate.month = static_cast<uint8_t>(colU32(stmt, 10));
  out.startDate.day = static_cast<uint8_t>(colU32(stmt, 11));
  out.finishedDate.year = static_cast<uint16_t>(colU32(stmt, 12));
  out.finishedDate.month = static_cast<uint8_t>(colU32(stmt, 13));
  out.finishedDate.day = static_cast<uint8_t>(colU32(stmt, 14));
  out.timeOfDaySeconds[0] = colU32(stmt, 15);
  out.timeOfDaySeconds[1] = colU32(stmt, 16);
  out.timeOfDaySeconds[2] = colU32(stmt, 17);
  out.timeOfDaySeconds[3] = colU32(stmt, 18);
  for (size_t i = 0; i < READING_DAY_OF_WEEK_COUNT; ++i) {
    out.dayOfWeekSeconds[i] = colU32(stmt, static_cast<int>(19 + i));
  }
  sqlite3_finalize(stmt);
  return true;
}

bool ReadingStatsStore::saveBook(const std::string& bookId, const BookReadingStats& stats) {
  if (!ensureOpen()) return false;
  sqlite3* db = static_cast<sqlite3*>(db_);
  const char* sql =
      "INSERT INTO book_stats (book_id, session_count, total_reading_seconds, total_pages_turned, is_completed, "
      "avg_seconds_per_forward_page, pace_sample_count, estimated_time_left_seconds, "
      "start_date_manual, finished_date_manual, "
      "start_date_year, start_date_month, start_date_day, "
      "finished_date_year, finished_date_month, finished_date_day, "
      "tod_morning, tod_afternoon, tod_evening, tod_night, "
      "dow_mon, dow_tue, dow_wed, dow_thu, dow_fri, dow_sat, dow_sun) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(book_id) DO UPDATE SET "
      "session_count = excluded.session_count, "
      "total_reading_seconds = excluded.total_reading_seconds, "
      "total_pages_turned = excluded.total_pages_turned, "
      "is_completed = excluded.is_completed, "
      "avg_seconds_per_forward_page = excluded.avg_seconds_per_forward_page, "
      "pace_sample_count = excluded.pace_sample_count, "
      "estimated_time_left_seconds = excluded.estimated_time_left_seconds, "
      "start_date_manual = excluded.start_date_manual, "
      "finished_date_manual = excluded.finished_date_manual, "
      "start_date_year = excluded.start_date_year, start_date_month = excluded.start_date_month, "
      "start_date_day = excluded.start_date_day, "
      "finished_date_year = excluded.finished_date_year, finished_date_month = excluded.finished_date_month, "
      "finished_date_day = excluded.finished_date_day, "
      "tod_morning = excluded.tod_morning, tod_afternoon = excluded.tod_afternoon, "
      "tod_evening = excluded.tod_evening, tod_night = excluded.tod_night, "
      "dow_mon = excluded.dow_mon, dow_tue = excluded.dow_tue, dow_wed = excluded.dow_wed, "
      "dow_thu = excluded.dow_thu, dow_fri = excluded.dow_fri, dow_sat = excluded.dow_sat, "
      "dow_sun = excluded.dow_sun;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(stmt, 1, bookId.c_str(), -1, SQLITE_TRANSIENT);
  bindU32(stmt, 2, stats.sessionCount);
  bindU32(stmt, 3, stats.totalReadingSeconds);
  bindU32(stmt, 4, stats.totalPagesTurned);
  sqlite3_bind_int(stmt, 5, stats.isCompleted ? 1 : 0);
  bindU32(stmt, 6, stats.avgSecondsPerForwardPage);
  bindU32(stmt, 7, stats.paceSampleCount);
  bindU32(stmt, 8, stats.estimatedTimeLeftSeconds);
  sqlite3_bind_int(stmt, 9, stats.startDateManual ? 1 : 0);
  sqlite3_bind_int(stmt, 10, stats.finishedDateManual ? 1 : 0);
  bindU32(stmt, 11, stats.startDate.year);
  bindU32(stmt, 12, stats.startDate.month);
  bindU32(stmt, 13, stats.startDate.day);
  bindU32(stmt, 14, stats.finishedDate.year);
  bindU32(stmt, 15, stats.finishedDate.month);
  bindU32(stmt, 16, stats.finishedDate.day);
  bindU32(stmt, 17, stats.timeOfDaySeconds[0]);
  bindU32(stmt, 18, stats.timeOfDaySeconds[1]);
  bindU32(stmt, 19, stats.timeOfDaySeconds[2]);
  bindU32(stmt, 20, stats.timeOfDaySeconds[3]);
  for (size_t i = 0; i < READING_DAY_OF_WEEK_COUNT; ++i) {
    bindU32(stmt, static_cast<int>(21 + i), stats.dayOfWeekSeconds[i]);
  }
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool ReadingStatsStore::removeBook(const std::string& bookId) {
  if (!ensureOpen()) return false;
  sqlite3* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "DELETE FROM book_stats WHERE book_id = ?;", -1, &stmt, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_text(stmt, 1, bookId.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) return false;
  return sqlite3_changes(db) > 0;
}

bool ReadingStatsStore::migrateBookKey(const std::string& oldBookId, const std::string& newBookId) {
  if (oldBookId == newBookId) return true;
  if (!ensureOpen()) return false;

  BookReadingStats stats;
  if (!loadBook(oldBookId, stats)) return false;
  if (!saveBook(newBookId, stats)) return false;

  sqlite3* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "UPDATE reading_sessions SET book_id = ? WHERE book_id = ?;", -1, &stmt, nullptr) !=
      SQLITE_OK)
    return false;
  sqlite3_bind_text(stmt, 1, newBookId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, oldBookId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return removeBook(oldBookId);
}

bool ReadingStatsStore::recordSession(const std::string& bookId, int64_t startEpoch, int64_t durationSec,
                                      int32_t pagesTurned) {
  if (!ensureOpen()) return false;
  sqlite3* db = static_cast<sqlite3*>(db_);
  // Foreign key requires a book_stats row; ensure one exists (empty defaults).
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO book_stats (book_id) VALUES (?);", -1, &stmt, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_text(stmt, 1, bookId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(
          db, "INSERT INTO reading_sessions (book_id, start_epoch, duration_sec, pages_turned) VALUES (?, ?, ?, ?);",
          -1, &stmt, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_text(stmt, 1, bookId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, startEpoch);
  sqlite3_bind_int64(stmt, 3, durationSec);
  sqlite3_bind_int(stmt, 4, pagesTurned);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool ReadingStatsStore::transaction(std::function<bool()> body) {
  if (!ensureOpen()) return false;
  sqlite3* db = static_cast<sqlite3*>(db_);
  if (!execSql(db, "BEGIN;")) return false;
  if (!body()) {
    execSql(db, "ROLLBACK;");
    return false;
  }
  if (!execSql(db, "COMMIT;")) {
    execSql(db, "ROLLBACK;");
    return false;
  }
  return true;
}

#else  // !READING_STATS_ENABLED

// No-op stub: reading stats are compiled out on non-PSRAM devices.
ReadingStatsStore::ReadingStatsStore() = default;
ReadingStatsStore::~ReadingStatsStore() = default;

ReadingStatsStore& ReadingStatsStore::getInstance() {
  static ReadingStatsStore instance;
  return instance;
}

bool ReadingStatsStore::begin(const char*) { return false; }
void ReadingStatsStore::close() {}
bool ReadingStatsStore::isReady() const { return false; }
bool ReadingStatsStore::ensureOpen() { return false; }
bool ReadingStatsStore::runPragmas() { return false; }
bool ReadingStatsStore::createSchema() { return false; }
bool ReadingStatsStore::verifyIntegrity() { return false; }
bool ReadingStatsStore::loadGlobal(GlobalReadingStats&) { return false; }
bool ReadingStatsStore::saveGlobal(const GlobalReadingStats&) { return false; }
bool ReadingStatsStore::loadBook(const std::string&, BookReadingStats&) { return false; }
bool ReadingStatsStore::saveBook(const std::string&, const BookReadingStats&) { return false; }
bool ReadingStatsStore::removeBook(const std::string&) { return false; }
bool ReadingStatsStore::migrateBookKey(const std::string&, const std::string&) { return false; }
bool ReadingStatsStore::recordSession(const std::string&, int64_t, int64_t, int32_t) { return false; }
bool ReadingStatsStore::transaction(std::function<bool()>) { return false; }

#endif  // READING_STATS_ENABLED
