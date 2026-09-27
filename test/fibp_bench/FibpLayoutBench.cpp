// FIBP layout benchmark — measures the chapter-index build path end to end:
// EPUB open -> XHTML parse -> line measurement -> page placement, using the
// real FreeType backend (FtFont). Run manually (not part of the ctest gate):
//
//   cmake -S test -B build/bench -G Ninja -DCROSSPOINT_BUILD_BENCH=ON
//   cmake --build build/bench --target FibpLayoutBench
//   build/bench/fibp_bench/FibpLayoutBench [epub] [font] [iters]
//
// Defaults: the committed font-prewarm benchmark EPUB and Amazon Ember.
// With FIBP_BENCH_CACHE=1 an additional pass wraps the font in a per-glyph
// metrics memo, which quantifies the headroom a metrics cache would recover
// (the layout algorithm is unchanged).
//
// Output is wall-clock ms per layout pass plus pages and per-page cost. The
// number that matters when comparing font backends or engine revisions is the
// ratio, not the absolute host time.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "BookArena.h"
#include "BookStorage.h"
#include "FtFont.h"
#include "epub/PackageParsers.h"
#include "epub/ZipCatalog.h"
#include "layout/ChapterLayout.h"

#ifndef TESTDATA_DIR
#define TESTDATA_DIR "."
#endif

namespace {

using freeink::book::Arena;
using freeink::book::BookFont;
using freeink::book::BookSource;
using freeink::book::BookStatus;
using freeink::book::ChapterLayout;
using freeink::book::LayoutParams;
using freeink::book::Page;
using freeink::book::PageSink;
using freeink::book::ZipCatalog;
using freeink::book::ZipEntry;

class FileSource final : public BookSource {
 public:
  // RAII ownership: fclose via deleter, early-return constructor failures
  // leave the handle null without manual fclose discipline.
  static int kFclose(std::FILE* f) { return std::fclose(f); }

  explicit FileSource(const std::string& path) : f_(std::fopen(path.c_str(), "rb"), kFclose) {
    if (f_ == nullptr) return;
    if (std::fseek(f_.get(), 0, SEEK_END) != 0) {
      f_.reset();
      return;
    }
    const long end = std::ftell(f_.get());
    if (end < 0 || std::fseek(f_.get(), 0, SEEK_SET) != 0) {
      f_.reset();
      return;
    }
    size_ = static_cast<uint64_t>(end);
  }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (f_ == nullptr) return -1;
    if (std::fseek(f_.get(), static_cast<long>(offset), SEEK_SET) != 0) return -1;
    const size_t got = std::fread(dst, 1, len, f_.get());
    // BookSource's contract: negative = I/O error, 0 = clean EOF. A host
    // filesystem failure must not masquerade as end-of-archive.
    return std::ferror(f_.get()) ? -1 : static_cast<int32_t>(got);
  }
  uint64_t size() const override { return size_; }

 private:
  std::unique_ptr<std::FILE, int (*)(std::FILE*)> f_{nullptr, kFclose};
  uint64_t size_ = 0;
};

// Opens the fixture EPUB and selects its largest uncompressed text spine item.
class BookFixture {
 public:
  explicit BookFixture(const std::string& epubPath) : source_(epubPath) {
    if (!source_.size()) return;
    bookArenaBytes_.resize(256 * 1024);
    bookArena_ = Arena(bookArenaBytes_.data(), bookArenaBytes_.size());
    // Single open: ZipCatalog::open() validates the container AND allocates
    // its entry table from the arena, so a throwaway probe open would
    // permanently consume that arena a second time before the real one.
    if (zip_.open(source_, bookArena_) != BookStatus::Ok) return;
    const auto* container = zip_.find("META-INF/container.xml");
    const char* opfPath = nullptr;
    std::vector<uint8_t> scratchBytes(64 * 1024);
    Arena scratch(scratchBytes.data(), scratchBytes.size());
    if (container == nullptr ||
        freeink::book::parseContainer(source_, *container, bookArena_, scratch, &opfPath) != BookStatus::Ok) {
      return;
    }
    char opfDir[256];
    freeink::book::dirName(opfPath, opfDir, sizeof(opfDir));
    const auto* opf = zip_.find(opfPath);
    freeink::book::PackageResult pkg;
    if (opf == nullptr ||
        freeink::book::parsePackage(source_, *opf, opfDir, bookArena_, scratch, &pkg) != BookStatus::Ok) {
      return;
    }
    uint32_t best = 0;
    for (size_t i = 0; i < pkg.spineCount; ++i) {
      const auto& item = pkg.manifest[pkg.spine[i]];
      const auto* e = zip_.find(item.href);
      if (e != nullptr && e->uncompressedSize > best) {
        best = e->uncompressedSize;
        entry_ = e;
        href_ = item.href;
      }
    }
  }

