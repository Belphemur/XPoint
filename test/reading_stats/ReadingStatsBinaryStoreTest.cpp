#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "FinishedBooksIndex.h"
#include "GlobalReadingStats.h"
#include "HalStorage.h"

namespace {
constexpr const char* BOOK_DIR = "/.crosspoint/epub_1234";
constexpr const char* GLOBAL_PATH = "/.crosspoint/global_stats.bin";
constexpr const char* GLOBAL_BAK_PATH = "/.crosspoint/global_stats.bin.bak";

std::string statsPath(const char* dir, int version) {
  return std::string(dir) + "/stats_v" + std::to_string(version) + ".bin";
}

std::vector<uint8_t> readFileBytes(const std::string& path) {
  HalFile f;
  if (!Storage.openFileForRead("TEST", path, f)) return {};
  std::vector<uint8_t> out(f.fileSize());
  f.read(out.data(), out.size());
  return out;
}

uint32_t readLe32At(const std::vector<uint8_t>& data, size_t offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void writeLe16At(std::vector<uint8_t>& data, size_t offset, uint16_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
}

void writeLe32At(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
  data[offset + 2] = (value >> 16) & 0xFF;
  data[offset + 3] = (value >> 24) & 0xFF;
}

void writeLe64At(std::vector<uint8_t>& data, size_t offset, uint64_t value) {
  for (size_t i = 0; i < 8; ++i) {
    data[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

uint64_t testPathKey(const std::string& path) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char c : path) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
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

class ReadingStatsBinaryStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.clear();
    FinishedBooksIndex::clearRecentBooksForTest();
    FinishedBooksIndex::resetForwardGuardForTest();
    // Reset per-path forward-format latches left by earlier tests; the storage
    // backing those paths has just been cleared.
    (void)BookReadingStats::remove(BOOK_DIR);
    // Reset the destructive-save latch: loading a missing file leaves it false
    // (load() only latches on a detected newer-format record).
    GlobalReadingStats::load();
  }
};

TEST_F(ReadingStatsBinaryStoreTest, GlobalRoundTrip) {
  GlobalReadingStats g;
  g.totalSessions = 5;
  g.totalReadingSeconds = 9999;
  g.totalPagesTurned = 42;
  g.completedBooks = 2;
  g.timeOfDaySeconds[0] = 100;
  g.dayOfWeekSeconds[6] = 200;
  g.readingHistoryAnchorDay = 20600;
  g.readingHistoryBits[10] = 0xAB;
  g.dailyReadingMinutes[0] = 7;
  g.dailyReadingMinutes[90] = 91;
  g.longestReadingStreak = 7;
  g.wpm.record(60, 220);  // 220 words in 60 s -> 220 WPM
  for (int i = 0; i < 5; ++i) {
    g.recordGlobalSession(600);
  }
  g.save();

  const auto bytes = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(bytes.size(), 407u);
  EXPECT_EQ(bytes[0], 6);  // version byte

  GlobalReadingStats out = GlobalReadingStats::load();
  EXPECT_EQ(out.totalSessions, 5u);
  EXPECT_EQ(out.totalReadingSeconds, 9999u);
  EXPECT_EQ(out.totalPagesTurned, 42u);
  EXPECT_EQ(out.completedBooks, 2u);
  EXPECT_EQ(out.timeOfDaySeconds[0], 100u);
  EXPECT_EQ(out.dayOfWeekSeconds[6], 200u);
  EXPECT_EQ(out.readingHistoryAnchorDay, 20600u);
  EXPECT_EQ(out.readingHistoryBits[10], 0xAB);
  EXPECT_EQ(out.dailyReadingMinutes[0], 7u);
  EXPECT_EQ(out.dailyReadingMinutes[90], 91u);
  EXPECT_EQ(out.longestReadingStreak, 7u);
  EXPECT_EQ(out.wpm.count, 1u);
  EXPECT_EQ(out.wpm.avg, 220u);
  EXPECT_EQ(out.sessionWindow.count, 5u);
  EXPECT_EQ(out.sessionWindow.avg, 600u);
}

TEST_F(ReadingStatsBinaryStoreTest, GlobalBackupRotationAndRecovery) {
  GlobalReadingStats g;
  g.totalSessions = 3;
  g.save();

  // After one save: main exists, no backup yet.
  EXPECT_TRUE(Storage.exists(GLOBAL_PATH));
  EXPECT_FALSE(Storage.exists(GLOBAL_BAK_PATH));

  GlobalReadingStats g2;
  g2.totalSessions = 9;
  g2.save();

  // After second save: old content rotated into .bak.
  EXPECT_TRUE(Storage.exists(GLOBAL_BAK_PATH));
  const auto bak = readFileBytes(GLOBAL_BAK_PATH);
  ASSERT_EQ(bak.size(), 407u);
  EXPECT_EQ(readLe32At(bak, 1), 3u);  // first save's totalSessions
  const auto main = readFileBytes(GLOBAL_PATH);
  EXPECT_EQ(readLe32At(main, 1), 9u);  // second save's totalSessions

  // Third save: rotation must also work when a .bak already exists (the
  // production code removes it first because FatFile::rename refuses existing
  // destinations — this exercises exactly that path).
  GlobalReadingStats g3;
  g3.totalSessions = 12;
  g3.save();
  const auto main3 = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(main3.size(), 407u);
  EXPECT_EQ(readLe32At(main3, 1), 12u);
  const auto bak3 = readFileBytes(GLOBAL_BAK_PATH);
  EXPECT_EQ(readLe32At(bak3, 1), 9u);

  // Corrupt the main file; load must recover from the backup.
  HalFile f;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", GLOBAL_PATH, f));
  const uint8_t garbage[] = {0xFF, 0x99};
  f.write(garbage, sizeof(garbage));

  GlobalReadingStats out = GlobalReadingStats::load();
  EXPECT_EQ(out.totalSessions, 9u);  // from .bak: the last value that was rotated
}

TEST_F(ReadingStatsBinaryStoreTest, GlobalNewerFormatBlocksSaves) {
  std::vector<uint8_t> future(408, 0);
  future[0] = 7;  // one version ahead
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", GLOBAL_PATH, f));
    f.write(future.data(), future.size());
  }

  const GlobalReadingStats out = GlobalReadingStats::load();
  EXPECT_EQ(out.totalSessions, 0u);  // defaults, not garbage
  out.save();

  // The newer-format file must be untouched by the refused save.
  const auto after = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(after.size(), 408u);
  EXPECT_EQ(after[0], 7);
}

TEST_F(ReadingStatsBinaryStoreTest, GlobalResetLocalBypassesNewerFormatGuard) {
  std::vector<uint8_t> future(408, 0);
  future[0] = 7;
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", GLOBAL_PATH, f));
    f.write(future.data(), future.size());
  }
  (void)GlobalReadingStats::load();  // latches the guard

