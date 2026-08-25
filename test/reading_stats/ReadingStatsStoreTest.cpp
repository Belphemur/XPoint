#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingStatsStore.h"
#include "ReadingStatsUtils.h"

namespace {

std::string makeTempDbPath() {
  char tmpl[] = "/tmp/crosspoint_rstats_XXXXXX";
  const int fd = ::mkstemp(tmpl);
  if (fd >= 0) ::close(fd);
  return std::string(tmpl);
}

bool fileExists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

void removeIfExists(const std::string& path) { ::remove(path.c_str()); }

void cleanupDb(const std::string& dbPath) {
  removeIfExists(dbPath);
  removeIfExists(dbPath + "-wal");
  removeIfExists(dbPath + "-shm");
  removeIfExists(dbPath + "-journal");
}

ReadingStatsDateTime makeDateTime(uint16_t year, uint8_t month, uint8_t day, uint8_t hour) {
  ReadingStatsDateTime dt;
  dt.date.year = year;
  dt.date.month = month;
  dt.date.day = day;
  dt.hour = hour;
  dt.minute = 0;
  dt.second = 0;
  return dt;
}

}  // namespace

class ReadingStatsStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dbPath_ = makeTempDbPath();
    cleanupDb(dbPath_);
    ReadingStatsStore::getInstance().close();
  }
  void TearDown() override {
    ReadingStatsStore::getInstance().close();
    cleanupDb(dbPath_);
  }
  std::string dbPath_;
};

TEST_F(ReadingStatsStoreTest, GlobalRoundTrip) {
  ReadingStatsStore& store = ReadingStatsStore::getInstance();
  ASSERT_TRUE(store.begin(dbPath_.c_str()));
  ASSERT_TRUE(store.isReady());

  GlobalReadingStats in;
  in.totalSessions = 42;
  in.totalReadingSeconds = 123456;
  in.totalPagesTurned = 987;
  in.completedBooks = 3;
  in.timeOfDaySeconds = {10, 20, 30, 40};
  in.dayOfWeekSeconds = {1, 2, 3, 4, 5, 6, 7};
  in.readingHistoryAnchorDay = 9000;
  for (size_t i = 0; i < in.readingHistoryBits.size(); ++i) {
    in.readingHistoryBits[i] = static_cast<uint8_t>(i * 3);
  }
  in.longestReadingStreak = 17;
  ASSERT_TRUE(store.saveGlobal(in));

  GlobalReadingStats out;
  ASSERT_TRUE(store.loadGlobal(out));
  EXPECT_EQ(out.totalSessions, in.totalSessions);
  EXPECT_EQ(out.totalReadingSeconds, in.totalReadingSeconds);
  EXPECT_EQ(out.totalPagesTurned, in.totalPagesTurned);
  EXPECT_EQ(out.completedBooks, in.completedBooks);
  EXPECT_EQ(out.timeOfDaySeconds, in.timeOfDaySeconds);
  EXPECT_EQ(out.dayOfWeekSeconds, in.dayOfWeekSeconds);
  EXPECT_EQ(out.readingHistoryAnchorDay, in.readingHistoryAnchorDay);
  EXPECT_EQ(out.readingHistoryBits, in.readingHistoryBits);
  EXPECT_EQ(out.longestReadingStreak, in.longestReadingStreak);
}

