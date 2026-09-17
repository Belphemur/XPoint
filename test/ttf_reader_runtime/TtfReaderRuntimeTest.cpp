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
#include <array>
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

TEST(ProgressRecordTest, GenerationShapePreservesZeroGeneration) {
  uint8_t buf[progress_record::kSizeGeneration] = {};
  const size_t n =
      progress_record::encode(/*hasOffset=*/false, /*hasGeneration=*/true, 2, 5, 33, 0, 0xABCDEF, 0, buf, sizeof(buf));
  ASSERT_EQ(n, progress_record::kSizeGeneration);

  ProgressRecord rec;
  EXPECT_EQ(progress_record::decode(buf, n, rec), progress_record::kSizeGeneration);
  EXPECT_TRUE(rec.hasGeneration);
  EXPECT_EQ(rec.generation, 0u);
  EXPECT_EQ(rec.charOffset, 0xABCDEFu);
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
  // All 16 bytes are written: the reserved tail (14..15) must be zero, not
  // the caller's stack data.
  EXPECT_EQ(buf[14], 0);
  EXPECT_EQ(buf[15], 0);
  ProgressRecord rec;
  ASSERT_EQ(progress_record::decode(buf, n, rec), progress_record::kSizeGeneration);
  EXPECT_TRUE(rec.hasGeneration);
  EXPECT_FALSE(rec.hasOffset);  // generation precedence clears the offset flag
  EXPECT_EQ(rec.spineIndex, 2);
  EXPECT_EQ(rec.pageNumber, 5);
  EXPECT_EQ(rec.pageCount, 33);
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
  // A future/oversized record is not silently accepted as the current shape.
  EXPECT_EQ(progress_record::decode(full, progress_record::kSizeGeneration + 1, rec), progress_record::kSizeBase);
  EXPECT_FALSE(rec.hasGeneration);
  // Oversized cap -> encode refuses.
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
    if (failNextWrite) {
      failNextWrite = false;
      return false;
    }
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
  bool failNextWrite = false;

 private:
  std::string writeName_;
  std::vector<uint8_t> writeBuf_;
};

TEST(PageCacheWriterFailureTest, FailedBeginCanBeDiscardedAndStorageReused) {
  const auto arenaBuf = std::make_unique<uint8_t[]>(64 * 1024);
  book::Arena arena(arenaBuf.get(), 64 * 1024);
  MemCacheStorage storage;
  storage.failNextWrite = true;

  book::PageCacheWriter writer;
  EXPECT_FALSE(writer.begin(storage, "s0-fail.fibp", 1, arena));
  EXPECT_TRUE(writer.failed());
  writer.finish();
  EXPECT_FALSE(storage.exists("s0-fail.fibp"));

  book::PageCacheWriter retry;
  ASSERT_TRUE(retry.begin(storage, "s0-retry.fibp", 2, arena));
  ASSERT_TRUE(retry.finish());
  EXPECT_TRUE(storage.exists("s0-retry.fibp"));
}

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

TEST(PageCacheWriterFailureTest, FailedMidBuildCloseAllowsSameNameRetry) {
  const auto arenaBuf = std::make_unique<uint8_t[]>(64 * 1024);
  book::Arena arena(arenaBuf.get(), 64 * 1024);
  MemCacheStorage storage;

  book::PageCacheWriter writer;
  ASSERT_TRUE(writer.begin(storage, "s2-fail.fibp", 1, arena));
  // Arm the failure AFTER begin() (which streams the header): the next write
  // then fails mid-build, and finish() must close/remove the active temp and
  // return the storage to a writable state.
  storage.failNextWrite = true;
  EXPECT_FALSE(writer.onPage(makePage(0, 0, "first")));
  EXPECT_FALSE(writer.finish());
  EXPECT_TRUE(writer.failed());
  EXPECT_FALSE(storage.exists("s2-fail.fibp"));

  book::PageCacheWriter retry;
  ASSERT_TRUE(retry.begin(storage, "s2-fail.fibp", 2, arena));
  ASSERT_TRUE(retry.onPage(makePage(0, 0, "retry")));
  ASSERT_TRUE(retry.finish());
  EXPECT_TRUE(storage.exists("s2-fail.fibp"));
}

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
  ASSERT_EQ(reader.totalChars(), 500u);
  ASSERT_FALSE(reader.isPartial());

  // Position restore: a saved charOffset maps to the page whose charStart
  // range covers it (the primitive behind openBookTtf's restore path).
  EXPECT_EQ(reader.pageForChar(0), 0u);
  EXPECT_EQ(reader.pageForChar(99), 0u);
  EXPECT_EQ(reader.pageForChar(100), 1u);
  EXPECT_EQ(reader.pageForChar(250), 2u);
  EXPECT_EQ(reader.pageForChar(499), 4u);
  EXPECT_EQ(reader.pageForChar(999999), 4u);
  // pageForChar clamps beyond the chapter; callers must check totalChars()
  // before treating the result as a valid saved-position mapping.
  EXPECT_EQ(reader.pageForChar(reader.totalChars()), 4u);
  EXPECT_EQ(reader.pageForChar(reader.totalChars() + 1), 4u);

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
  writer.setTotalChars(150);
  // Suspend commits a PARTIAL footer carrying the input-side build progress.
  ASSERT_TRUE(writer.suspend(1234, 4096));

  book::PageCacheReader reader;
  ASSERT_EQ(reader.open(storage, "s1-def.fibp", 0x22U, arena), book::BookStatus::Ok);
  EXPECT_TRUE(reader.isPartial());
  EXPECT_EQ(reader.pageCount(), 3u);
  EXPECT_EQ(reader.buildBytesConsumed(), 1234u);
  EXPECT_EQ(reader.buildBytesTotal(), 4096u);
  EXPECT_EQ(reader.totalChars(), 150u);
  // A suspended partial still restores the position within its prefix.
  EXPECT_EQ(reader.pageForChar(200), 2u);
  EXPECT_EQ(reader.pageForChar(100000), 2u);  // watermark clamp, not the total
}

