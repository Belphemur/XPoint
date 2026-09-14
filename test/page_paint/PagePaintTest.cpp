// PagePaint host tests: the shared tone quantizer, base/plane consistency,
// and strip-band vs full-frame plane equivalence (the two gray transports
// must produce identical plane bits for the same page — E-Ink AA research,
// 2026-09, "full-frame versus strip" host test row).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "GfxRenderer.h"
#include "PagePaint.h"

namespace {

// A 4x4 glyph whose pixels hit every quantizer boundary; persistent storage
// (rasterize results must outlive the walk).
constexpr uint16_t kW = 4;
constexpr uint16_t kH = 4;
const uint8_t kCoverage[kW * kH] = {
    0,   42,  43,  127,  // white / light boundaries
    128, 212, 213, 255,  // dark / black boundaries
    1,   96,  144, 100,  // mid-band samples
    255, 0,   43,  128,
};

class FakeRenderFont final : public freeink::book::RenderFont {
 public:
  int16_t advance(uint32_t, uint16_t, uint8_t) override { return static_cast<int16_t>(kW); }
  int16_t lineHeight(uint16_t) override { return 16; }
  int16_t ascent(uint16_t) override { return 12; }
  bool hasGlyph(uint32_t) const override { return true; }
  bool glyphBounds(uint32_t, uint16_t, int16_t& xoff, int16_t& yoff, uint16_t& width, uint16_t& height) const override {
    xoff = 0;
    yoff = -static_cast<int16_t>(kH);
    width = kW;
    height = kH;
    return true;
  }
  const freeink::book::GlyphBitmap* rasterize(uint32_t, uint16_t) override {
    glyph_ = {kCoverage, kW, kH, 0, -static_cast<int16_t>(kH), static_cast<int16_t>(kW)};
    return &glyph_;
  }

 private:
  freeink::book::GlyphBitmap glyph_{};
};

// One two-glyph run: every sample of the coverage table lands on the page.
freeink::book::Page makeTestPage() {
  static const char kText[] = "AA";
  static freeink::book::PageTextRun run{};
  run.text = kText;
  run.charStart = 0;
  run.len = 2;
  run.x = 8;
  run.baselineY = 24;
  run.sizePx = 16;
  run.charLen = 2;
  run.styleFlags = freeink::book::StyleNone;
  run.layoutFlags = 0;
  static freeink::book::Page page{};
  page.runs = &run;
  page.runCount = 1;
  return page;  // POD struct; run/text outlive the test scope
}

bool bitAt(const uint8_t* plane, const int x, const int y) {
  return (plane[y * GfxRenderer::kStride + x / 8] & (0x80 >> (x & 7))) != 0;
}

// The two glyphs render at x 8..15, rows 20..23 (baselineY 24, yoff -4).
bool inTestGlyph(const int x, const int y, int& sampleIdx) {
  if (x < 8 || x >= 8 + 2 * static_cast<int>(kW)) return false;
  const int gy = y - 20;
  if (gy < 0 || gy >= static_cast<int>(kH)) return false;
  sampleIdx = gy * kW + ((x - 8) % kW);
  return true;
}

}  // namespace

TEST(PagePaintQuantizer, UniformBaselineBoundaries) {
  using freeink::book::pagepaint::grayTone;
  EXPECT_EQ(grayTone(0), 0);
  EXPECT_EQ(grayTone(42), 0);
  EXPECT_EQ(grayTone(43), 1);
  EXPECT_EQ(grayTone(127), 1);
  EXPECT_EQ(grayTone(128), 2);
  EXPECT_EQ(grayTone(212), 2);
  EXPECT_EQ(grayTone(213), 3);
  EXPECT_EQ(grayTone(255), 3);
}

TEST(PagePaintQuantizer, ExhaustiveCoverageMapsToValidTones) {
  using freeink::book::pagepaint::grayTone;
  uint8_t prev = 0;
  for (unsigned coverage = 0; coverage <= 255; ++coverage) {
    const uint8_t tone = grayTone(static_cast<uint8_t>(coverage));
    ASSERT_LE(tone, 3);
    ASSERT_GE(tone, prev);  // monotonic: more coverage never gets lighter
    prev = tone;
  }
  // The baseline spans all four tones.
  EXPECT_EQ(prev, 3);
}

