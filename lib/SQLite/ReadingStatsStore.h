#pragma once

#include <cstdint>
#include <string>

struct GlobalReadingStats;
struct BookReadingStats;

// Reading-stats persistence backed by SQLite (schema documented in
// docs/design/reading-stats-sqlite-integration.md, section 4).
//
// A single lazily-opened sqlite3 handle lives here and is only ever used from
// the main/reader task (single-connection invariant). All file I/O goes
// through the "hal" VFS so it stays serialized under the storage mutex on the
// device, and through POSIX files on the host for the native test suite.
//
// journal_mode=DELETE + synchronous=NORMAL (no WAL) is the chosen durability
// trade-off, per design decision D3. The database is a single self-contained
// .db file.
class ReadingStatsStore {
 public:
  static ReadingStatsStore& getInstance();

  ReadingStatsStore(const ReadingStatsStore&) = delete;
  ReadingStatsStore& operator=(const ReadingStatsStore&) = delete;

  // Opens the database at `dbPath` (default /.crosspoint/reading_stats.db),
  // applies PRAGMAs, verifies integrity and creates the schema if needed.
  // Idempotent: safe to call when already open. Returns false on failure.
  bool begin(const char* dbPath = "/.crosspoint/reading_stats.db");
  void close();
  bool isReady() const;

  bool loadGlobal(GlobalReadingStats& out);
  bool saveGlobal(const GlobalReadingStats& stats);

  bool loadBook(const std::string& bookId, BookReadingStats& out);
  bool saveBook(const std::string& bookId, const BookReadingStats& stats);
  bool removeBook(const std::string& bookId);

  // Re-key a book row after the EPUB cache directory is renamed. Returns true
  // if the old key existed and was moved, false otherwise (including no row).
  bool migrateBookKey(const std::string& oldBookId, const std::string& newBookId);

  // Appends a row to reading_sessions (local-only, never synced).
  bool recordSession(const std::string& bookId, int64_t startEpoch, int64_t durationSec, int32_t pagesTurned);

 private:
  ReadingStatsStore();
  ~ReadingStatsStore();

  bool ensureOpen();
  bool runPragmas();
  bool createSchema();
  bool verifyIntegrity();

  void* db_ = nullptr;  // sqlite3*
  std::string dbPath_;
};

#define ReadingStats ReadingStatsStore::getInstance()