  EXPECT_TRUE(GlobalReadingStats::resetLocal());
  const auto after = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(after.size(), 407u);
  EXPECT_EQ(after[0], 6);
}

TEST_F(ReadingStatsBinaryStoreTest, BookRoundTrip) {
  BookReadingStats b;
  b.sessionCount = 4;
  b.totalReadingSeconds = 3661;
  b.totalPagesTurned = 120;
  b.isCompleted = true;
  b.recordForwardPageRead(30, 220);  // 220 words in 30 s -> 440 WPM sample
  b.startDateManual = true;
  b.startDate = ReadingStatsDate{2026, 8, 26};
  b.finishedDateManual = true;
  b.finishedDate = ReadingStatsDate{2026, 8, 27};
  b.recordReadingSpan(makeDateTime(2026, 8, 26, 8), 600);
  b.recordReadingSpan(makeDateTime(2026, 8, 26, 22), 300);
  b.estimatedTimeLeftSeconds = 5400;
  for (int i = 0; i < 5; ++i) {
    b.recordSession(600);
  }
  b.completionAchievementPending = true;
  b.completionPromptDismissedAtHundred = true;
  b.save(BOOK_DIR);

  const auto bytes = readFileBytes(statsPath(BOOK_DIR, 8));
  ASSERT_EQ(bytes.size(), 135u);
  EXPECT_EQ(bytes[0], 8);       // version byte
  EXPECT_EQ(bytes[134], 0x03);  // both completion flags

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.sessionCount, 4u);
  EXPECT_EQ(out.totalReadingSeconds, 3661u);
  EXPECT_EQ(out.totalPagesTurned, 120u);
  EXPECT_TRUE(out.isCompleted);
  // Legacy seconds-per-page average was dropped during the v5 -> v6 migration:
  // reading speed now comes from the WPM window only.
  EXPECT_EQ(out.wpm.count, 1u);
  EXPECT_EQ(out.wpm.avg, 440u);
  EXPECT_EQ(out.sessionWindow.count, 5u);
  EXPECT_EQ(out.sessionWindow.avg, 600u);
  EXPECT_TRUE(out.startDateManual);
  EXPECT_EQ(out.startDate.year, 2026);
  EXPECT_EQ(out.startDate.month, 8);
  EXPECT_EQ(out.startDate.day, 26);
  EXPECT_TRUE(out.finishedDateManual);
  EXPECT_TRUE(out.completionAchievementPending);
  EXPECT_TRUE(out.completionPromptDismissedAtHundred);
  EXPECT_EQ(out.finishedDate.year, 2026);
  EXPECT_EQ(out.finishedDate.month, 8);
  EXPECT_EQ(out.finishedDate.day, 27);
  EXPECT_EQ(out.timeOfDaySeconds[0], 600u);  // morning bucket
  EXPECT_EQ(out.timeOfDaySeconds[3], 300u);  // night bucket
  EXPECT_EQ(out.estimatedTimeLeftSeconds, 5400u);

  EXPECT_TRUE(BookReadingStats::remove(BOOK_DIR));
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 8)));
  const BookReadingStats gone = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(gone.sessionCount, 0u);
}

