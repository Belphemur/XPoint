// TtfReaderRuntimeTest — host suites for the native-TTF reader runtime
// (Phase 2a). Covers the byte-layout contract of progress.bin records (the
// extracted single-source encoder in activities/reader/ProgressRecord.h) and
// the FreeInkBook page-cache primitives the runtime leans on:
// pageCacheName format, layoutGenerationHash behavior, and the
// pageForChar position-restore path over a synthetic charStart table
// (design §3.4/§7). Engine code is compiled from the SDK submodule; the
// storage side uses an in-memory CacheStorage fake.

#include <cache/PageCache.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "activities/reader/ProgressRecord.h"

namespace book = freeink::book;

// ── ProgressRecord byte layout (single-source encoder/decoder) ───────────────

TEST(ProgressRecordTest, BaseShapeRoundTrip) {
  uint8_t buf[progress_record::kSizeGeneration] = {};
  const size_t n =
      progress_record::encode(/*hasOffset=*/false, /*hasGeneration=*/false, 7, 12, 40, 0, 0, 0, buf, sizeof(buf));
  ASSERT_EQ(n, progress_record::kSizeBase);

  ProgressRecord rec;
  EXPECT_EQ(progress_record::decode(buf, n, rec), progress_record::kSizeBase);
  EXPECT_EQ(rec.spineIndex, 7);
  EXPECT_EQ(rec.pageNumber, 12);
  EXPECT_EQ(rec.pageCount, 40);
  EXPECT_FALSE(rec.hasOffset);
  EXPECT_FALSE(rec.hasGeneration);
}

TEST(ProgressRecordTest, OffsetShapeRoundTrip) {
  uint8_t buf[progress_record::kSizeGeneration] = {};
  const size_t n = progress_record::encode(/*hasOffset=*/true, /*hasGeneration=*/false, 3, 9, 55, 0x11223344, 0, 0, buf,
                                           sizeof(buf));
  EXPECT_EQ(n, progress_record::kSizeOffset);

  ProgressRecord rec;
  EXPECT_EQ(progress_record::decode(buf, n, rec), progress_record::kSizeOffset);
  EXPECT_EQ(rec.spineIndex, 3);
  EXPECT_EQ(rec.pageNumber, 9);
  EXPECT_EQ(rec.pageCount, 55);
  EXPECT_TRUE(rec.hasOffset);
  EXPECT_EQ(rec.visibleTextOffset, 0x11223344u);
  EXPECT_FALSE(rec.hasGeneration);
}

TEST(ProgressRecordTest, GenerationShapeRoundTrip) {
  uint8_t buf[progress_record::kSizeGeneration + 8] = {};
  const size_t n = progress_record::encode(/*hasOffset=*/false, /*hasGeneration=*/true, 2, 5, 33, 0, 0x00ABCDEF,
                                           0x87654321, buf, sizeof(buf));
  EXPECT_EQ(n, progress_record::kSizeGeneration);

  ProgressRecord rec;
  EXPECT_EQ(progress_record::decode(buf, n, rec), progress_record::kSizeGeneration);
  EXPECT_EQ(rec.spineIndex, 2);
  EXPECT_EQ(rec.pageNumber, 5);
  EXPECT_EQ(rec.pageCount, 33);
  EXPECT_FALSE(rec.hasOffset);
  EXPECT_TRUE(rec.hasGeneration);
  EXPECT_EQ(rec.charOffset, 0x00ABCDEFu);
  EXPECT_EQ(rec.generation, 0x87654321u);
}

TEST(ProgressRecordTest, GenerationShapeWinsWhenBothFlagsSet) {
  // The 16-byte selection takes priority: a caller setting both flags must
  // still get a fully written generation record (no stale stack bytes in the
  // generation slot).
  uint8_t buf[progress_record::kSizeGeneration];
  memset(buf, 0xEE, sizeof(buf));
  const size_t n = progress_record::encode(/*hasOffset=*/true, /*hasGeneration=*/true, 2, 5, 33, 0xDEAD, 0xABCDEF,
                                           0x1234, buf, sizeof(buf));
  ASSERT_EQ(n, progress_record::kSizeGeneration);
  ProgressRecord rec;
  ASSERT_EQ(progress_record::decode(buf, n, rec), progress_record::kSizeGeneration);
  EXPECT_TRUE(rec.hasGeneration);
  EXPECT_EQ(rec.charOffset, 0xABCDEFu);
  EXPECT_EQ(rec.generation, 0x1234u);
}

