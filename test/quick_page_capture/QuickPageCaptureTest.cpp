// QuickPageCapture host tests: a captured page must reproduce the page the
// quick font sheet's inline paint used to draw (same record fields, same
// string bytes, same rendered frame), so painting once after the ChapterLayout
// scan is pixel-identical to the paint-every-page behavior it replaces
// (issue #137). Also covers overwrite semantics and overflow rejection.

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BookArena.h"
#include "BookStorage.h"
#include "activities/reader/QuickPageCapture.h"
#include "epub/PackageParsers.h"
#include "epub/ZipCatalog.h"
#include "layout/ChapterLayout.h"
#include "render/PageRenderer.h"
#include "render/TtfFont.h"

#ifndef TESTDATA_DIR
#define TESTDATA_DIR "."
#endif

namespace {

class FileSource final : public freeink::book::BookSource {
 public:
  explicit FileSource(const std::string& path) : f_(fopen(path.c_str(), "rb")) {
    if (f_ == nullptr) return;
    fseek(f_, 0, SEEK_END);
    size_ = ftell(f_);
    fseek(f_, 0, SEEK_SET);
  }
  ~FileSource() override {
    if (f_ != nullptr) fclose(f_);
  }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (f_ == nullptr) return 0;
    fseek(f_, static_cast<long>(offset), SEEK_SET);
    return static_cast<int32_t>(fread(dst, 1, len, f_));
  }
  uint64_t size() const override { return size_; }

 private:
  FILE* f_ = nullptr;
  uint64_t size_ = 0;
};

constexpr int kFrameW = 480;
constexpr int kFrameH = 800;

freeink::book::FrameTarget makeTarget(uint8_t* fb) {
  freeink::book::FrameTarget target{};
  target.framebuffer = fb;
  target.width = kFrameW;
  target.height = kFrameH;
  target.widthBytes = kFrameW / 8;
  target.format = freeink::book::FrameFormat::Mono1Dithered;
  target.rotation = freeink::book::FrameRotation::Portrait;
  return target;
}

// Opens the fixture book and picks its largest text spine item.
class BookFixture {
 public:
  explicit BookFixture(const std::string& epubPath) : source_(epubPath) {
    bookArenaBytes_.resize(256 * 1024);
    bookArena_ = freeink::book::Arena(bookArenaBytes_.data(), bookArenaBytes_.size());
    if (freeink::book::ZipCatalog{}.open(source_, bookArena_) != freeink::book::BookStatus::Ok) return;
    zip_.open(source_, bookArena_);
    const auto* container = zip_.find("META-INF/container.xml");
    const char* opfPath = nullptr;
    std::vector<uint8_t> scratchBytes(64 * 1024);
    freeink::book::Arena scratch(scratchBytes.data(), scratchBytes.size());
    if (container == nullptr ||
        freeink::book::parseContainer(source_, *container, bookArena_, scratch, &opfPath) !=
            freeink::book::BookStatus::Ok) {
      return;
    }
    char opfDir[256];
    freeink::book::dirName(opfPath, opfDir, sizeof(opfDir));
    const auto* opf = zip_.find(opfPath);
    freeink::book::PackageResult pkg;
    if (opf == nullptr ||
        freeink::book::parsePackage(source_, *opf, opfDir, bookArena_, scratch, &pkg) !=
            freeink::book::BookStatus::Ok) {
      return;
    }
    uint32_t bestSize = 0;
    for (size_t i = 0; i < pkg.spineCount; ++i) {
      const auto& item = pkg.manifest[pkg.spine[i]];
      const auto* e = zip_.find(item.href);
      if (e != nullptr && e->uncompressedSize > bestSize) {
        bestSize = e->uncompressedSize;
        entry_ = e;
        href_ = item.href;
      }
    }
  }

  freeink::book::BookSource& source() { return source_; }
  freeink::book::ZipCatalog& zip() { return zip_; }
  const freeink::book::ZipEntry* entry() const { return entry_; }
  const char* href() const { return href_.c_str(); }

 private:
  FileSource source_;
  std::vector<uint8_t> bookArenaBytes_;
  freeink::book::Arena bookArena_{};
  freeink::book::ZipCatalog zip_{};
  const freeink::book::ZipEntry* entry_ = nullptr;
  std::string href_;
};

freeink::book::LayoutParams makeParams(freeink::book::FontChain& chain) {
  freeink::book::LayoutParams params;
  params.pageWidth = kFrameW;
  params.pageHeight = kFrameH;
  params.baseSizePx = 18;
  params.font = &chain;
  return params;
}

// Captures every delivered page (the quick sheet's scan behavior).
class CaptureSink final : public freeink::book::PageSink {
 public:
  CaptureSink(QuickPageCapture& capture, uint32_t stopAfter) : capture_(capture), stopAfter_(stopAfter) {}
  bool onPage(const freeink::book::Page& page) override {
    ++pages;
    allCaptured_ = capture_.capture(page) && allCaptured_;
    return page.pageIndex + 1 < stopAfter_;
  }
  uint32_t pages = 0;
  bool allCaptured() const { return allCaptured_; }