TEST_F(ReadingStatsBinaryStoreTest, BookTornWriteStartsFresh) {
  // Simulate a battery cut mid-write: short garbage file.
  HalFile f;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 8), f));
  const uint8_t garbage[] = {0x05, 0xAA, 0xBB};
  f.write(garbage, sizeof(garbage));

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.sessionCount, 0u);
  EXPECT_EQ(out.totalReadingSeconds, 0u);
  EXPECT_FALSE(out.isCompleted);
}

TEST_F(ReadingStatsBinaryStoreTest, BookLegacyFallbackChain) {
  // v5 record (73 bytes) in stats_v5.bin: accepted on load, then upgraded
  // in place on the next save — straight to v8, the current version, with
  // every recognized legacy file removed so a later load goes to the v8 file.
  std::vector<uint8_t> v5(73, 0);
  v5[0] = 5;
  v5[1] = 2;    // sessionCount
  v5[3] = 111;  // totalReadingSeconds (LE)
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 5), f));
    f.write(v5.data(), v5.size());
  }

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.sessionCount, 2u);
  EXPECT_EQ(out.totalReadingSeconds, 111u);
  EXPECT_EQ(out.estimatedTimeLeftSeconds, 0u);

  // Save writes v8 (one hop across three format versions), deletes v5-v7.
  out.save(BOOK_DIR);
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 5).c_str()));
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 6).c_str()));
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 7).c_str()));
  const auto bytes = readFileBytes(statsPath(BOOK_DIR, 8));
  ASSERT_EQ(bytes.size(), 135u);
  EXPECT_EQ(bytes[0], 8);
}

TEST_F(ReadingStatsBinaryStoreTest, RemoveCoversAllFallbackNames) {
  for (const char* name : {"stats_v8.bin", "stats_v7.bin", "stats_v6.bin", "stats_v5.bin"}) {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", std::string(BOOK_DIR) + "/" + name, f));
    const uint8_t byte = 5;
    f.write(&byte, 1);
  }

  EXPECT_TRUE(BookReadingStats::remove(BOOK_DIR));
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 8)));
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 7)));
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 6)));
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 5)));
}

// A corrupt stats_v6.bin must not shadow a valid older record: the loader
// falls through to the next candidate instead of starting fresh.
TEST_F(ReadingStatsBinaryStoreTest, CorruptV6FallsBackToV5) {
  // Garbage v6 (right size, wrong version).
  std::vector<uint8_t> corruptV6(109, 0);
  corruptV6[0] = 0xEE;
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 6), f));
    f.write(corruptV6.data(), corruptV6.size());
  }
  // Valid v5 with real data.
  std::vector<uint8_t> v5(73, 0);
  v5[0] = 5;
  v5[1] = 8;   // sessionCount = 8
  v5[3] = 77;  // totalReadingSeconds = 77
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 5), f));
    f.write(v5.data(), v5.size());
  }

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.sessionCount, 8u);
  EXPECT_EQ(out.totalReadingSeconds, 77u);
  EXPECT_EQ(out.wpm.count, 0u);  // v5 record carries no WPM window
}

// A v5 record loads with empty windows; saving writes the current v8 format.
TEST_F(ReadingStatsBinaryStoreTest, BackwardCompatV5) {
  std::vector<uint8_t> v5(73, 0);
  v5[0] = 5;
  v5[1] = 6;  // sessionCount = 6
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 5), f));
    f.write(v5.data(), v5.size());
  }

  BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.sessionCount, 6u);
  EXPECT_EQ(out.wpm.count, 0u);
  EXPECT_EQ(out.wpm.avg, 0u);

  for (int i = 0; i < 15; ++i) {
    out.recordForwardPageRead(60, 220);  // 220 WPM each
  }
  EXPECT_EQ(out.wpm.count, 15u);
  EXPECT_EQ(out.wpm.avg, 220u);
  out.save(BOOK_DIR);

  const auto bytes = readFileBytes(statsPath(BOOK_DIR, 8));
  ASSERT_EQ(bytes.size(), 135u);
  EXPECT_EQ(bytes[0], 8);
}

TEST_F(ReadingStatsBinaryStoreTest, BookProgressPercentRoundTrip) {
  BookReadingStats b;
  b.totalReadingSeconds = 600;
  b.lastBookProgressPercent = 42;
  b.save(BOOK_DIR);

  const auto bytes = readFileBytes(statsPath(BOOK_DIR, 8));
  ASSERT_EQ(bytes.size(), 135u);
  EXPECT_EQ(bytes[0], 8);      // version byte
  EXPECT_EQ(bytes[108], 42u);  // progress byte keeps its v6 offset in v8

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.lastBookProgressPercent, 42u);
}

