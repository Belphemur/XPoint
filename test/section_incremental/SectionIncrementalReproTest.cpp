// Reproduction test for the "Failed to index" (STR_INDEX_FAILED) notification
// using the real EPUB that reproduced the bug on the user's device.
//
// Source EPUB: "The Infinite and the Divine" by Robert Rath (Warhammer 40K).
// On the device, navigating from Act Three (spine[20] = 08-40k-Content-13.xhtml)
// to Chapter One (spine[8] = 08-40k-Content-1.xhtml) triggered
// STR_INDEX_FAILED. The path:
//   1. Open the book at a mid-book chapter (Act Three)
//   2. Use the TOC to jump back to Chapter One (Act One's opening)
//   3. ReaderActivity creates a fresh Section for spine[8] whose cache file
//      does not exist; loadSectionFile() returns false, then
//      createSectionFile() / startBuild() / buildSomeMore() must succeed.
//
// This test is gated on the env var CROSSPOINT_REPRO_EPUB so the real EPUB
// is NEVER committed. When the var is unset, the test is skipped.
//
// On device the error surfaces from these call sites in
// src/activities/reader/EpubReaderActivity.cpp:
//   * line 1351 — section->createSectionFile() returned false
//   * lines 1391, 1467 — section->startBuild() returned false
//   * lines 1405, 1460, 1477 — section->buildSomeMore() returned false
//
// Expected: this test FAILS on the unfixed engine (reproduces the bug)
// and must PASS once the fix lands.

#include <Epub.h>
#include <GfxRenderer.h>
#include <ReaderRenderSpec.h>
#include <Section.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr int kMaxBuildChunks = 10000;

class SectionIncrementalReproTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* reproPath = std::getenv("CROSSPOINT_REPRO_EPUB");
    if (!reproPath || reproPath[0] == '\0') {
      GTEST_SKIP() << "CROSSPOINT_REPRO_EPUB not set; skipping the real-EPUB "
                      "reproduction. Set it to the path of the real EPUB to "
                      "enable this test.";
    }
    srcEpub_ = reproPath;
    ASSERT_TRUE(fs::exists(srcEpub_)) << "CROSSPOINT_REPRO_EPUB does not exist: " << srcEpub_;

    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    tmpDir_ = fs::temp_directory_path() /
              ("section_incremental_repro_" + std::string(info->name()) + "_" + std::to_string(::getpid()));
    fs::remove_all(tmpDir_);
    fs::create_directories(tmpDir_);

    bookPath_ = (tmpDir_ / "repro.epub").string();
    fs::copy_file(srcEpub_, bookPath_, fs::copy_options::overwrite_existing);

    epub_ = std::make_shared<Epub>(bookPath_, (tmpDir_ / ".crosspoint").string());
    ASSERT_TRUE(epub_->load(/*buildIfMissing=*/true, /*skipLoadingCss=*/false)) << "Epub::load failed";

    ReaderRenderSpec spec{};
    spec.viewportWidth = 400;
    spec.viewportHeight = 600;
    spec.embeddedStyle = true;
    spec_ = spec;

    const int n = epub_->getSpineItemsCount();
    ASSERT_GE(n, 30) << "repro EPUB should be a long novel (>=30 spine items), got " << n;
  }

  void TearDown() override {
    epub_.reset();
    std::error_code ec;
    fs::remove_all(tmpDir_, ec);
  }

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

  // Builds one spine and lets ~Section persist the cache. Mirrors the reader
  // closing the chapter view (which suspends the build). The next time the
  // user navigates to this spine, loadSectionFile() will return true and the
  // build is skipped.
  bool buildAndCacheSpine(int spineIndex) {
    Section section(epub_, spineIndex, renderer_);
    if (!section.loadSectionFile(spec_)) {
      if (!runIncrementalBuild(section, spineIndex, "EpubReaderActivity.cpp:1391", "EpubReaderActivity.cpp:1405")) {
        return false;
      }
    }
    return true;
  }

  fs::path srcEpub_;
  fs::path tmpDir_;
  std::string bookPath_;
  std::shared_ptr<Epub> epub_;
  GfxRenderer renderer_;
  ReaderRenderSpec spec_{};
};