  BookSource& source() { return source_; }
  ZipCatalog& zip() { return zip_; }
  const ZipEntry* entry() const { return entry_; }
  const char* href() const { return href_.c_str(); }

 private:
  FileSource source_;
  std::vector<uint8_t> bookArenaBytes_;
  Arena bookArena_{};
  ZipCatalog zip_{};
  const ZipEntry* entry_ = nullptr;
  std::string href_;
};

class CountingSink final : public PageSink {
 public:
  bool onPage(const Page& page) override {
    ++pages;
    words += page.wordCount;
    return true;
  }
  uint32_t pages = 0;
  uint64_t words = 0;
};

// Memoizes Font metrics by (codepoint, sizePx, styleFlags). Used only with
// FIBP_BENCH_CACHE=1 to measure how much of the layout pass is font metrics.
class CachingFont final : public BookFont {
 public:
  explicit CachingFont(BookFont& inner) : inner_(inner) {}
  int16_t advance(uint32_t cp, uint16_t sizePx, uint8_t flags) override {
    const uint64_t key = (uint64_t(cp) << 24) | (uint64_t(sizePx) << 8) | flags;
    const auto it = adv_.find(key);
    if (it != adv_.end()) return it->second;
    const int16_t v = inner_.advance(cp, sizePx, flags);
    adv_.emplace(key, v);
    return v;
  }
  int16_t lineHeight(uint16_t sizePx) override { return inner_.lineHeight(sizePx); }
  int16_t ascent(uint16_t sizePx) override { return inner_.ascent(sizePx); }
  uint32_t ligature(uint32_t l, uint32_t r, uint8_t flags) override { return inner_.ligature(l, r, flags); }
  int16_t kerning(uint32_t l, uint32_t r, uint16_t sizePx, uint8_t flags) override {
    // Injective 128-bit key: codepoints in a, size+flags in b. An XOR mix of
    // these fields collides (overlapping shifts), returning another pair's
    // kerning for sizePx >= 256 and skewing the speedup comparison.
    const KernKey key{uint64_t(l) | (uint64_t(r) << 32), uint64_t(sizePx) | (uint64_t(flags) << 16)};
    const auto it = kern_.find(key);
    if (it != kern_.end()) return it->second;
    const int16_t v = inner_.kerning(l, r, sizePx, flags);
    kern_.emplace(key, v);
    return v;
  }
  bool covers(uint32_t cp) override { return inner_.covers(cp); }

  // Injective 128-bit key: codepoints in a, px size + style flags in b.
  // The previous 64-bit XOR mix had overlapping shifts and collided for
  // large glyph IDs / sizes, returning another pair's kerning.
  struct KernKey {
    uint64_t a;  // l in low 32, r in high 32
    uint64_t b;  // sizePx in low 16, flags in bits 16-23
    bool operator==(const KernKey& o) const { return a == o.a && b == o.b; }
  };
  struct KernKeyHash {
    size_t operator()(const KernKey& k) const {
      return static_cast<size_t>(k.a * 0x9E3779B97F4A7C15ull ^ (k.b * 0xC2B2AE3D27D4EB4Full));
    }
  };

 private:
  BookFont& inner_;
  std::unordered_map<uint64_t, int16_t> adv_;
  std::unordered_map<KernKey, int16_t, KernKeyHash> kern_;
};