TEST_F(ReadingStatsBinaryStoreTest, BookProgressUnknownClampedFromTornByte) {
  // A torn write or out-of-range byte (here 200 > 100) must load as the
  // unknown sentinel, never as a bogus percentage.
  std::vector<uint8_t> record(109, 0);
  record[0] = 6;
  record[108] = 200;
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 6), f));
    f.write(record.data(), record.size());
  }

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.lastBookProgressPercent, static_cast<uint8_t>(UNKNOWN_BOOK_PROGRESS_PERCENT));
}

// Global v3 record (159 bytes — one version behind the current v4) is
// recognized on load: bookkeeping loads, the trailing WPM window is empty.
// v1/v2 layouts are no longer supported — see the binary layout comment in
// GlobalReadingStats.cpp.
TEST_F(ReadingStatsBinaryStoreTest, GlobalLegacyRecordsLoad) {
  // v3: 159 bytes
  std::vector<uint8_t> v3(159, 0);
  v3[0] = 3;
  v3[1] = 21;    // totalSessions = 21 (low byte; high bytes remain 0)
  v3[5] = 0x58;  // totalReadingSeconds = 0x58 = 88 (low byte; high bytes remain 0)
  v3[6] = 0x02;  // high byte of totalReadingSeconds -> 0x0258 = 600
  v3[13] = 4;    // completedBooks = 4 (low byte; high bytes remain 0)
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", GLOBAL_PATH, f));
    f.write(v3.data(), v3.size());
  }

  const GlobalReadingStats out3 = GlobalReadingStats::load();
  EXPECT_EQ(out3.totalSessions, 21u);
  EXPECT_EQ(out3.completedBooks, 4u);
  EXPECT_EQ(out3.totalReadingSeconds, 600u);
  EXPECT_EQ(out3.wpm.count, 0u);  // v3 has no WPM window
  EXPECT_EQ(out3.wpm.avg, 0u);

  // Saving after loading a legacy record writes the CURRENT format back,
  // replacing the v3 file with v6 in place.
  out3.save();
  const auto bytes = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(bytes.size(), 407u);
  EXPECT_EQ(bytes[0], 6);
}

// After an explicit reset, saves must resume even if the destructive-save
// guard had been latched earlier by a newer-format file.
TEST_F(ReadingStatsBinaryStoreTest, ResetClearsNewerFormatGuard) {
  std::vector<uint8_t> future(408, 0);
  future[0] = 7;
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", GLOBAL_PATH, f));
    f.write(future.data(), future.size());
  }
  (void)GlobalReadingStats::load();  // latches the guard

  GlobalReadingStats blocked;
  blocked.totalSessions = 55;
  blocked.save();
  // Still refused: the file is untouched (version byte 99, zeroed sessions).
  const auto refused = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(refused.size(), 408u);
  EXPECT_EQ(refused[0], 7);
  EXPECT_EQ(readLe32At(refused, 1), 0u);

  ASSERT_TRUE(GlobalReadingStats::resetLocal());

  GlobalReadingStats resumed;
  resumed.totalSessions = 56;
  resumed.save();
  EXPECT_EQ(readLe32At(readFileBytes(GLOBAL_PATH), 1), 56u);  // saves work again
}

// Global v4 record (195 bytes — two versions behind the current v6) is
// recognized on load: bookkeeping loads, the trailing session window and
// minutes array are empty/backfilled, and the next save upgrades to v6.
TEST_F(ReadingStatsBinaryStoreTest, GlobalV4MigratesToV6InPlace) {
  std::vector<uint8_t> v4(195, 0);
  v4[0] = 4;
  v4[1] = 9;  // totalSessions = 9
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", GLOBAL_PATH, f));
    f.write(v4.data(), v4.size());
  }

  const GlobalReadingStats out = GlobalReadingStats::load();
  EXPECT_EQ(out.totalSessions, 9u);
  EXPECT_EQ(out.wpm.count, 0u);
  EXPECT_EQ(out.sessionWindow.count, 0u);
  EXPECT_EQ(out.sessionWindow.avg, 0u);

  out.save();
  const auto bytes = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(bytes.size(), 407u);
  EXPECT_EQ(bytes[0], 6);

  const GlobalReadingStats reloaded = GlobalReadingStats::load();
  EXPECT_EQ(reloaded.totalSessions, 9u);
  EXPECT_EQ(reloaded.sessionWindow.count, 0u);
}