TEST_F(ReadingStatsStoreTest, BookRoundTripAndRemove) {
  ReadingStatsStore& store = ReadingStatsStore::getInstance();
  ASSERT_TRUE(store.begin(dbPath_.c_str()));

  const std::string bookId = "/.crosspoint/epub_12345";
  BookReadingStats in;
  in.sessionCount = 7;
  in.totalReadingSeconds = 3600 * 2 + 30;
  in.totalPagesTurned = 250;
  in.isCompleted = true;
  in.avgSecondsPerForwardPage = 33;
  in.paceSampleCount = 100;
  in.estimatedTimeLeftSeconds = 5555;
  in.startDateManual = true;
  in.finishedDateManual = true;
  in.startDate = ReadingStatsDate{2024, 1, 15};
  in.finishedDate = ReadingStatsDate{2024, 2, 20};
  in.timeOfDaySeconds = {5, 15, 25, 35};
  in.dayOfWeekSeconds = {2, 4, 6, 8, 10, 12, 14};
  ASSERT_TRUE(store.saveBook(bookId, in));

  BookReadingStats out;
  ASSERT_TRUE(store.loadBook(bookId, out));
  EXPECT_EQ(out.sessionCount, in.sessionCount);
  EXPECT_EQ(out.totalReadingSeconds, in.totalReadingSeconds);
  EXPECT_EQ(out.totalPagesTurned, in.totalPagesTurned);
  EXPECT_EQ(out.isCompleted, in.isCompleted);
  EXPECT_EQ(out.avgSecondsPerForwardPage, in.avgSecondsPerForwardPage);
  EXPECT_EQ(out.paceSampleCount, in.paceSampleCount);
  EXPECT_EQ(out.estimatedTimeLeftSeconds, in.estimatedTimeLeftSeconds);
  EXPECT_EQ(out.startDateManual, in.startDateManual);
  EXPECT_EQ(out.finishedDateManual, in.finishedDateManual);
  EXPECT_EQ(out.startDate.year, in.startDate.year);
  EXPECT_EQ(out.startDate.month, in.startDate.month);
  EXPECT_EQ(out.startDate.day, in.startDate.day);
  EXPECT_EQ(out.finishedDate.year, in.finishedDate.year);
  EXPECT_EQ(out.finishedDate.month, in.finishedDate.month);
  EXPECT_EQ(out.finishedDate.day, in.finishedDate.day);
  EXPECT_EQ(out.timeOfDaySeconds, in.timeOfDaySeconds);
  EXPECT_EQ(out.dayOfWeekSeconds, in.dayOfWeekSeconds);

  ASSERT_TRUE(store.removeBook(bookId));
  BookReadingStats gone;
  EXPECT_FALSE(store.loadBook(bookId, gone));
}

TEST_F(ReadingStatsStoreTest, NoWalOrShmFilesAfterCommit) {
  ReadingStatsStore& store = ReadingStatsStore::getInstance();
  ASSERT_TRUE(store.begin(dbPath_.c_str()));

  GlobalReadingStats g;
  g.totalReadingSeconds = 60;
  ASSERT_TRUE(store.saveGlobal(g));

  BookReadingStats b;
  b.totalReadingSeconds = 120;
  ASSERT_TRUE(store.saveBook("/.crosspoint/epub_abc", b));

  store.close();

  EXPECT_TRUE(fileExists(dbPath_));
  EXPECT_FALSE(fileExists(std::string(dbPath_) + "-wal"));
  EXPECT_FALSE(fileExists(std::string(dbPath_) + "-shm"));
  EXPECT_FALSE(fileExists(std::string(dbPath_) + "-journal"));
}

TEST_F(ReadingStatsStoreTest, RecordReadingSpanUpdatesBucketsAndStreak) {
  ReadingStatsStore& store = ReadingStatsStore::getInstance();
  ASSERT_TRUE(store.begin(dbPath_.c_str()));

  GlobalReadingStats g;
  // A 3600s session starting at 10:00 (morning bucket) on a known date.
  g.recordReadingSpan(makeDateTime(2024, 6, 15, 10), 3600);

  const auto morning = static_cast<size_t>(ReadingTimeBucket::Morning);
  EXPECT_EQ(g.timeOfDaySeconds[morning], 3600u);
  EXPECT_GT(g.displayLongestReadingStreak(), 0u);

  ASSERT_TRUE(store.saveGlobal(g));
  GlobalReadingStats out;
  ASSERT_TRUE(store.loadGlobal(out));
  EXPECT_EQ(out.timeOfDaySeconds[morning], 3600u);
  EXPECT_EQ(out.displayLongestReadingStreak(), g.displayLongestReadingStreak());
}

