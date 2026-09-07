// Reproduction test for the "Failed to index" (STR_INDEX_FAILED) notification.
//
// Failure mode being reproduced (as reported on device): the reader navigates
// from one already-parsed chapter to the next on a multi-chapter EPUB whose
// first chapter is already cached on SD (.crosspoint/epub_<hash>/sections/0.bin),
// and EpubReaderActivity::renderBook() hits showBuildError() → tr(STR_INDEX_FAILED)
// ("Failed to index") while building the next chapter's section.
//
// On device the error surfaces from these call sites in
// src/activities/reader/EpubReaderActivity.cpp:
//   * line 1351 — section->createSectionFile() returned false
//   * lines 1391, 1467 — section->startBuild() returned false
//   * lines 1405, 1460, 1477 — section->buildSomeMore() returned false
//
// This harness drives the exact same engine entry points on the host against a
// real multi-chapter EPUB (test/epubs/test_br_section_break.epub — 5 chapters,
// 9 spine items) with a per-test temp SD root, so any false return from
// startBuild()/buildSomeMore()/createSectionFile() is the STR_INDEX_FAILED
// path, reported with the matching reader call-site line.
//
// Expected end state: this test FAILS on the unfixed engine (reproducing the
// user-visible failure) and must PASS once the fix lands.
//
// ACTUAL RESULT (recorded evidence, host build): the failure does NOT
// reproduce on the host. All three scenarios below — and origin/develop at
// e3532fee via a throwaway worktree — pass in milliseconds. The STR_INDEX_FAILED
// path is therefore either device-specific (SD/SdFat + mutex, 380 KB heap
// pressure, PSRAM branch) or driven by reader-level state not modeled here
// (pendingAnchor/pendingOffsetJump, or resuming a PARTIAL section across a
// session boundary — see ResumeSuspendedPartialBuild for that scenario). Next
// evidence step: serial capture at LOG_LEVEL=2 during ch1→ch2 navigation to
// see which LOG_ERR fires.

#include <Epub.h>
#include <GfxRenderer.h>
#include <ReaderRenderSpec.h>
#include <Section.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

// Safety cap so a wedged build fails the test instead of hanging it.
constexpr int kMaxBuildChunks = 10000;

}  // namespace

class SectionIncrementalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    tmpDir_ = fs::temp_directory_path() /
              ("section_incremental_" + std::string(info->name()) + "_" + std::to_string(::getpid()));
    fs::remove_all(tmpDir_);
    fs::create_directories(tmpDir_);

    bookPath_ = (tmpDir_ / "book.epub").string();
    fs::copy_file(kEpubSource, bookPath_, fs::copy_options::overwrite_existing);

    // Cache root mirrors the SD layout: cacheDir + "/epub_<hash>".
    epub_ = std::make_shared<Epub>(bookPath_, (tmpDir_ / ".crosspoint").string());
    ASSERT_TRUE(epub_->load(/*buildIfMissing=*/true, /*skipLoadingCss=*/false)) << "Epub::load failed";

    // Mirrors the reader's render spec (default settings: embedded style on).
    ReaderRenderSpec spec{};
    spec.viewportWidth = 400;
    spec.viewportHeight = 600;
    spec.embeddedStyle = true;
    spec_ = spec;
  }

  void TearDown() override {
    epub_.reset();
    std::error_code ec;
    fs::remove_all(tmpDir_, ec);
  }

  // Drives Section exactly like EpubReaderActivity::renderBook()'s incremental
  // build loop. Returns false (and fails the test) at the same points the
  // reader shows STR_INDEX_FAILED, citing the reader call site.
  bool runIncrementalBuild(Section& section, int spineIndex, const char* startSite, const char* chunkSite) {
    if (!section.startBuild(spec_)) {
      ADD_FAILURE() << "startBuild() returned false for spine " << spineIndex
                    << " — reader path: showBuildError() → STR_INDEX_FAILED (" << startSite << ")";
      return false;
    }
    for (int chunk = 0; chunk < kMaxBuildChunks && !section.isBuildComplete(); ++chunk) {
      if (!section.buildSomeMore(/*maxPages=*/4)) {
        ADD_FAILURE() << "buildSomeMore() returned false for spine " << spineIndex << " after " << section.pageCount
                      << " pages — reader path: showBuildError() "
                      << "→ STR_INDEX_FAILED (" << chunkSite << ")";
        return false;
      }
    }
    if (!section.isBuildComplete()) {
      ADD_FAILURE() << "build for spine " << spineIndex << " did not complete within " << kMaxBuildChunks << " chunks";
      return false;
    }
    if (section.pageCount == 0) {
      ADD_FAILURE() << "build for spine " << spineIndex << " completed with 0 pages";
      return false;
    }
    return true;
  }

  // Builds a spine and destroys the Section, leaving its cache on "SD" — the
  // state after reading chapter 1 (the Section is reset on chapter change).
  bool buildAndCacheSpine(int spineIndex) {
    Section section(epub_, spineIndex, renderer_);
    if (!section.loadSectionFile(spec_)) {
      // No cache yet: build it, as the reader does.
      if (!runIncrementalBuild(section, spineIndex, "EpubReaderActivity.cpp:1391", "EpubReaderActivity.cpp:1405")) {
        return false;
      }
    }
    return true;  // ~Section → suspendBuild() persists partial state, as on device
  }

  static constexpr const char* kEpubSource = TEST_EPUB_SOURCE;

  fs::path tmpDir_;
  std::string bookPath_;
  std::shared_ptr<Epub> epub_;
  GfxRenderer renderer_;
  ReaderRenderSpec spec_{};
};