// Book v6 record (109 bytes — two versions behind the current v8) loads via
// the migration path with an empty session window; the next save writes v8
// and removes the v6 file.
TEST_F(ReadingStatsBinaryStoreTest, BookV6MigratesToV8InPlace) {
  std::vector<uint8_t> v6(109, 0);
  v6[0] = 6;
  v6[1] = 3;       // sessionCount = 3
  v6[3] = 55;      // totalReadingSeconds = 55
  v6[108] = 0xFF;  // progress unknown sentinel
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 6), f));
    f.write(v6.data(), v6.size());
  }

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.sessionCount, 3u);
  EXPECT_EQ(out.wpm.count, 0u);
  EXPECT_EQ(out.sessionWindow.count, 0u);
  EXPECT_EQ(out.sessionWindow.avg, 0u);
  EXPECT_EQ(out.lastBookProgressPercent, static_cast<uint8_t>(UNKNOWN_BOOK_PROGRESS_PERCENT));

  out.save(BOOK_DIR);
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 6)));
  const auto bytes = readFileBytes(statsPath(BOOK_DIR, 8));
  ASSERT_EQ(bytes.size(), 135u);
  EXPECT_EQ(bytes[0], 8);

  const BookReadingStats reloaded = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(reloaded.sessionCount, 3u);
  EXPECT_EQ(reloaded.sessionWindow.count, 0u);
}

// A corrupt v7 record must not shadow a valid v6: the loader falls through
// to the next candidate instead of starting fresh.
TEST_F(ReadingStatsBinaryStoreTest, CorruptV7FallsBackToV6) {
  std::vector<uint8_t> corruptV7(134, 0);
  corruptV7[0] = 0xEE;
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 7), f));
    f.write(corruptV7.data(), corruptV7.size());
  }
  std::vector<uint8_t> v6(109, 0);
  v6[0] = 6;
  v6[1] = 4;  // sessionCount = 4
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 6), f));
    f.write(v6.data(), v6.size());
  }

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.sessionCount, 4u);  // from the v6 candidate, not the corrupt v7
  EXPECT_EQ(out.sessionWindow.count, 0u);
}

// Wire-offset pins for the session window: a saved record must carry the
// session window at exactly bytes 109-133, so a layout regression is caught
// at the file level rather than only via symmetric round-trip.
TEST_F(ReadingStatsBinaryStoreTest, BookSessionWindowWireOffsets) {
  BookReadingStats b;
  b.recordSession(600);
  b.save(BOOK_DIR);

  const auto bytes = readFileBytes(statsPath(BOOK_DIR, 8));
  ASSERT_EQ(bytes.size(), 135u);
  // avg (u16 LE) at 109-110: 600 = 0x0258
  EXPECT_EQ(bytes[109], 0x58);
  EXPECT_EQ(bytes[110], 0x02);
  // count (u16 LE) at 111-112: 1
  EXPECT_EQ(bytes[111], 0x01);
  EXPECT_EQ(bytes[112], 0x00);
  // samples[0] (u16 LE) at 113-114
  EXPECT_EQ(bytes[113], 0x58);
  EXPECT_EQ(bytes[114], 0x02);
  // pos at 133
  EXPECT_EQ(bytes[133], 1u);
}

// Same pin for the global v6 session window at bytes 195-224.
TEST_F(ReadingStatsBinaryStoreTest, GlobalSessionWindowWireOffsets) {
  GlobalReadingStats g;
  g.recordGlobalSession(600);
  g.save();

  const auto bytes = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(bytes.size(), 407u);
  EXPECT_EQ(bytes[195], 0x58);  // avg low byte
  EXPECT_EQ(bytes[196], 0x02);  // avg high byte
  EXPECT_EQ(bytes[197], 1u);    // count low byte
  EXPECT_EQ(bytes[198], 0u);    // count high byte
  EXPECT_EQ(bytes[224], 1u);    // pos
}

TEST_F(ReadingStatsBinaryStoreTest, GlobalDailyMinutesWireOffsets) {
  GlobalReadingStats g;
  g.recordReadingSpan(makeDateTime(2026, 8, 1, 23), 3600);
  g.readingHistoryAnchorDay = 20600;
  g.dailyReadingMinutes[0] = 60;
  g.dailyReadingMinutes[90] = 123;
  g.save();

  const auto bytes = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(bytes.size(), 407u);
  EXPECT_EQ(bytes[0], 6);
  // Minute 0 is byte 225 (60 real minutes from a one-hour span).
  EXPECT_EQ(bytes[225], 60u);
  EXPECT_EQ(bytes[226], 0u);
  // Minute 90 is bytes 405-406 (last entry of the 91-day array).
  EXPECT_EQ(bytes[405], 123u);
  EXPECT_EQ(bytes[406], 0u);

  const GlobalReadingStats out = GlobalReadingStats::load();
  EXPECT_EQ(out.readingHistoryAnchorDay, 20600u);
  EXPECT_EQ(out.readingMinutesOnDay(20600), 60u);
  EXPECT_EQ(out.readingMinutesOnDay(20600 - 90), 123u);
  EXPECT_EQ(out.readingMinutesOnDay(20600 - 91), 0u);
}

TEST_F(ReadingStatsBinaryStoreTest, GlobalDailyMinutesCrossMidnight) {
  GlobalReadingStats g;
  g.recordReadingSpan(makeDateTime(2026, 8, 2, 23), 5400);

  EXPECT_EQ(g.readingMinutesOnDay(readingStatsDayIndex(ReadingStatsDate{2026, 8, 3})), 30u);
  EXPECT_EQ(g.readingMinutesOnDay(readingStatsDayIndex(ReadingStatsDate{2026, 8, 2})), 60u);
  EXPECT_TRUE(g.readingHistoryBits[0] & 0x01);
  EXPECT_TRUE(g.readingHistoryBits[0] & 0x02);
}

