// Host tests for SdCardCacheStorage (PRRT...Ulfv): validName accept/reject,
// endWrite success (tmp→final), rename-failure paths (final preserved via
// "<final>.old" rotate), and close-failure handling.
#include <gtest/gtest.h>

#include <string>

#include "HalFile.h"
#include "HalStorage.h"
#include "adapters/SdCardCacheStorage.h"

using freeink::book::SdCardCacheStorage;

namespace {
constexpr const char* kDir = "/cache";
}  // namespace

class SdCardCacheStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.files.clear();
    Storage.failOnRenameCall = 0;
    Storage.renameCallCount = 0;
    HalFile::testFailClose = false;
    HalFile::testFailWrite = false;
    HalFile::testFailSync = false;
  }
};

// ── validName accept/reject (observed through exists()) ─────────────────────

TEST_F(SdCardCacheStorageTest, ValidNameAccepted) {
  Storage.files["/cache/s3-1a2b3c4d.fibp"] = "x";
  SdCardCacheStorage s(kDir);
  EXPECT_TRUE(s.exists("s3-1a2b3c4d.fibp"));
}

TEST_F(SdCardCacheStorageTest, ValidNameMaxLengthAccepted) {
  std::string name(SdCardCacheStorage::kNameMax, 'a');  // exactly 64 chars
  Storage.files[std::string(kDir) + "/" + name] = "x";
  SdCardCacheStorage s(kDir);
  EXPECT_TRUE(s.exists(name.c_str()));
}

TEST_F(SdCardCacheStorageTest, NameOverMaxLengthRejected) {
  std::string name(SdCardCacheStorage::kNameMax + 1, 'a');
  Storage.files[std::string(kDir) + "/" + name] = "x";
  SdCardCacheStorage s(kDir);
  EXPECT_FALSE(s.exists(name.c_str()));
}

TEST_F(SdCardCacheStorageTest, DotRejected) {
  Storage.files["/cache/."] = "x";
  SdCardCacheStorage s(kDir);
  EXPECT_FALSE(s.exists("."));
}

TEST_F(SdCardCacheStorageTest, DotDotRejected) {
  Storage.files["/cache/.."] = "x";
  SdCardCacheStorage s(kDir);
  EXPECT_FALSE(s.exists(".."));
}

TEST_F(SdCardCacheStorageTest, SlashRejected) {
  Storage.files["/cache/a/b"] = "x";
  SdCardCacheStorage s(kDir);
  EXPECT_FALSE(s.exists("a/b"));
}

TEST_F(SdCardCacheStorageTest, BackslashRejected) {
  Storage.files["/cache/a\\b"] = "x";
  SdCardCacheStorage s(kDir);
  EXPECT_FALSE(s.exists("a\\b"));
}

TEST_F(SdCardCacheStorageTest, EmptyNameRejected) {
  SdCardCacheStorage s(kDir);
  EXPECT_FALSE(s.exists(""));
  EXPECT_FALSE(s.exists(nullptr));
}

// ── endWrite success: .tmp renamed onto the final ───────────────────────────

TEST_F(SdCardCacheStorageTest, EndWritePublishesTmpToFinal) {
  SdCardCacheStorage s(kDir);
  ASSERT_TRUE(s.beginWrite("page1.fibp"));
  const char payload[] = "page-one-bytes";
  ASSERT_TRUE(s.write(payload, sizeof(payload) - 1));
  EXPECT_TRUE(s.endWrite());

  EXPECT_TRUE(Storage.files.count("/cache/page1.fibp") != 0);
  EXPECT_EQ(Storage.files["/cache/page1.fibp"], "page-one-bytes");
  EXPECT_FALSE(Storage.exists("/cache/page1.fibp.tmp"));
  EXPECT_FALSE(Storage.exists("/cache/page1.fibp.old"));
}

// A previous final is replaced by the new content, and the rotated .old copy
// is removed once the publish succeeds.
TEST_F(SdCardCacheStorageTest, EndWriteReplacesExistingFinalAndDropsOld) {
  Storage.files["/cache/page1.fibp"] = "old-generation";
  SdCardCacheStorage s(kDir);
  ASSERT_TRUE(s.beginWrite("page1.fibp"));
  ASSERT_TRUE(s.write("new-generation", 14));
  EXPECT_TRUE(s.endWrite());

  EXPECT_EQ(Storage.files["/cache/page1.fibp"], "new-generation");
  EXPECT_FALSE(Storage.exists("/cache/page1.fibp.old"));
  EXPECT_FALSE(Storage.exists("/cache/page1.fibp.tmp"));
}