// THE reproduction: chapter 1 already built + cached on SD, user pages into
// chapter 2. The second chapter's build must succeed without hitting the
// STR_INDEX_FAILED path.
//
// NOTE: expected to FAIL on the unfixed engine — that failure IS the
// reproduced bug, observed on host with real evidence instead of theory.
// OBSERVED: passes on host (see header comment) — bug not host-reproducible
// via this path.
TEST_F(SectionIncrementalTest, NavigateFromCachedChapter1ToChapter2) {
  // Chapter 1: built and cached (previous reading session / prior pages).
  ASSERT_TRUE(buildAndCacheSpine(0));

  // Navigate to chapter 2 (spine 1). The reader resets the Section and
  // constructs a fresh one for the new spine; its cache file does not exist.
  Section nextSection(epub_, 1, renderer_);
  ASSERT_FALSE(nextSection.loadSectionFile(spec_)) << "spine 1 cache should not exist yet";

  EXPECT_TRUE(runIncrementalBuild(nextSection, 1, "EpubReaderActivity.cpp:1391", "EpubReaderActivity.cpp:1405"));
}

// One-shot persist path: pendingPercentJump forces createSectionFile()
// (EpubReaderActivity.cpp:1347→1351).
TEST_F(SectionIncrementalTest, CreateSectionFileForChapter2AfterChapter1Cached) {
  ASSERT_TRUE(buildAndCacheSpine(0));

  Section nextSection(epub_, 1, renderer_);
  ASSERT_FALSE(nextSection.loadSectionFile(spec_)) << "spine 1 cache should not exist yet";

  EXPECT_TRUE(nextSection.createSectionFile(spec_))
      << "createSectionFile() returned false — reader path: showBuildError() → "
      << "STR_INDEX_FAILED (EpubReaderActivity.cpp:1351)";
}

// Full sequential read: every chapter built chapter-by-chapter exercises the
// repeated startBuild/buildSomeMore cycles (call sites 1391/1405 and the
// partial-extension loops at 1460/1467/1477).
TEST_F(SectionIncrementalTest, SequentialBuildOfAllChapters) {
  const int spineCount = epub_->getSpineItemsCount();
  ASSERT_GT(spineCount, 1) << "test EPUB should be multi-chapter";

  for (int spine = 0; spine < spineCount; ++spine) {
    SCOPED_TRACE("spine " + std::to_string(spine));
    Section section(epub_, spine, renderer_);
    section.loadSectionFile(spec_);  // false except nothing cached yet per spine
    EXPECT_TRUE(runIncrementalBuild(section, spine, "EpubReaderActivity.cpp:1467", "EpubReaderActivity.cpp:1477"));
  }
}

// Partial-resume path: a build suspended mid-chapter is persisted by
// ~Section (suspendBuild) and must reopen as a partial the next session,
// then extend to completion. This mirrors EpubReaderActivity.cpp:1452-1471
// (the partial-extension loops calling startBuild/buildSomeMore — the
// showBuildError() sites at 1460/1467).
//
// A reduced viewport plus the largest chapters of the fixture (chapter7 is
// 1157 bytes, others 577-1078) are needed: a single parseStep can finish a
// whole ~700-byte chapter, so the test scans spines for one whose build is
// still incomplete after one buildSomeMore(1) tick — that tick's Section
// destructor then commits a genuine SECTION_FILE_PARTIAL_VERSION partial.
TEST_F(SectionIncrementalTest, ResumeSuspendedPartialBuild) {
  // Chapter 1 cached with the fixture spec, as before.
  ASSERT_TRUE(buildAndCacheSpine(0));

  ReaderRenderSpec smallSpec = spec_;
  smallSpec.viewportWidth = 120;
  smallSpec.viewportHeight = 160;

  // Build one tick per spine until one suspends mid-build (~Section commits
  // the partial); spines that complete in one tick stay finalized, harmless.
  int suspendedSpine = -1;
  const int spineCount = epub_->getSpineItemsCount();
  for (int spine = 1; spine < spineCount && suspendedSpine < 0; ++spine) {
    SCOPED_TRACE(testing::Message() << "probing spine " << spine);
    Section partial(epub_, spine, renderer_);
    ASSERT_FALSE(partial.loadSectionFile(smallSpec)) << "spine cache should not exist yet";
    ASSERT_TRUE(partial.startBuild(smallSpec))
        << "startBuild() failed — showBuildError() → STR_INDEX_FAILED (EpubReaderActivity.cpp:1391)";
    ASSERT_TRUE(partial.buildSomeMore(/*maxPages=*/1))
        << "buildSomeMore() failed on first tick — STR_INDEX_FAILED (EpubReaderActivity.cpp:1405)";
    if (!partial.isBuildComplete()) suspendedSpine = spine;
  }
  ASSERT_GE(suspendedSpine, 0) << "no chapter could be suspended mid-build even at the reduced viewport";

  Section resumed(epub_, suspendedSpine, renderer_);
  ASSERT_TRUE(resumed.loadSectionFile(smallSpec)) << "suspended build was not persisted as a partial section file";
  ASSERT_TRUE(resumed.isPartial()) << "reopened section is not marked partial";

  EXPECT_TRUE(resumed.startBuild(smallSpec))
      << "startBuild() failed to resume partial — STR_INDEX_FAILED (EpubReaderActivity.cpp:1460)";
  int ticks = 0;
  while (!resumed.isBuildComplete()) {
    ASSERT_TRUE(resumed.buildSomeMore(/*maxPages=*/4))
        << "buildSomeMore() failed while extending partial — STR_INDEX_FAILED "
        << "(EpubReaderActivity.cpp:1467)";
    ASSERT_LT(++ticks, kMaxBuildChunks);
  }
  EXPECT_GT(resumed.pageCount, 0u) << "resumed partial built no pages";
}