TEST_F(ReadingStatsBinaryStoreTest, GlobalLegacyDailyMinutesBackfill) {
  std::vector<uint8_t> v3(159, 0);
  v3[0] = 3;
  v3[1] = 4;
  v3[65] = 0b101;
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", GLOBAL_PATH, f));
    ASSERT_EQ(f.write(v3.data(), v3.size()), v3.size());
  }

  const GlobalReadingStats out = GlobalReadingStats::load();
  EXPECT_EQ(out.dailyReadingMinutes[0], 1u);
  EXPECT_EQ(out.dailyReadingMinutes[1], 0u);
  EXPECT_EQ(out.dailyReadingMinutes[2], 1u);
  EXPECT_EQ(out.dailyReadingMinutes[3], 0u);

  out.save();
  const auto bytes = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(bytes.size(), 407u);
  EXPECT_EQ(bytes[0], 6);
  EXPECT_EQ(bytes[225], 1u);
  EXPECT_EQ(bytes[226], 0u);
  EXPECT_EQ(bytes[229], 1u);
  EXPECT_EQ(bytes[230], 0u);
}

TEST_F(ReadingStatsBinaryStoreTest, GlobalForwardMinuteValueClamps) {
  std::vector<uint8_t> v6(407, 0);
  v6[0] = 6;
  writeLe16At(v6, 225, 1500);
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", GLOBAL_PATH, f));
    ASSERT_EQ(f.write(v6.data(), v6.size()), v6.size());
  }

  const GlobalReadingStats out = GlobalReadingStats::load();
  EXPECT_EQ(out.readingMinutesOnDay(0), 1440u);
}

TEST_F(ReadingStatsBinaryStoreTest, BookCompletionFlagsMigrateFromV7) {
  std::vector<uint8_t> v7(134, 0);
  v7[0] = 7;
  v7[1] = 2;
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 7), f));
    ASSERT_EQ(f.write(v7.data(), v7.size()), v7.size());
  }

  BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  ASSERT_EQ(out.sessionCount, 2u);
  EXPECT_FALSE(out.completionAchievementPending);
  EXPECT_FALSE(out.completionPromptDismissedAtHundred);
  out.completionAchievementPending = true;
  out.completionPromptDismissedAtHundred = true;
  out.save(BOOK_DIR);
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 7)));

  const auto bytes = readFileBytes(statsPath(BOOK_DIR, 8));
  ASSERT_EQ(bytes.size(), 135u);
  EXPECT_EQ(bytes[0], 8);
  EXPECT_EQ(bytes[134], 0x03);
  const BookReadingStats reloaded = BookReadingStats::load(BOOK_DIR);
  EXPECT_TRUE(reloaded.completionAchievementPending);
  EXPECT_TRUE(reloaded.completionPromptDismissedAtHundred);
}

TEST_F(ReadingStatsBinaryStoreTest, BookNewerFormatBlocksSaves) {
  (void)BookReadingStats::remove(BOOK_DIR);  // clear a prior test's latch
  std::vector<uint8_t> future(136, 0);
  future[0] = 9;
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 9), f));
    f.write(future.data(), future.size());
  }

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.sessionCount, 0u);
  BookReadingStats blocked;
  blocked.sessionCount = 55;
  blocked.save(BOOK_DIR);

  // The future-format file is untouched and no v8 file shadows it.
  const auto after = readFileBytes(statsPath(BOOK_DIR, 9));
  ASSERT_EQ(after.size(), 136u);
  EXPECT_EQ(after[0], 9);
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 8)));
}

TEST_F(ReadingStatsBinaryStoreTest, CorruptCurrentFileDoesNotLatchGuard) {
  (void)BookReadingStats::remove(BOOK_DIR);  // clear a prior test's latch
  // A corrupt current-version record (garbage version byte) must be skipped,
  // not treated as a forward-format file: only stats_v9.bin may latch the
  // destructive-save guard.
  std::vector<uint8_t> corrupt(135, 0);
  corrupt[0] = 10;  // > STATS_FILE_VERSION in a v8-named file
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 8), f));
    f.write(corrupt.data(), corrupt.size());
  }

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.sessionCount, 0u);  // not decodable — fresh stats

  BookReadingStats fresh;
  fresh.sessionCount = 7;
  fresh.save(BOOK_DIR);
  // The save went through (no latch) and wrote the current version.
  const BookReadingStats saved = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(saved.sessionCount, 7u);
}

