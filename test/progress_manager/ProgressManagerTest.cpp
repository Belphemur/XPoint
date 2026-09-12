// Host tests for ProgressManager's save coordination (PRRT...J3r/J3z):
// a synchronous bypass save (KOReader sync / cache-clear backup / footnote
// origin) with a record that differs from the live mirror must reach disk
// AND be adopted as the live mirror, so a later worker or closeBook() flush
// cannot revert the just-written file.
#include <gtest/gtest.h>

#include <string>

#include "ProgressManager.h"
#include "activities/reader/ProgressRecord.h"

// Test-controllable wall clock (defined in stubs/Stubs.cpp).
extern unsigned long testClockMs;

namespace {

// Seeds a legacy 10-byte record (offset shape) into the fake file map.
void seedLegacyRecord(const char* path, const uint16_t spine, const uint16_t page, const uint16_t count,
                      const uint32_t offset) {
  uint8_t buf[progress_record::kSizeOffset];
  const size_t n = progress_record::encode(/*hasOffset=*/true, /*hasGeneration=*/false, spine, page, count, offset, 0,
                                           0, buf, sizeof(buf));
  Storage.files[path] = std::string(reinterpret_cast<const char*>(buf), n);
}

ProgressRecord decodeDiskRecord(const std::string& bytes) {
  ProgressRecord rec;
  (void)progress_record::decode(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), rec);
  return rec;
}

}  // namespace

class ProgressManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.files.clear();
    testClockMs = 0;
    progressManager.begin();
    seedLegacyRecord("/cache/book/progress.bin", 1, 2, 20, 100);
  }
};

TEST_F(ProgressManagerTest, OpenBookRestoresBaseline) {
  uint16_t spine = 0, page = 0, count = 0;
  uint32_t offset = 0;
  ASSERT_TRUE(progressManager.openBook("/cache/book", spine, page, count, offset));
  EXPECT_EQ(spine, 1);
  EXPECT_EQ(page, 2);
  EXPECT_EQ(count, 20);
  EXPECT_EQ(offset, 100u);
}

TEST_F(ProgressManagerTest, SaveNowAdoptsDivergentRecord) {
  uint16_t spine = 0, page = 0, count = 0;
  uint32_t offset = 0;
  ASSERT_TRUE(progressManager.openBook("/cache/book", spine, page, count, offset));

  // The live mirror moves to a newer position; the interval gate holds the
  // write (worker stub never creates a task, so nothing is flushed).
  progressManager.save(4, 10, 20, false, 0);

  // A synchronous bypass save (KOReader sync) writes a DIFFERENT position.
  ASSERT_TRUE(progressManager.saveNow("/cache/book", 2, 5, 20, true, 555));

  // The file carries the bypass record…
  const auto& bytes = Storage.files.at("/cache/book/progress.bin");
  ASSERT_EQ(bytes.size(), progress_record::kSizeOffset);
  const ProgressRecord rec = decodeDiskRecord(bytes);
  EXPECT_EQ(rec.spineIndex, 2);
  EXPECT_EQ(rec.pageNumber, 5);
  EXPECT_EQ(rec.visibleTextOffset, 555u);

  // …and the mirror adopted it: neither a forced flush nor closeBook may
  // overwrite the just-written file with a different snapshot.
  ASSERT_TRUE(progressManager.flushNow());
  EXPECT_EQ(Storage.files.at("/cache/book/progress.bin"), bytes);

  progressManager.closeBook();
  EXPECT_EQ(Storage.files.at("/cache/book/progress.bin"), bytes);
}

TEST_F(ProgressManagerTest, SaveNowForOtherBookDoesNotAdopt) {
  uint16_t spine = 0, page = 0, count = 0;
  uint32_t offset = 0;
  ASSERT_TRUE(progressManager.openBook("/cache/book", spine, page, count, offset));

  // Bypass save for a DIFFERENT book's file: written to disk, but the open
  // book's in-memory state must stay untouched.
  ASSERT_TRUE(progressManager.saveNow("/cache/other", 9, 1, 30, true, 77));

  const auto& otherBytes = Storage.files.at("/cache/other/progress.bin");
  ASSERT_EQ(otherBytes.size(), progress_record::kSizeOffset);
  const ProgressRecord other = decodeDiskRecord(otherBytes);
  EXPECT_EQ(other.spineIndex, 9);
  EXPECT_EQ(other.pageNumber, 1);
  EXPECT_EQ(other.visibleTextOffset, 77u);

  // The open book's owed position still flushes normally afterwards.
  progressManager.save(4, 10, 20, false, 0);
  ASSERT_TRUE(progressManager.flushNow());
  const ProgressRecord book = decodeDiskRecord(Storage.files.at("/cache/book/progress.bin"));
  EXPECT_EQ(book.spineIndex, 4);
  EXPECT_EQ(book.pageNumber, 10);
  EXPECT_FALSE(book.hasOffset);
}

TEST_F(ProgressManagerTest, FlushWritesMirrorAfterIntervalGate) {
  uint16_t spine = 0, page = 0, count = 0;
  uint32_t offset = 0;
  ASSERT_TRUE(progressManager.openBook("/cache/book", spine, page, count, offset));

  // Position change inside the interval gate: queued-but-not-due (the worker
  // stub never wakes), so nothing reaches disk yet.
  progressManager.save(3, 7, 20, true, 42);

  // Forced flush (sleep / power-off path): the mirror's position lands.
  ASSERT_TRUE(progressManager.flushNow());
  const ProgressRecord rec = decodeDiskRecord(Storage.files.at("/cache/book/progress.bin"));
  EXPECT_EQ(rec.spineIndex, 3);
  EXPECT_EQ(rec.pageNumber, 7);
  EXPECT_EQ(rec.visibleTextOffset, 42u);

  // Baseline advanced: a second forced flush is a no-op.
  const auto bytes = Storage.files.at("/cache/book/progress.bin");
  ASSERT_TRUE(progressManager.flushNow());
  EXPECT_EQ(Storage.files.at("/cache/book/progress.bin"), bytes);
}