TEST_F(ReadingStatsStoreTest, BookDurationFormatting) {
  char buf[32];
  BookReadingStats::formatDuration(30, buf, sizeof(buf));
  EXPECT_STREQ(buf, "< 1 min");
  BookReadingStats::formatDuration(90, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1 min");
  BookReadingStats::formatDuration(5400, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1h 30 min");
}

TEST_F(ReadingStatsStoreTest, CorruptDbRecovers) {
  ReadingStatsStore& store = ReadingStatsStore::getInstance();
  ASSERT_TRUE(store.begin(dbPath_.c_str()));

  GlobalReadingStats g;
  g.totalReadingSeconds = 123;
  ASSERT_TRUE(store.saveGlobal(g));
  store.close();

  // Corrupt the database by overwriting the header with garbage.
  {
    std::ofstream out(dbPath_, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out.write("NOT A SQLITE DATABASE", 21);
  }

  ASSERT_TRUE(store.begin(dbPath_.c_str()));
  EXPECT_TRUE(store.isReady());

  // Schema must be usable after recovery.
  BookReadingStats b;
  b.totalReadingSeconds = 456;
  ASSERT_TRUE(store.saveBook("/ corrupted-recovery", b));
  BookReadingStats outStats;
  ASSERT_TRUE(store.loadBook("/ corrupted-recovery", outStats));
  EXPECT_EQ(outStats.totalReadingSeconds, 456u);
}

TEST_F(ReadingStatsStoreTest, IntegrityCheckDetectsCorruption) {
  ReadingStatsStore& store = ReadingStatsStore::getInstance();
  ASSERT_TRUE(store.begin(dbPath_.c_str()));

  GlobalReadingStats g;
  g.totalReadingSeconds = 123;
  ASSERT_TRUE(store.saveGlobal(g));
  store.close();

  // Overwrite the middle of the file to damage a page.
  {
    std::fstream fs(dbPath_, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(fs.is_open());
    fs.seekp(4096, std::ios::beg);
    const char garbage[] = "CORRUPT";
    fs.write(garbage, sizeof(garbage) - 1);
  }

  // The store detects the corruption, removes the file, and recreates it.
  ASSERT_TRUE(store.begin(dbPath_.c_str()));
  EXPECT_TRUE(store.isReady());

  // The corrupted data is gone, but the schema is usable again.
  GlobalReadingStats out;
  EXPECT_FALSE(store.loadGlobal(out));
  ASSERT_TRUE(store.saveGlobal(g));
  ASSERT_TRUE(store.loadGlobal(out));
  EXPECT_EQ(out.totalReadingSeconds, g.totalReadingSeconds);
}

TEST_F(ReadingStatsStoreTest, ReopenRoundTrip) {
  ReadingStatsStore& store = ReadingStatsStore::getInstance();
  ASSERT_TRUE(store.begin(dbPath_.c_str()));

  GlobalReadingStats g;
  g.totalSessions = 5;
  g.totalReadingSeconds = 9999;
  g.totalPagesTurned = 42;
  ASSERT_TRUE(store.saveGlobal(g));

  BookReadingStats b;
  b.sessionCount = 3;
  b.totalReadingSeconds = 1234;
  ASSERT_TRUE(store.saveBook("/ reopen/book", b));

  // Close and reopen with a fresh connection.
  store.close();
  ASSERT_TRUE(store.begin(dbPath_.c_str()));

  GlobalReadingStats gOut;
  ASSERT_TRUE(store.loadGlobal(gOut));
  EXPECT_EQ(gOut.totalSessions, g.totalSessions);
  EXPECT_EQ(gOut.totalReadingSeconds, g.totalReadingSeconds);
  EXPECT_EQ(gOut.totalPagesTurned, g.totalPagesTurned);

  BookReadingStats bOut;
  ASSERT_TRUE(store.loadBook("/ reopen/book", bOut));
  EXPECT_EQ(bOut.sessionCount, b.sessionCount);
  EXPECT_EQ(bOut.totalReadingSeconds, b.totalReadingSeconds);
}

TEST_F(ReadingStatsStoreTest, BookKeyMigration) {
  ReadingStatsStore& store = ReadingStatsStore::getInstance();
  ASSERT_TRUE(store.begin(dbPath_.c_str()));

  const std::string keyA = "/.crosspoint/epub_old";
  const std::string keyB = "/.crosspoint/epub_new";

  BookReadingStats in;
  in.totalReadingSeconds = 7777;
  in.totalPagesTurned = 111;
  ASSERT_TRUE(store.saveBook(keyA, in));

  ASSERT_TRUE(store.migrateBookKey(keyA, keyB));

  BookReadingStats out;
  ASSERT_TRUE(store.loadBook(keyB, out));
  EXPECT_EQ(out.totalReadingSeconds, in.totalReadingSeconds);
  EXPECT_EQ(out.totalPagesTurned, in.totalPagesTurned);

  BookReadingStats gone;
  EXPECT_FALSE(store.loadBook(keyA, gone));
}