TEST_F(ReadingStatsBinaryStoreTest, RemoveKeepsGuardWhileForwardFileExists) {
  (void)BookReadingStats::remove(BOOK_DIR);  // clear a prior test's latch
  std::vector<uint8_t> future(136, 0);
  future[0] = 9;
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 9), f));
    f.write(future.data(), future.size());
  }
  (void)BookReadingStats::load(BOOK_DIR);  // latch the guard via the forward file

  // remove() deletes v8-v5 but the forward record remains: saves stay blocked,
  // otherwise a fresh v8 record would shadow the newer firmware's data.
  EXPECT_TRUE(BookReadingStats::remove(BOOK_DIR));
  BookReadingStats blocked;
  blocked.sessionCount = 55;
  blocked.save(BOOK_DIR);
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 8)));
}

TEST_F(ReadingStatsBinaryStoreTest, FinishedBooksRoundTripAndWireSize) {
  BookReadingStats stats;
  stats.isCompleted = true;
  stats.totalReadingSeconds = 7200;
  stats.startDate = ReadingStatsDate{2026, 8, 1};
  stats.finishedDate = ReadingStatsDate{2026, 8, 10};

  ASSERT_TRUE(FinishedBooksIndex::record("/books/one.epub", "One", "Author", stats));
  const auto bytes = readFileBytes("/.crosspoint/finished_books.bin");
  ASSERT_EQ(bytes.size(), 41u);  // 8-byte header + 33-byte fixed/text entry
  EXPECT_EQ(bytes[4], 3u);       // CPFB version
  EXPECT_EQ(bytes[5], 1u);       // one entry
  EXPECT_EQ(bytes[6], 0u);       // reserved field

  const auto entries = FinishedBooksIndex::load();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].pathKey, testPathKey("/books/one.epub"));
  EXPECT_EQ(entries[0].title, "One");
  EXPECT_EQ(entries[0].author, "Author");
  EXPECT_EQ(entries[0].totalReadingSeconds, 7200u);
  EXPECT_EQ(entries[0].startDate.year, 2026u);
  EXPECT_EQ(entries[0].finishedDate.day, 10u);
}

TEST_F(ReadingStatsBinaryStoreTest, FinishedBooksCapsAt32Entries) {
  BookReadingStats stats;
  stats.isCompleted = true;
  for (int i = 0; i < 33; ++i) {
    const std::string path = "/books/" + std::to_string(i) + ".epub";
    ASSERT_TRUE(FinishedBooksIndex::record(path, "Book " + std::to_string(i), "Author", stats));
  }

  const auto entries = FinishedBooksIndex::load();
  EXPECT_EQ(entries.size(), FinishedBooksIndex::MAX_ENTRIES);
}

TEST_F(ReadingStatsBinaryStoreTest, FinishedBooksRecoversFromBackup) {
  BookReadingStats first;
  first.isCompleted = true;
  first.totalReadingSeconds = 100;
  ASSERT_TRUE(FinishedBooksIndex::record("/books/first.epub", "First", "Author", first));

  BookReadingStats second;
  second.isCompleted = true;
  second.totalReadingSeconds = 200;
  ASSERT_TRUE(FinishedBooksIndex::record("/books/second.epub", "Second", "Author", second));
  EXPECT_TRUE(Storage.exists("/.crosspoint/finished_books.bin.bak"));

  HalFile corrupt;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/.crosspoint/finished_books.bin", corrupt));
  const uint8_t garbage[] = {0, 1, 2};
  ASSERT_EQ(corrupt.write(garbage, sizeof(garbage)), sizeof(garbage));

  const auto entries = FinishedBooksIndex::load();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].title, "First");
  EXPECT_EQ(entries[0].totalReadingSeconds, 100u);
}

TEST_F(ReadingStatsBinaryStoreTest, FinishedBooksRestoresBackupBeforeRotation) {
  BookReadingStats first;
  first.isCompleted = true;
  first.totalReadingSeconds = 100;
  ASSERT_TRUE(FinishedBooksIndex::record("/books/first.epub", "First", "Author", first));

  BookReadingStats second;
  second.isCompleted = true;
  second.totalReadingSeconds = 200;
  ASSERT_TRUE(FinishedBooksIndex::record("/books/second.epub", "Second", "Author", second));

  // Corrupt the primary; the .bak still holds the first record.
  HalFile corrupt;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/.crosspoint/finished_books.bin", corrupt));
  const uint8_t garbage[] = {0, 1, 2};
  ASSERT_EQ(corrupt.write(garbage, sizeof(garbage)), sizeof(garbage));

  // The next record must restore the primary from the backup BEFORE the
  // rotation deletes the backup, so the rotated .bak stays decodable.
  BookReadingStats third;
  third.isCompleted = true;
  third.totalReadingSeconds = 300;
  ASSERT_TRUE(FinishedBooksIndex::record("/books/third.epub", "Third", "Author", third));

  const auto entries = FinishedBooksIndex::load();
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].title, "First");  // survived only in the backup
  EXPECT_EQ(entries[1].title, "Third");

  const auto bak = readFileBytes("/.crosspoint/finished_books.bin.bak");
  ASSERT_GE(bak.size(), 6u);
  EXPECT_EQ(bak[0], 'C');
  EXPECT_EQ(bak[4], 3u);  // restored primary, not the corrupt bytes
  EXPECT_EQ(bak[5], 1u);  // one entry (the pre-restore state)
}