// ── Reading-stats word-count hypothesis tests ───────────────────────────────
//
// Device symptom: reading-speed card stuck at exactly 80 WPM (the WPM_FLOOR).
// The pace sampler rejects wordsOnPage == 0 outright (WpmWindow::record), and
// the WPM cell renders '-' when the window is empty — so a stuck 80 can only
// be produced by RECORDED samples whose wpm landed at/below the floor, i.e.
// undercounted words or inflated dwell. These tests pin the storage/tokenizer
// half of that chain: the FIBP round-trip must preserve run text verbatim so
// the page-word tokenizer in EpubReaderActivity::renderBookTtf (replicated
// below) sees every word on both fresh-build and cache-served pages.
namespace {

// Exact replica of the whitespace-token counter in
// src/activities/reader/EpubReaderActivity.cpp::renderBookTtf
// (READING_STATS_ENABLED block, "Reading-stats approximation" comment).
// Keep in sync — it is the code under test, byte for byte.
uint16_t countPageWords(const book::Page& page) {
  uint16_t words = 0;
  bool inWord = false;
  for (uint16_t r = 0; r < page.runCount; ++r) {
    const char* p = page.runs[r].text;
    for (uint16_t i = 0; i < page.runs[r].len; ++i) {
      const bool ws = p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r';
      if (!ws && !inWord) {
        ++words;
        inWord = true;
      } else if (ws) {
        inWord = false;
      }
    }
  }
  return words;
}

// A realistic rendered page: ~14 lines of prose across 5 style runs, mixed
// ASCII + multi-byte UTF-8, leading/trailing/inter-run whitespace.
struct SyntheticPage {
  std::array<std::string, 5> runText;
  std::vector<book::PageTextRun> runs;
  book::Page page{};  // zero-init: counts/pointers the writer walks must be 0

  explicit SyntheticPage(uint32_t charStart) {
    runText[0] =
        "It was a bright cold day in April, and the clocks were striking "
        "thirteen. Winston Smith, his chin nuzzled into his breast in an";
    runText[1] =
        " effort\xe2\x80\x91ful shade \xc3\xa9vit\xe9 le vent — the telex "
        "screened with garbled tales;\n  twice the messenger re-wrote it ";
    runText[2] =
        "and still the sentence would not end the way he wanted it to, the "
        "clause curling back on itself like smoke in a shut room where";
    runText[3] =
        "\tthe window had been painted black for the winter and the "
        "light came down the stairwell in measured rations,";
    runText[4] =
        " a spoonful at a time, until the landing below was only a "
        "rumour and the coat rack a silhouette of somebody waiting.";
    uint32_t chars = charStart;
    for (int i = 0; i < 5; ++i) {
      book::PageTextRun run{};
      run.text = runText[i].data();
      run.len = static_cast<uint16_t>(runText[i].size());
      run.charStart = chars;
      run.charLen = static_cast<uint16_t>(runText[i].size());
      run.x = 12;
      run.baselineY = static_cast<int16_t>(60 + i * 40);
      run.sizePx = 25;
      run.styleFlags = 0;
      run.layoutFlags = 0;
      chars += run.charLen;
      runs.push_back(run);
    }
    page.runs = runs.data();
    page.runCount = static_cast<uint16_t>(runs.size());
    page.pageIndex = 0;
    page.charStart = charStart;
  }
};

}  // namespace

