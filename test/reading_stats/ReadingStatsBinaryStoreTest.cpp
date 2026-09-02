#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "BookReadingStats.h"
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

void seedBookRecordV5(const std::string& path, uint16_t sessions, uint32_t seconds) {
  std::vector<uint8_t> data(73, 0);
  data[0] = 5;
  data[1] = sessions & 0xFF;
  data[2] = (sessions >> 8) & 0xFF;
  data[3] = seconds & 0xFF;
  data[4] = (seconds >> 8) & 0xFF;
  data[5] = (seconds >> 16) & 0xFF;
  data[6] = (seconds >> 24) & 0xFF;
  HalFile f;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", path, f));
  f.write(data.data(), data.size());
}
}  // namespace

class ReadingStatsBinaryStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.clear();
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
  g.longestReadingStreak = 7;
  g.wpm.record(60, 220);  // 220 words in 60 s -> 220 WPM
  g.save();

  const auto bytes = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(bytes.size(), 195u);
  EXPECT_EQ(bytes[0], 4);  // version byte

  GlobalReadingStats out = GlobalReadingStats::load();
  EXPECT_EQ(out.totalSessions, 5u);
  EXPECT_EQ(out.totalReadingSeconds, 9999u);
  EXPECT_EQ(out.totalPagesTurned, 42u);
  EXPECT_EQ(out.completedBooks, 2u);
  EXPECT_EQ(out.timeOfDaySeconds[0], 100u);
  EXPECT_EQ(out.dayOfWeekSeconds[6], 200u);
  EXPECT_EQ(out.readingHistoryAnchorDay, 20600u);
  EXPECT_EQ(out.readingHistoryBits[10], 0xAB);
  EXPECT_EQ(out.longestReadingStreak, 7u);
  EXPECT_EQ(out.wpm.count, 1u);
  EXPECT_EQ(out.wpm.avg, 220u);
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
  ASSERT_EQ(bak.size(), 195u);
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
  ASSERT_EQ(main3.size(), 195u);
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
  std::vector<uint8_t> future(160, 0);
  future[0] = 99;  // version far ahead
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
  ASSERT_EQ(after.size(), 160u);
  EXPECT_EQ(after[0], 99);
}

TEST_F(ReadingStatsBinaryStoreTest, GlobalResetLocalBypassesNewerFormatGuard) {
  std::vector<uint8_t> future(160, 0);
  future[0] = 99;
  {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", GLOBAL_PATH, f));
    f.write(future.data(), future.size());
  }
  (void)GlobalReadingStats::load();  // latches the guard

  EXPECT_TRUE(GlobalReadingStats::resetLocal());
  const auto after = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(after.size(), 195u);
  EXPECT_EQ(after[0], 4);
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
  b.save(BOOK_DIR);

  const auto bytes = readFileBytes(statsPath(BOOK_DIR, 6));
  ASSERT_EQ(bytes.size(), 109u);
  EXPECT_EQ(bytes[0], 6);  // version byte

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.sessionCount, 4u);
  EXPECT_EQ(out.totalReadingSeconds, 3661u);
  EXPECT_EQ(out.totalPagesTurned, 120u);
  EXPECT_TRUE(out.isCompleted);
  // Legacy seconds-per-page average was dropped during the v5 -> v6 migration:
  // reading speed now comes from the WPM window only.
  EXPECT_EQ(out.wpm.count, 1u);
  EXPECT_EQ(out.wpm.avg, 440u);
  EXPECT_TRUE(out.startDateManual);
  EXPECT_EQ(out.startDate.year, 2026);
  EXPECT_EQ(out.startDate.month, 8);
  EXPECT_EQ(out.startDate.day, 26);
  EXPECT_TRUE(out.finishedDateManual);
  EXPECT_EQ(out.finishedDate.year, 2026);
  EXPECT_EQ(out.finishedDate.month, 8);
  EXPECT_EQ(out.finishedDate.day, 27);
  EXPECT_EQ(out.timeOfDaySeconds[0], 600u);  // morning bucket
  EXPECT_EQ(out.timeOfDaySeconds[3], 300u);  // night bucket
  EXPECT_EQ(out.estimatedTimeLeftSeconds, 5400u);

  EXPECT_TRUE(BookReadingStats::remove(BOOK_DIR));
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 6)));
  const BookReadingStats gone = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(gone.sessionCount, 0u);
}