TEST(ProgressRecordTest, LegacyReaderDegradesGenerationRecord) {
  // A legacy reader reading a 16-byte TTF record keeps only the base triple;
  // the charOffset slot must NOT be mistaken for a visible-text offset.
  uint8_t buf[progress_record::kSizeGeneration] = {};
  (void)progress_record::encode(false, true, 9, 4, 20, 0, 777, 42, buf, sizeof(buf));

  // Simulate the legacy manager: it reads only RECORD_SIZE_OFFSET bytes.
  ProgressRecord rec;
  const size_t size = progress_record::decode(buf, progress_record::kSizeOffset, rec);
  EXPECT_EQ(size, progress_record::kSizeOffset);
  EXPECT_TRUE(rec.hasOffset);  // legacy layout mapping
  EXPECT_EQ(rec.spineIndex, 9);
  EXPECT_EQ(rec.pageNumber, 4);
  EXPECT_EQ(rec.pageCount, 20);
  // Legacy consumers treat size==kSizeOffset as "has visibleTextOffset" —
  // this documents the cross-build degrade, never a crash.
  EXPECT_EQ(rec.visibleTextOffset, 777u);
  EXPECT_FALSE(rec.hasGeneration);
}

TEST(ProgressRecordTest, MalformedAndShortRecordsDegrade) {
  ProgressRecord rec;
  EXPECT_EQ(progress_record::decode(nullptr, 0, rec), 0u);
  uint8_t buf[4] = {1, 2, 3, 4};
  EXPECT_EQ(progress_record::decode(buf, sizeof(buf), rec), 0u);
  // Truncated generation record falls back to the legacy offset shape.
  uint8_t full[progress_record::kSizeGeneration] = {};
  (void)progress_record::encode(true, true, 1, 2, 3, 99, 5, 6, full, sizeof(full));
  EXPECT_EQ(progress_record::decode(full, progress_record::kSizeBase, rec), progress_record::kSizeBase);
  EXPECT_EQ(rec.spineIndex, 1);
  // Oversized cap → encode refuses.
  EXPECT_EQ(progress_record::encode(true, false, 1, 2, 3, 0, 0, 0, buf, 4), 0u);
}

// ── PageCache primitives (engine) ────────────────────────────────────────────

TEST(PageCacheNameTest, FormatIsSpineDashHashFibp) {
  char buf[64];
  ASSERT_TRUE(book::pageCacheName(3, 0xDEADBEEF, buf, sizeof(buf)));
  EXPECT_STREQ(buf, "s0003-deadbeef.fibp");
  // Spine index uses 4-digit zero padding; hash 8 hex digits lowercase.
  ASSERT_TRUE(book::pageCacheName(0, 0, buf, sizeof(buf)));
  EXPECT_STREQ(buf, "s0000-00000000.fibp");
  ASSERT_TRUE(book::pageCacheName(65535, 0xFFFFFFFF, buf, sizeof(buf)));
  EXPECT_STREQ(buf, "s65535-ffffffff.fibp");
  // Too-small buffer refuses instead of truncating.
  char tiny[8];
  EXPECT_FALSE(book::pageCacheName(1, 1, tiny, sizeof(tiny)));
}

TEST(LayoutGenerationHashTest, HashFollowsLayoutInputs) {
  book::LayoutParams a;
  book::LayoutParams b = a;
  EXPECT_EQ(book::layoutGenerationHash(a, 0), book::layoutGenerationHash(b, 0));

  // Geometry change → different generation (cache invalidation §3.4).
  b.pageWidth = a.pageWidth + 10;
  EXPECT_NE(book::layoutGenerationHash(a, 0), book::layoutGenerationHash(b, 0));

  // Font fingerprint change → different generation.
  b = a;
  EXPECT_NE(book::layoutGenerationHash(a, 1), book::layoutGenerationHash(a, 2));

  // Base size change → different generation.
  b = a;
  b.baseSizePx = static_cast<uint16_t>(a.baseSizePx + 1);
  EXPECT_NE(book::layoutGenerationHash(a, 7), book::layoutGenerationHash(b, 7));
}

// In-memory CacheStorage double for the writer/reader round-trips.
class MemCacheStorage final : public book::CacheStorage {
 public:
  bool exists(const char* name) override { return files_.count(name) != 0; }
  bool remove(const char* name) override { return files_.erase(name) != 0; }
  int64_t fileSize(const char* name) override {
    const auto it = files_.find(name);
    return it != files_.end() ? static_cast<int64_t>(it->second.size()) : -1;
  }
  int32_t readAt(const char* name, const uint32_t offset, void* dst, const uint32_t len) override {
    const auto it = files_.find(name);
    if (it == files_.end()) return -1;
    const auto& data = it->second;
    if (offset >= data.size()) return 0;
    const uint32_t n = std::min<uint32_t>(len, static_cast<uint32_t>(data.size() - offset));
    memcpy(dst, data.data() + offset, n);
    return static_cast<int32_t>(n);
  }
  bool beginWrite(const char* name) override {
    writeName_ = name;
    writeBuf_.clear();
    return true;
  }
  bool write(const void* data, const uint32_t len) override {
    const auto* p = static_cast<const uint8_t*>(data);
    writeBuf_.insert(writeBuf_.end(), p, p + len);
    return true;
  }
  bool endWrite() override {
    files_[writeName_] = writeBuf_;
    writeBuf_.clear();
    return true;
  }
  int32_t readBackAt(const uint32_t offset, void* dst, const uint32_t len) override {
    if (offset >= writeBuf_.size()) return 0;
    const uint32_t n = std::min<uint32_t>(len, static_cast<uint32_t>(writeBuf_.size() - offset));
    memcpy(dst, writeBuf_.data() + offset, n);
    return static_cast<int32_t>(n);
  }