TEST(FibpWordCountTest, RoundTripPreservesWordsOnCacheServedPages) {
  const auto arenaBuf = std::make_unique<uint8_t[]>(256 * 1024);
  const auto scratchBuf = std::make_unique<uint8_t[]>(256 * 1024);
  book::Arena arena(arenaBuf.get(), 256 * 1024);
  book::Arena scratch(scratchBuf.get(), 256 * 1024);

  MemCacheStorage storage;
  SyntheticPage original(0);
  const uint16_t directWords = countPageWords(original.page);
  ASSERT_GT(directWords, 100u) << "fixture must model a real prose page";

  book::PageCacheWriter writer;
  ASSERT_TRUE(writer.begin(storage, "s4-wc.fibp", 0x33U, arena));
  ASSERT_TRUE(writer.onPage(original.page));
  writer.setTotalChars(original.runText[0].size() + original.runText[1].size() + original.runText[2].size() +
                       original.runText[3].size() + original.runText[4].size());
  ASSERT_TRUE(writer.finish());

  // Cache-served path: the FIBP blob must reconstruct the runs so the
  // tokenizer sees the same words the fresh layout delivered.
  book::PageCacheReader reader;
  ASSERT_EQ(reader.open(storage, "s4-wc.fibp", 0x33U, arena), book::BookStatus::Ok);
  book::Page decoded{};
  ASSERT_EQ(reader.readPage(0, scratch, &decoded), book::BookStatus::Ok);
  ASSERT_EQ(decoded.runCount, original.page.runCount);
  EXPECT_EQ(decoded.charStart, 0u);

  const uint16_t cachedWords = countPageWords(decoded);
  EXPECT_EQ(cachedWords, directWords);
  EXPECT_GT(cachedWords, 100u);
}

TEST(FibpWordCountTest, MidBuildReadBackPreservesWords) {
  // PageCacheWriter::readPage (mid-build read-back of the open write stream)
  // must decode the same words — readers render pages while the build runs.
  const auto arenaBuf = std::make_unique<uint8_t[]>(256 * 1024);
  const auto scratchBuf = std::make_unique<uint8_t[]>(256 * 1024);
  book::Arena arena(arenaBuf.get(), 256 * 1024);
  book::Arena scratch(scratchBuf.get(), 256 * 1024);

  MemCacheStorage storage;
  SyntheticPage original(0);
  const uint16_t directWords = countPageWords(original.page);

  book::PageCacheWriter writer;
  ASSERT_TRUE(writer.begin(storage, "s5-wc.fibp", 0x44U, arena));
  ASSERT_TRUE(writer.onPage(original.page));
  writer.setTotalChars(1);

  book::Page midBuild{};
  ASSERT_EQ(writer.readPage(0, scratch, &midBuild), book::BookStatus::Ok);
  EXPECT_EQ(countPageWords(midBuild), directWords);
  EXPECT_GT(directWords, 100u);
  ASSERT_TRUE(writer.finish());
}

TEST(FibpWordCountTest, ImageOnlyPageYieldsZeroWordsAndIsRoundTripped) {
  // A page with no text runs (full-page image) counts 0 words; the pace
  // sampler rejects those (WpmWindow::record wordsOnPage==0 guard) and the
  // WPM cell would show '-' on an empty window — it must never fabricate a
  // floor value from a zero-word page.
  const auto arenaBuf = std::make_unique<uint8_t[]>(64 * 1024);
  const auto scratchBuf = std::make_unique<uint8_t[]>(64 * 1024);
  book::Arena arena(arenaBuf.get(), 64 * 1024);
  book::Arena scratch(scratchBuf.get(), 64 * 1024);

  MemCacheStorage storage;
  book::Page imagePage{};
  book::PageImage img{};
  static const char href[] = "images/cover.png";
  img.href = href;
  book::PageImage images[1] = {img};
  imagePage.images = images;
  imagePage.imageCount = 1;

  book::PageCacheWriter writer;
  ASSERT_TRUE(writer.begin(storage, "s6-img.fibp", 0x55U, arena));
  ASSERT_TRUE(writer.onPage(imagePage));
  writer.setTotalChars(0);
  ASSERT_TRUE(writer.finish());

  book::PageCacheReader reader;
  ASSERT_EQ(reader.open(storage, "s6-img.fibp", 0x55U, arena), book::BookStatus::Ok);
  book::Page decoded{};
  ASSERT_EQ(reader.readPage(0, scratch, &decoded), book::BookStatus::Ok);
  EXPECT_EQ(decoded.runCount, 0u);
  EXPECT_EQ(countPageWords(decoded), 0u);
}

TEST(FibpWordCountTest, TokenizerTreatsNonBreakingSpaceAsWordContent) {
  // Documented approximation: only ASCII whitespace splits words. U+00A0 is
  // word content (counted), matching the firmware block byte-for-byte.
  const auto arenaBuf = std::make_unique<uint8_t[]>(64 * 1024);
  book::Arena arena(arenaBuf.get(), 64 * 1024);

  SyntheticPage page(0);
  page.runText[0] = "word\xc2\xa0word word";
  page.runs[0].text = page.runText[0].data();
  page.runs[0].len = static_cast<uint16_t>(page.runText[0].size());
  page.page.runCount = 1;
  EXPECT_EQ(countPageWords(page.page), 2u);
}