 private:
  QuickPageCapture& capture_;
  uint32_t stopAfter_;
  bool allCaptured_ = true;
};

// Paints every delivered page (the pre-#137 behavior; the frame ends on the
// last page).
class InlinePaintSink final : public freeink::book::PageSink {
 public:
  InlinePaintSink(freeink::book::FontChain& chain, const freeink::book::FrameTarget& target, uint32_t stopAfter)
      : chain_(chain), target_(target), stopAfter_(stopAfter) {}
  bool onPage(const freeink::book::Page& page) override {
    ++pages;
    std::memset(target_.framebuffer, 0xFF, static_cast<size_t>(target_.widthBytes) * target_.height);
    freeink::book::PageRenderer::renderText(page, chain_, target_, nullptr);
    return page.pageIndex + 1 < stopAfter_;
  }
  uint32_t pages = 0;

 private:
  freeink::book::FontChain& chain_;
  freeink::book::FrameTarget target_;
  uint32_t stopAfter_;
};

// Compares the captured copy against the live page while both are valid.
class FidelitySink final : public freeink::book::PageSink {
 public:
  explicit FidelitySink(QuickPageCapture& capture) : capture_(capture) {}
  bool onPage(const freeink::book::Page& page) override {
    ++pages;
    lastCaptured_ = capture_.capture(page);
    if (lastCaptured_) verify(page);
    return true;
  }
  void verify(const freeink::book::Page& page) {
    const freeink::book::Page& got = capture_.page();
    EXPECT_EQ(got.pageIndex, page.pageIndex);
    EXPECT_EQ(got.charStart, page.charStart);
    ASSERT_EQ(got.runCount, page.runCount);
    for (uint16_t i = 0; i < page.runCount; ++i) {
      EXPECT_EQ(got.runs[i].charStart, page.runs[i].charStart);
      EXPECT_EQ(got.runs[i].len, page.runs[i].len);
      EXPECT_EQ(got.runs[i].x, page.runs[i].x);
      EXPECT_EQ(got.runs[i].baselineY, page.runs[i].baselineY);
      EXPECT_EQ(got.runs[i].sizePx, page.runs[i].sizePx);
      EXPECT_EQ(got.runs[i].styleFlags, page.runs[i].styleFlags);
      EXPECT_EQ(got.runs[i].layoutFlags, page.runs[i].layoutFlags);
      EXPECT_NE(got.runs[i].text, page.runs[i].text);  // rebased, not aliased
      EXPECT_EQ(0, memcmp(got.runs[i].text, page.runs[i].text, page.runs[i].len));
    }
    EXPECT_EQ(got.linkCount, page.linkCount);
    for (uint16_t i = 0; i < page.linkCount; ++i) {
      EXPECT_STREQ(got.links[i].target, page.links[i].target);
      EXPECT_EQ(got.links[i].x, page.links[i].x);
      if (page.links[i].fragment != nullptr) {
        EXPECT_STREQ(got.links[i].fragment, page.links[i].fragment);
      } else {
        EXPECT_EQ(got.links[i].fragment, nullptr);
      }
    }
    EXPECT_EQ(got.imageCount, page.imageCount);
    for (uint16_t i = 0; i < page.imageCount; ++i) {
      EXPECT_STREQ(got.images[i].href, page.images[i].href);
      EXPECT_EQ(got.images[i].x, page.images[i].x);
      EXPECT_EQ(got.images[i].width, page.images[i].width);
    }
    EXPECT_EQ(got.ruleCount, page.ruleCount);
    for (uint16_t i = 0; i < page.ruleCount; ++i) {
      EXPECT_EQ(0, memcmp(&got.rules[i], &page.rules[i], sizeof(freeink::book::PageRule)));
    }
    EXPECT_EQ(got.rubyCount, page.rubyCount);
    for (uint16_t i = 0; i < page.rubyCount; ++i) {
      EXPECT_STREQ(got.rubies[i].text, page.rubies[i].text);
      EXPECT_EQ(got.rubies[i].baselineY, page.rubies[i].baselineY);
    }
  }
  uint32_t pages = 0;
  bool lastCaptured() const { return lastCaptured_; }

 private:
  QuickPageCapture& capture_;
  bool lastCaptured_ = false;
};

class QuickPageCaptureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    book_ = std::make_unique<BookFixture>(std::string(TESTDATA_DIR) + "/epubs/font-prewarm-benchmark.epub");
    ASSERT_NE(book_->entry(), nullptr);
    glyphBytes_.resize(256 * 1024);
    glyphArena_ = freeink::book::Arena(glyphBytes_.data(), glyphBytes_.size());
    fontData_ = readFont();
    ASSERT_FALSE(fontData_.empty());
    ASSERT_TRUE(font_.init(fontData_.data(), static_cast<uint32_t>(fontData_.size()), glyphArena_));
    chain_.add(&font_, freeink::book::StyleNone);

    captureBuf_.resize(QuickPageCapture::kBufferBytes);
    capture_.attach(captureBuf_.data(), captureBuf_.size());

    scratchBytes_.resize(256 * 1024);
    parseBytes_.resize(128 * 1024);
    scratch_ = freeink::book::Arena(scratchBytes_.data(), scratchBytes_.size());
    parse_ = freeink::book::Arena(parseBytes_.data(), parseBytes_.size());
    fbA_.resize(static_cast<size_t>(kFrameW / 8) * kFrameH);
    fbB_.resize(fbA_.size());
  }

  std::vector<uint8_t> readFont() const {
    FILE* f = fopen((std::string(TESTDATA_DIR) + "/fixtures/fonts/amazon-ember/Amazon_Ember_Regular.ttf").c_str(), "rb");
    if (f == nullptr) return {};
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(static_cast<size_t>(len));
    const size_t got = fread(data.data(), 1, data.size(), f);
    fclose(f);
    data.resize(got);
    return data;
  }

  // Runs one full layout pass with the given sink; arenas reset before return.
  void layout(freeink::book::PageSink& sink) {
    freeink::book::LayoutParams params = makeParams(chain_);
    freeink::book::ChapterLayout::layout(book_->source(), book_->zip(), *book_->entry(), book_->href(), params,
                                         scratch_, sink, nullptr, nullptr, &parse_);
    scratch_.reset();
    parse_.reset();
  }

  std::unique_ptr<BookFixture> book_;
  std::vector<uint8_t> glyphBytes_;
  freeink::book::Arena glyphArena_{};
  std::vector<uint8_t> fontData_;
  freeink::book::TtfFont font_;
  freeink::book::FontChain chain_;
  QuickPageCapture capture_;
  std::vector<uint8_t> captureBuf_;
  std::vector<uint8_t> scratchBytes_;
  std::vector<uint8_t> parseBytes_;
  freeink::book::Arena scratch_{};
  freeink::book::Arena parse_{};
  std::vector<uint8_t> fbA_;
  std::vector<uint8_t> fbB_;
};

}  // namespace

TEST_F(QuickPageCaptureTest, CapturedPagePaintMatchesInlinePaint) {
  // Pass A: capture every page, paint only the final capture after the scan.
  CaptureSink sinkA(capture_, UINT32_MAX);
  layout(sinkA);
  ASSERT_GT(sinkA.pages, 1U);  // multi-page chapter so "last page" is meaningful
  ASSERT_TRUE(sinkA.allCaptured());
  ASSERT_TRUE(capture_.ready());
  const auto targetA = makeTarget(fbA_.data());
  std::memset(fbA_.data(), 0xFF, fbA_.size());
  freeink::book::PageRenderer::renderText(capture_.page(), chain_, targetA, nullptr);
  const uint32_t capturedIndex = capture_.page().pageIndex;

  // Pass B: paint every page inline; the frame ends on the same last page.
  const auto targetB = makeTarget(fbB_.data());
  InlinePaintSink sinkB(chain_, targetB, UINT32_MAX);
  layout(sinkB);
  ASSERT_GT(sinkB.pages, 1U);
  EXPECT_EQ(capturedIndex, sinkB.pages - 1);
  EXPECT_EQ(0, memcmp(fbA_.data(), fbB_.data(), fbA_.size()));
}

TEST_F(QuickPageCaptureTest, CaptureMatchesLivePageFields) {
  FidelitySink sink(capture_);
  layout(sink);
  ASSERT_GT(sink.pages, 1U);
  EXPECT_TRUE(sink.lastCaptured());
}

TEST_F(QuickPageCaptureTest, CaptureOverwritesAndResetClears) {
  CaptureSink sink(capture_, 3);  // delivers pages 0..2, then stops
  layout(sink);
  EXPECT_EQ(sink.pages, 3U);
  ASSERT_TRUE(capture_.ready());
  EXPECT_EQ(capture_.page().pageIndex, 2U);
  const uint32_t firstCharStart = capture_.page().charStart;

  capture_.reset();
  EXPECT_FALSE(capture_.ready());

  CaptureSink sink2(capture_, UINT32_MAX);
  layout(sink2);
  ASSERT_TRUE(capture_.ready());
  // A later pass must overwrite the previous capture entirely.
  EXPECT_GT(capture_.page().pageIndex, 2U);
  EXPECT_GE(capture_.page().charStart, firstCharStart);
}

TEST_F(QuickPageCaptureTest, CaptureRejectsUndersizedBuffer) {
  // A cap without the string region cannot hold any real page.
  capture_.attach(captureBuf_.data(), QuickPageCapture::kStringsOff);
  CaptureSink sink(capture_, UINT32_MAX);
  layout(sink);
  ASSERT_GT(sink.pages, 0U);
  EXPECT_FALSE(sink.allCaptured());
  EXPECT_FALSE(capture_.ready());
}