// ── rename-failure paths (doc contract: previous final is retained) ─────────

// First rename (final → .old) fails: final untouched, .tmp left for retry.
TEST_F(SdCardCacheStorageTest, RotateFailureKeepsFinalAndTmp) {
  Storage.files["/cache/page1.fibp"] = "old-generation";
  Storage.failOnRenameCall = 1;  // rotate rename
  SdCardCacheStorage s(kDir);
  ASSERT_TRUE(s.beginWrite("page1.fibp"));
  ASSERT_TRUE(s.write("new-generation", 14));
  EXPECT_FALSE(s.endWrite());

  EXPECT_EQ(Storage.files["/cache/page1.fibp"], "old-generation");
  EXPECT_TRUE(Storage.exists("/cache/page1.fibp.tmp"));
  EXPECT_FALSE(Storage.exists("/cache/page1.fibp.old"));
}

// Second rename (.tmp → final) fails: the rotated .old is restored best-effort
// so readers always see the previous final.
TEST_F(SdCardCacheStorageTest, PublishFailureRestoresOldToFinal) {
  Storage.files["/cache/page1.fibp"] = "old-generation";
  Storage.failOnRenameCall = 2;  // publish rename (rotate = call 1 succeeds)
  SdCardCacheStorage s(kDir);
  ASSERT_TRUE(s.beginWrite("page1.fibp"));
  ASSERT_TRUE(s.write("new-generation", 14));
  EXPECT_FALSE(s.endWrite());

  // Restored previous final, no .old leftover, .tmp retained for retry.
  EXPECT_EQ(Storage.files["/cache/page1.fibp"], "old-generation");
  EXPECT_FALSE(Storage.exists("/cache/page1.fibp.old"));
  EXPECT_TRUE(Storage.exists("/cache/page1.fibp.tmp"));
}

// ── close-failure path: unpublished .tmp must not replace the final ─────────

TEST_F(SdCardCacheStorageTest, CloseFailureLeavesTmpForRetry) {
  Storage.files["/cache/page1.fibp"] = "old-generation";
  SdCardCacheStorage s(kDir);
  ASSERT_TRUE(s.beginWrite("page1.fibp"));
  ASSERT_TRUE(s.write("new-generation", 14));

  HalFile::testFailClose = true;
  EXPECT_FALSE(s.endWrite());
  HalFile::testFailClose = false;

  EXPECT_EQ(Storage.files["/cache/page1.fibp"], "old-generation");
  EXPECT_TRUE(Storage.exists("/cache/page1.fibp.tmp"));
  EXPECT_FALSE(Storage.exists("/cache/page1.fibp.old"));
}

TEST_F(SdCardCacheStorageTest, CloseFailureRetriesRetainedTmp) {
  Storage.files["/cache/page1.fibp"] = "old-generation";
  SdCardCacheStorage s(kDir);
  ASSERT_TRUE(s.beginWrite("page1.fibp"));
  ASSERT_TRUE(s.write("new-generation", 14));

  HalFile::testFailClose = true;
  EXPECT_FALSE(s.endWrite());
  HalFile::testFailClose = false;

  // beginWrite retries the close, then truncates the retained temporary file.
  ASSERT_TRUE(s.beginWrite("page1.fibp"));
  ASSERT_TRUE(s.write("retry", 5));
  ASSERT_TRUE(s.endWrite());
  EXPECT_EQ(Storage.files["/cache/page1.fibp"], "retry");
}