// On "The Infinite and the Divine":
//   spine[20] = 08-40k-Content-13.xhtml  (Act Three: Exterminatus — Act cover)
//   spine[21] = 08-40k-Content-14.xhtml  (Act Three, Chapter One)
//   spine[8]  = 08-40k-Content-1.xhtml   (Act One, Chapter One — first "Chapter One")
//
// User reported failure: "from Act Three to Chapter One". This test builds a
// mid-Act-Three chapter first, then navigates back to Act One's Chapter One
// (spine[8]) — the most likely interpretation of "Chapter One" since it is
// the *first* chapter of the book and the natural target of a "go back to
// the start" tap.
TEST_F(SectionIncrementalReproTest, ActThreeThenJumpBackToChapterOne) {
  // Step 1: build Act Three's content (a mid-book chapter the user is
  // currently reading). No cache yet, so loadSectionFile() returns false
  // and the reader's renderBook() does startBuild() + buildSomeMore().
  ASSERT_TRUE(buildAndCacheSpine(22))  // 08-40k-Content-15.xhtml — Act Three Ch 2
      << "Failed to build the mid-Act-Three chapter (the user's starting point)";

  // Step 2: navigate back to Chapter One (spine[8] = 08-40k-Content-1.xhtml).
  // The reader creates a fresh Section for spine[8]; its cache file does
  // not exist, so the reader must build it from the EPUB.
  Section nextSection(epub_, 8, renderer_);
  ASSERT_FALSE(nextSection.loadSectionFile(spec_))
      << "spine[8] cache should not exist (first visit after the Act Three build)";

  EXPECT_TRUE(runIncrementalBuild(nextSection, 8, "EpubReaderActivity.cpp:1391", "EpubReaderActivity.cpp:1405"))
      << "Building Act One Chapter One (spine[8]) failed after Act Three build — "
      << "this is the on-device 'Failed to index' (STR_INDEX_FAILED) path.";
}

// Same scenario, alternate interpretation: Act Three's "Chapter One"
// (spine[21]) is the *first* chapter of Act Three, and is a common navigation
// target when the user enters Act Three via TOC and then jumps to its first
// chapter.
TEST_F(SectionIncrementalReproTest, ActThreeCoverThenBuildActThreeChapterOne) {
  ASSERT_TRUE(buildAndCacheSpine(20))  // 08-40k-Content-13.xhtml — Act Three cover
      << "Failed to build Act Three cover page";

  Section nextSection(epub_, 21, renderer_);  // Act Three Chapter One
  ASSERT_FALSE(nextSection.loadSectionFile(spec_));

  EXPECT_TRUE(runIncrementalBuild(nextSection, 21, "EpubReaderActivity.cpp:1391", "EpubReaderActivity.cpp:1405"));
}

// The "go back to first Chapter One from deep in the book" case — the most
// aggressive variation of the user's reported flow. Build a chapter near the
// end of Act Three, then jump all the way back to the first chapter of the
// book.
TEST_F(SectionIncrementalReproTest, DeepInActThreeThenJumpToFirstChapter) {
  ASSERT_TRUE(buildAndCacheSpine(30))  // 08-40k-Content-23.xhtml — Act Three Ch 10
      << "Failed to build deep-Act-Three chapter";

  Section nextSection(epub_, 8, renderer_);  // Act One Chapter One
  ASSERT_FALSE(nextSection.loadSectionFile(spec_));

  EXPECT_TRUE(runIncrementalBuild(nextSection, 8, "EpubReaderActivity.cpp:1391", "EpubReaderActivity.cpp:1405"));
}

}  // namespace