std::vector<uint8_t> readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return {};
  if (std::fseek(f, 0, SEEK_END) != 0) {
    std::fclose(f);
    return {};
  }
  const long len = std::ftell(f);
  if (len < 0 || std::fseek(f, 0, SEEK_SET) != 0) {
    std::fclose(f);
    return {};
  }
  std::vector<uint8_t> data(static_cast<size_t>(len));
  const size_t got = std::fread(data.data(), 1, data.size(), f);
  std::fclose(f);
  data.resize(got);
  return data;
}

double nowMs() {
  using Clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

// One full layout pass over the fixture's largest chapter. Fresh arenas so
// repeated passes cannot drift; returns the page count via sink.
void layoutOnce(BookFixture& book, BookFont& font, std::vector<uint8_t>& scratchBytes, std::vector<uint8_t>& parseBytes,
                CountingSink& sink) {
  LayoutParams params;
  params.pageWidth = 480;
  params.pageHeight = 800;
  params.baseSizePx = 18;
  params.font = &font;
  Arena scratch(scratchBytes.data(), scratchBytes.size());
  Arena parse(parseBytes.data(), parseBytes.size());
  const BookStatus st = ChapterLayout::layout(book.source(), book.zip(), *book.entry(), book.href(), params, scratch,
                                              sink, nullptr, nullptr, &parse);
  if (st != BookStatus::Ok) {
    std::fprintf(stderr, "layout failed: %s\n", freeink::book::bookStatusName(st));
    std::exit(1);
  }
}

double timePasses(BookFixture& book, BookFont& font, int iters, CountingSink& sinkOut) {
  std::vector<uint8_t> scratchBytes(256 * 1024);
  std::vector<uint8_t> parseBytes(128 * 1024);
  double total = 0;
  for (int i = 0; i < iters; ++i) {
    CountingSink sink;
    const double t0 = nowMs();
    layoutOnce(book, font, scratchBytes, parseBytes, sink);
    total += nowMs() - t0;
    sinkOut = sink;
  }
  return total / iters;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string epub =
      (argc > 1 && argv[1][0]) ? argv[1] : std::string(TESTDATA_DIR) + "/epubs/font-prewarm-benchmark.epub";
  const std::string fontPath =
      (argc > 2 && argv[2][0]) ? argv[2]
                               : std::string(TESTDATA_DIR) + "/fixtures/fonts/amazon-ember/Amazon_Ember_Regular.ttf";
  // One or more passes; garbage or non-positive input degrades to 1 rather
  // than dividing the accumulated time by zero.
  const long parsedIters = argc > 3 ? std::strtol(argv[3], nullptr, 10) : 3;
  const int iters = parsedIters < 1 ? 1 : static_cast<int>(parsedIters);

  BookFixture book(epub);
  if (book.entry() == nullptr) {
    std::fprintf(stderr, "cannot open fixture: %s\n", epub.c_str());
    return 1;
  }
  const std::vector<uint8_t> fontData = readFile(fontPath.c_str());
  if (fontData.empty()) {
    std::fprintf(stderr, "cannot read font: %s\n", fontPath.c_str());
    return 1;
  }

  freeink::font::FtFont font;
  if (!font.init(fontData.data(), static_cast<uint32_t>(fontData.size()), 18)) {
    std::fprintf(stderr, "FtFont init failed: %s\n", fontPath.c_str());
    return 1;
  }

  CountingSink result;
  const double baseline = timePasses(book, font, iters, result);
  std::printf("epub:  %s\nfont:  %s\nchapter: %s (%u pages, %llu words)\n", epub.c_str(), fontPath.c_str(), book.href(),
              result.pages, static_cast<unsigned long long>(result.words));
  std::printf("  FtFont layout            : %8.2f ms/pass  (%.3f ms/page)\n", baseline,
              result.pages ? baseline / result.pages : 0.0);

  if (std::getenv("FIBP_BENCH_CACHE") != nullptr) {
    CachingFont cached(font);
    CountingSink cachedResult;
    const double withCache = timePasses(book, cached, iters, cachedResult);
    std::printf("  + metrics memo (headroom): %8.2f ms/pass  (%.3f ms/page)\n", withCache,
                cachedResult.pages ? withCache / cachedResult.pages : 0.0);
    std::printf("  speedup ceiling          : %6.2fx\n", withCache > 0 ? baseline / withCache : 0.0);
  }
  return 0;
}