  std::map<std::string, std::vector<uint8_t>> files_;

 private:
  std::string writeName_;
  std::vector<uint8_t> writeBuf_;
};

namespace {

// Builds a minimal page: one run of text + charStart.
book::Page makePage(const uint32_t charStart, const uint32_t index, const char* text) {
  book::Page page{};
  static book::PageTextRun run;  // test-only storage; onPage copies the bytes
  run.text = text;
  run.len = static_cast<uint16_t>(strlen(text));
  run.x = 10;
  run.baselineY = 100;
  run.sizePx = 25;
  run.styleFlags = 0;
  run.layoutFlags = 0;
  page.runs = &run;
  page.runCount = 1;
  page.pageIndex = index;
  page.charStart = charStart;
  return page;
}

}  // namespace

TEST(PageCacheRestoreTest, PageForCharOverSyntheticCharStartTable) {
  const auto arenaBuf = std::make_unique<uint8_t[]>(64 * 1024);
  const auto scratchBuf = std::make_unique<uint8_t[]>(64 * 1024);
  book::Arena arena(arenaBuf.get(), 64 * 1024);
  book::Arena scratch(scratchBuf.get(), 64 * 1024);

  MemCacheStorage storage;
  book::PageCacheWriter writer;
  ASSERT_TRUE(writer.begin(storage, "s0-abc.fibp", 0xABCU, arena));

  // Synthetic chapter: 5 pages whose charStarts step by 100.
  static const char* texts[] = {"alpha", "beta", "gamma", "delta", "epsilon"};
  for (uint32_t i = 0; i < 5; ++i) {
    ASSERT_TRUE(writer.onPage(makePage(i * 100, i, texts[i])));
  }
  writer.setTotalChars(500);
  ASSERT_TRUE(writer.finish());
  EXPECT_EQ(writer.pageCount(), 5u);

  book::PageCacheReader reader;
  ASSERT_EQ(reader.open(storage, "s0-abc.fibp", 0xABCU, arena), book::BookStatus::Ok);
  ASSERT_EQ(reader.pageCount(), 5u);
  ASSERT_FALSE(reader.isPartial());

  // Position restore: a saved charOffset maps to the page whose charStart
  // range covers it (the primitive behind openBookTtf's restore path).
  EXPECT_EQ(reader.pageForChar(0), 0u);
  EXPECT_EQ(reader.pageForChar(99), 0u);
  EXPECT_EQ(reader.pageForChar(100), 1u);
  EXPECT_EQ(reader.pageForChar(250), 2u);
  EXPECT_EQ(reader.pageForChar(499), 4u);
  EXPECT_EQ(reader.pageForChar(999999), 4u);

  // charStart round-trip.
  for (uint32_t i = 0; i < 5; ++i) {
    EXPECT_EQ(reader.charStart(i), i * 100);
  }

  // Page decode returns the run text.
  book::Page page{};
  ASSERT_EQ(reader.readPage(2, scratch, &page), book::BookStatus::Ok);
  ASSERT_EQ(page.runCount, 1u);
  EXPECT_EQ(page.charStart, 200u);
  EXPECT_EQ(std::string(page.runs[0].text, page.runs[0].len), "gamma");

  // Generation mismatch → Stale (cache invalidation).
  book::PageCacheReader stale;
  EXPECT_EQ(stale.open(storage, "s0-abc.fibp", 0x111U, arena), book::BookStatus::Stale);
}

TEST(PageCacheRestoreTest, PartialSuspendServesBuiltPagesAndReportsPartial) {
  const auto arenaBuf = std::make_unique<uint8_t[]>(64 * 1024);
  const auto scratchBuf = std::make_unique<uint8_t[]>(64 * 1024);
  book::Arena arena(arenaBuf.get(), 64 * 1024);
  book::Arena scratch(scratchBuf.get(), 64 * 1024);

  MemCacheStorage storage;
  book::PageCacheWriter writer;
  ASSERT_TRUE(writer.begin(storage, "s1-def.fibp", 0x22U, arena));
  for (uint32_t i = 0; i < 3; ++i) {
    ASSERT_TRUE(writer.onPage(makePage(i * 50, i, "partial text")));
  }
  // Suspend commits a PARTIAL footer carrying the input-side build progress.
  ASSERT_TRUE(writer.suspend(1234, 4096));

  book::PageCacheReader reader;
  ASSERT_EQ(reader.open(storage, "s1-def.fibp", 0x22U, arena), book::BookStatus::Ok);
  EXPECT_TRUE(reader.isPartial());
  EXPECT_EQ(reader.pageCount(), 3u);
  EXPECT_EQ(reader.buildBytesConsumed(), 1234u);
  EXPECT_EQ(reader.buildBytesTotal(), 4096u);
  // A suspended partial still restores the position within its prefix.
  EXPECT_EQ(reader.pageForChar(200), 2u);
  EXPECT_EQ(reader.pageForChar(100000), 2u);  // watermark clamp, not the total
}