TEST_F(ReadingStatsBinaryStoreTest, FinishedBooksRecoversCompletedRecentBooks) {
  FinishedBooksIndex::setRecentBooksForTest({{"/books/epub.epub", "Epub", "Epub Author"},
                                             {"/books/xtc.xtc", "Xtc", "Xtc Author"},
                                             {"/books/plain.txt", "Plain", "Plain Author"}});

  BookReadingStats epubStats;
  epubStats.isCompleted = true;
  epubStats.totalReadingSeconds = 111;
  epubStats.save("/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}("/books/epub.epub")));
  BookReadingStats xtcStats;
  xtcStats.isCompleted = true;
  xtcStats.totalReadingSeconds = 222;
  xtcStats.save("/.crosspoint/xtc_" + std::to_string(std::hash<std::string>{}("/books/xtc.xtc")));

  const auto entries = FinishedBooksIndex::load();
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].title, "Epub");
  EXPECT_EQ(entries[1].title, "Xtc");
}

TEST_F(ReadingStatsBinaryStoreTest, FinishedBooksMigratesPath) {
  BookReadingStats stats;
  stats.isCompleted = true;
  ASSERT_TRUE(FinishedBooksIndex::record("/books/old.epub", "Old", "Author", stats));
  ASSERT_TRUE(FinishedBooksIndex::migratePath("/books/old.epub", "/books/new.epub"));

  const auto entries = FinishedBooksIndex::load();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].pathKey, testPathKey("/books/new.epub"));
  EXPECT_EQ(entries[0].title, "Old");
}

TEST_F(ReadingStatsBinaryStoreTest, FinishedBooksLegacyV2LoadsStartDate) {
  constexpr char path[] = "/books/legacy.epub";
  const std::string title = "Legacy";
  std::vector<uint8_t> bytes(8 + 8 + 4 + 4 + 4 + 2 + title.size(), 0);
  std::memcpy(bytes.data(), "CPFB", 4);
  bytes[4] = 2;
  bytes[5] = 1;
  size_t offset = 8;
  writeLe64At(bytes, offset, testPathKey(path));
  offset += 8;
  writeLe32At(bytes, offset, 321);
  offset += 4;
  writeLe16At(bytes, offset, 2025);
  offset += 2;
  bytes[offset++] = 7;
  bytes[offset++] = 4;
  writeLe16At(bytes, offset, 2025);
  offset += 2;
  bytes[offset++] = 7;
  bytes[offset++] = 8;
  writeLe16At(bytes, offset, static_cast<uint16_t>(title.size()));
  offset += 2;
  std::memcpy(bytes.data() + offset, title.data(), title.size());
  {
    HalFile file;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", "/.crosspoint/finished_books.bin", file));
    ASSERT_EQ(file.write(bytes.data(), bytes.size()), bytes.size());
  }

  const auto entries = FinishedBooksIndex::load();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].pathKey, testPathKey(path));
  EXPECT_EQ(entries[0].totalReadingSeconds, 321u);
  EXPECT_EQ(entries[0].startDate.year, 2025u);
  EXPECT_EQ(entries[0].startDate.month, 7u);
  EXPECT_EQ(entries[0].startDate.day, 4u);
  EXPECT_EQ(entries[0].finishedDate.year, 2025u);
  EXPECT_EQ(entries[0].finishedDate.month, 7u);
  EXPECT_EQ(entries[0].finishedDate.day, 8u);
  EXPECT_EQ(entries[0].title, title);
  EXPECT_TRUE(entries[0].author.empty());
}

TEST_F(ReadingStatsBinaryStoreTest, FinishedBooksForwardVersionBlocksSaves) {
  // A file written by a newer firmware: same magic, higher version.
  std::vector<uint8_t> future(64, 0);
  std::memcpy(future.data(), "CPFB", 4);
  future[4] = 4;  // INDEX_VERSION + 1
  future[5] = 1;
  {
    HalFile file;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", "/.crosspoint/finished_books.bin", file));
    ASSERT_EQ(file.write(future.data(), future.size()), future.size());
  }

  // This build cannot decode it: nothing loads.
  EXPECT_TRUE(FinishedBooksIndex::load().empty());

  BookReadingStats stats;
  stats.isCompleted = true;
  EXPECT_FALSE(FinishedBooksIndex::record("/books/new.epub", "New", "Author", stats));

  // The forward-version file is untouched and no legacy index shadows it.
  const auto after = readFileBytes("/.crosspoint/finished_books.bin");
  ASSERT_EQ(after.size(), future.size());
  EXPECT_EQ(after[4], 4u);
  EXPECT_FALSE(Storage.exists("/.crosspoint/finished_books.bin.tmp"));
}