// A failed sync followed by a successful close must still mark the write as
// failed: PageCacheWriter's finish-cleanup calls remove() after the handle
// closes, and remove() only retains the last good final while a failed write
// is visible. Without the flag the good cache would be deleted.
TEST_F(SdCardCacheStorageTest, SyncFailurePreservesFinalDuringCleanup) {
  Storage.files["/cache/page1.fibp"] = "old-generation";
  SdCardCacheStorage s(kDir);
  ASSERT_TRUE(s.beginWrite("page1.fibp"));
  ASSERT_TRUE(s.write("new-generation", 14));

  HalFile::testFailSync = true;
  EXPECT_FALSE(s.endWrite());
  HalFile::testFailSync = false;

  // The close succeeded, so the .tmp handle is closed — but the failed-write
  // state must keep remove() from deleting the active final.
  EXPECT_TRUE(s.remove("page1.fibp"));
  EXPECT_EQ(Storage.files["/cache/page1.fibp"], "old-generation");
  EXPECT_TRUE(Storage.exists("/cache/page1.fibp.tmp"));
  EXPECT_FALSE(Storage.exists("/cache/page1.fibp.old"));

  // The retained .tmp is retryable on the next beginWrite.
  ASSERT_TRUE(s.beginWrite("page1.fibp"));
  ASSERT_TRUE(s.write("retry", 5));
  ASSERT_TRUE(s.endWrite());
  EXPECT_EQ(Storage.files["/cache/page1.fibp"], "retry");
}

// ── stale .old cleanup at beginWrite ────────────────────────────────────────

TEST_F(SdCardCacheStorageTest, BeginWriteRemovesStaleOld) {
  // Leftover from a previous endWrite() that crashed between rotate and
  // publish; the final is the good copy, so the .old is garbage.
  Storage.files["/cache/page1.fibp"] = "old-generation";
  Storage.files["/cache/page1.fibp.old"] = "stale-rotate";
  SdCardCacheStorage s(kDir);
  EXPECT_TRUE(s.beginWrite("page1.fibp"));

  EXPECT_FALSE(Storage.exists("/cache/page1.fibp.old"));
  EXPECT_TRUE(Storage.exists("/cache/page1.fibp"));
}

TEST_F(SdCardCacheStorageTest, BeginWriteRestoresOldWhenFinalIsMissing) {
  Storage.files["/cache/page1.fibp.old"] = "last-good-cache";
  SdCardCacheStorage s(kDir);
  EXPECT_TRUE(s.beginWrite("page1.fibp"));

  EXPECT_EQ(Storage.files["/cache/page1.fibp"], "last-good-cache");
  EXPECT_FALSE(Storage.exists("/cache/page1.fibp.old"));
  EXPECT_TRUE(Storage.exists("/cache/page1.fibp.tmp"));
}

TEST_F(SdCardCacheStorageTest, WriteFailurePreservesFinalDuringCleanup) {
  Storage.files["/cache/page1.fibp"] = "last-good-cache";
  SdCardCacheStorage s(kDir);
  ASSERT_TRUE(s.beginWrite("page1.fibp"));
  HalFile::testFailWrite = true;
  EXPECT_FALSE(s.write("bad", 3));
  HalFile::testFailWrite = false;

  EXPECT_FALSE(s.endWrite());
  EXPECT_TRUE(s.remove("page1.fibp"));
  EXPECT_EQ(Storage.files["/cache/page1.fibp"], "last-good-cache");
  EXPECT_TRUE(Storage.exists("/cache/page1.fibp.tmp"));

  ASSERT_TRUE(s.beginWrite("page1.fibp"));
  ASSERT_TRUE(s.write("retry", 5));
  ASSERT_TRUE(s.endWrite());
  EXPECT_EQ(Storage.files["/cache/page1.fibp"], "retry");
}

// ── write/readBack basics over the write handle ─────────────────────────────

TEST_F(SdCardCacheStorageTest, WriteWithoutBeginFails) {
  SdCardCacheStorage s(kDir);
  EXPECT_FALSE(s.write("x", 1));
  EXPECT_FALSE(s.endWrite());
}

TEST_F(SdCardCacheStorageTest, ReadBackAtServesWrittenBytes) {
  SdCardCacheStorage s(kDir);
  ASSERT_TRUE(s.beginWrite("page1.fibp"));
  ASSERT_TRUE(s.write("hello-cache", 11));
  char buf[6] = {};
  EXPECT_EQ(s.readBackAt(6, buf, 5), 5);
  EXPECT_EQ(std::string(buf, 5), "cache");
  // Write cursor preserved: a subsequent write appends, not overwrites.
  EXPECT_TRUE(s.write("!", 1));
  ASSERT_TRUE(s.endWrite());
  EXPECT_EQ(Storage.files["/cache/page1.fibp"], "hello-cache!");
}