TEST_F(ReadingStatsBinaryStoreTest, BookTornWriteStartsFresh) {
  // Simulate a battery cut mid-write: short garbage file.
  HalFile f;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath(BOOK_DIR, 6), f));
  const uint8_t garbage[] = {0x05, 0xAA, 0xBB};
  f.write(garbage, sizeof(garbage));

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.sessionCount, 0u);
  EXPECT_EQ(out.totalReadingSeconds, 0u);
  EXPECT_FALSE(out.isCompleted);
}

TEST_F(ReadingStatsBinaryStoreTest, BookLegacyFallbackChain) {
  // v5 record (73 bytes) in stats_v5.bin: accepted on load, then upgraded to
  // v6 in place on the next save. The legacy file is removed as part of the
  // migration so a later load goes straight to the v6 file. The v4 layout
  // (69 B) and crossink's unversioned stats.bin are no longer recognized.
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

  // Save writes v6, deletes the v5 file.
  out.save(BOOK_DIR);
  EXPECT_FALSE(Storage.exists(statsPath(BOOK_DIR, 5).c_str()));
  const auto bytes = readFileBytes(statsPath(BOOK_DIR, 6));
  ASSERT_EQ(bytes.size(), 109u);
  EXPECT_EQ(bytes[0], 6);
}

TEST_F(ReadingStatsBinaryStoreTest, RemoveCoversAllFallbackNames) {
  for (const char* name : {"stats_v6.bin", "stats_v5.bin"}) {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", std::string(BOOK_DIR) + "/" + name, f));
    const uint8_t byte = 5;
    f.write(&byte, 1);
  }

  EXPECT_TRUE(BookReadingStats::remove(BOOK_DIR));
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

// A v5 record loads with an empty WPM window; saving writes the v6 format back.
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

  const auto bytes = readFileBytes(statsPath(BOOK_DIR, 6));
  ASSERT_EQ(bytes.size(), 109u);
  EXPECT_EQ(bytes[0], 6);
}

TEST_F(ReadingStatsBinaryStoreTest, BookReadingStatsV6_ProgressPercentRoundTrip) {
  BookReadingStats b;
  b.totalReadingSeconds = 600;
  b.lastBookProgressPercent = 42;
  b.save(BOOK_DIR);

  const auto bytes = readFileBytes(statsPath(BOOK_DIR, 6));
  ASSERT_EQ(bytes.size(), 109u);
  EXPECT_EQ(bytes[0], 6);      // version byte unchanged
  EXPECT_EQ(bytes[108], 42u);  // progress byte lands at its v6 offset

  const BookReadingStats out = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(out.lastBookProgressPercent, 42u);
}

TEST_F(ReadingStatsBinaryStoreTest, BookReadingStatsV6_ProgressPercentUnknownClampedFromTornByte) {
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
  // replacing the v3 file with v4 in place.
  out3.save();
  const auto bytes = readFileBytes(GLOBAL_PATH);
  ASSERT_EQ(bytes.size(), 195u);
  EXPECT_EQ(bytes[0], 4);
}

// After an explicit reset, saves must resume even if the destructive-save
// guard had been latched earlier by a newer-format file.
TEST_F(ReadingStatsBinaryStoreTest, ResetClearsNewerFormatGuard) {
  std::vector<uint8_t> future(160, 0);
  future[0] = 99;
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
  ASSERT_EQ(refused.size(), 160u);
  EXPECT_EQ(refused[0], 99);
  EXPECT_EQ(readLe32At(refused, 1), 0u);

  ASSERT_TRUE(GlobalReadingStats::resetLocal());

  GlobalReadingStats resumed;
  resumed.totalSessions = 56;
  resumed.save();
  EXPECT_EQ(readLe32At(readFileBytes(GLOBAL_PATH), 1), 56u);  // saves work again
}