// Base and planes must derive from ONE quantizer: base plots tone >= 1,
// MSB marks tones 1-2, LSB marks tone 2, tone 3 is base-only, tone 0 is
// untouched everywhere.
TEST(PagePaintConsistency, BaseAndPlanesAgreeWithSharedQuantizer) {
  using freeink::book::pagepaint::grayTone;
  freeink::book::FontChain chain;
  FakeRenderFont face;
  chain.add(&face, freeink::book::StyleNone);

  GfxRenderer renderer;
  const freeink::book::Page page = makeTestPage();
  freeink::book::PagePaint::paintText(page, chain, renderer);

  uint8_t lsb[GfxRenderer::kPlaneBytes] = {};
  uint8_t msb[GfxRenderer::kPlaneBytes] = {};
  renderer.beginStripTarget(lsb, 0, GfxRenderer::kPanelH, msb);
  freeink::book::PagePaint::paintPlanes(page, chain, renderer);
  renderer.endStripTarget();

  for (int y = 0; y < GfxRenderer::kPanelH; ++y) {
    for (int x = 0; x < GfxRenderer::kPanelW; ++x) {
      int sampleIdx = -1;
      const uint8_t coverage = inTestGlyph(x, y, sampleIdx) ? kCoverage[sampleIdx] : 0;
      const uint8_t tone = grayTone(coverage);
      EXPECT_EQ(bitAt(renderer.base, x, y), tone >= 1) << x << "," << y;
      EXPECT_EQ(bitAt(msb, x, y), tone == 1 || tone == 2) << x << "," << y;
      EXPECT_EQ(bitAt(lsb, x, y), tone == 2) << x << "," << y;
    }
  }
}

// The two transports must produce identical plane bits: one full-frame DUAL
// target vs the aggregate of band-wise targets (the renderer-side rotate/
// clip is shared code, so the PagePaint walk + band culling is the part
// under test here).
TEST(PagePaintEquivalence, StripBandsMatchFullFrame) {
  freeink::book::FontChain chain;
  FakeRenderFont face;
  chain.add(&face, freeink::book::StyleNone);

  GfxRenderer renderer;
  const freeink::book::Page page = makeTestPage();

  uint8_t fullLsb[GfxRenderer::kPlaneBytes] = {};
  uint8_t fullMsb[GfxRenderer::kPlaneBytes] = {};
  renderer.beginStripTarget(fullLsb, 0, GfxRenderer::kPanelH, fullMsb);
  freeink::book::PagePaint::paintPlanes(page, chain, renderer);
  renderer.endStripTarget();

  constexpr int kBandRows = 16;
  uint8_t bandLsb[GfxRenderer::kPlaneBytes] = {};
  uint8_t bandMsb[GfxRenderer::kPlaneBytes] = {};
  for (int y0 = 0; y0 < GfxRenderer::kPanelH; y0 += kBandRows) {
    uint8_t lsb[kBandRows * GfxRenderer::kStride] = {};
    uint8_t msb[kBandRows * GfxRenderer::kStride] = {};
    renderer.beginStripTarget(lsb, y0, kBandRows, msb);
    freeink::book::PagePaint::paintPlanes(page, chain, renderer);
    renderer.endStripTarget();
    std::memcpy(bandLsb + y0 * GfxRenderer::kStride, lsb, kBandRows * GfxRenderer::kStride);
    std::memcpy(bandMsb + y0 * GfxRenderer::kStride, msb, kBandRows * GfxRenderer::kStride);
  }

  EXPECT_EQ(std::memcmp(fullLsb, bandLsb, GfxRenderer::kPlaneBytes), 0);
  EXPECT_EQ(std::memcmp(fullMsb, bandMsb, GfxRenderer::kPlaneBytes), 0);
}

// The base pass (paintText) is band-independent by construction (no filter);
// assert it plots the tone >= 1 rule for the same page.
TEST(PagePaintEquivalence, BasePlotsToneOneBoundary) {
  using freeink::book::pagepaint::grayTone;
  freeink::book::FontChain chain;
  FakeRenderFont face;
  chain.add(&face, freeink::book::StyleNone);

  GfxRenderer renderer;
  const freeink::book::Page page = makeTestPage();
  freeink::book::PagePaint::paintText(page, chain, renderer);

  // The base must carry every non-white sample of the two glyphs, exactly.
  int inkSamples = 0;
  for (const uint8_t coverage : kCoverage) {
    if (grayTone(coverage) >= 1) ++inkSamples;
  }
  int basePixels = 0;
  for (const uint8_t byte : renderer.base) {
    for (int bit = 0; bit < 8; ++bit) {
      if (byte & (0x80 >> bit)) ++basePixels;
    }
  }
  EXPECT_EQ(basePixels, 2 * inkSamples);  // two glyphs, same 4x4 bitmap
}
