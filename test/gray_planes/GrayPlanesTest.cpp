#include <gtest/gtest.h>

#include "lib/GfxRenderer/GrayPlanes.h"

// Tone → plane mapping shared by renderCharImpl (full-size glyphs) and
// renderCharScaled (2x2-downsampled sup/sub/ruby glyphs). Tones are the
// font's raw 2-bit values: 0 = white, 1 = light gray, 2 = dark gray,
// 3 = black.
//
// Reference: the pre-fix renderCharImpl branch structure —
//   BW:             tone != 0 plots
//   GRAYSCALE_MSB:  tone 1|2 flags MSB, tone 3 skips (base carries it)
//   GRAYSCALE_LSB:  tone 2 flags LSB, tone 1|3 skip
// renderCharScaled previously called plain drawPixel() in the gray passes
// (superscript/subscript/ruby rendered solid black, no AA planes).

namespace {

constexpr bool BW = true;
constexpr bool GRAY = false;

TEST(GrayPlanesTest, PlaneBits) {
  EXPECT_FALSE(grayplanes::setMsb(0));
  EXPECT_TRUE(grayplanes::setMsb(1));
  EXPECT_TRUE(grayplanes::setMsb(2));
  EXPECT_FALSE(grayplanes::setMsb(3));

  EXPECT_FALSE(grayplanes::setLsb(0));
  EXPECT_FALSE(grayplanes::setLsb(1));
  EXPECT_TRUE(grayplanes::setLsb(2));
  EXPECT_FALSE(grayplanes::setLsb(3));
}

TEST(GrayPlanesTest, BlockPlanBwMatchesLegacyInkRule) {
  // Legacy rule: maxRaw >= 2 || coverage >= 2.
  EXPECT_FALSE(grayplanes::planBlock(BW, 0, 0).plot);
  EXPECT_FALSE(grayplanes::planBlock(BW, 1, 1).plot);  // one weak sample: below threshold
  EXPECT_TRUE(grayplanes::planBlock(BW, 2, 2).plot);
  EXPECT_TRUE(grayplanes::planBlock(BW, 3, 3).plot);
  // coverage 2 from two weak samples (1+1) still plots.
  EXPECT_TRUE(grayplanes::planBlock(BW, 1, 2).plot);
}

TEST(GrayPlanesTest, BlockPlanGrayscaleMirrorsPixelMapping) {
  // Solid block (maxRaw 3): skip entirely — base carries it.
  const auto solid = grayplanes::planBlock(GRAY, 3, 3);
  EXPECT_FALSE(solid.plot);
  EXPECT_FALSE(solid.msb);
  EXPECT_FALSE(solid.lsb);

  // Dark block (maxRaw 2): both planes flagged.
  const auto dark = grayplanes::planBlock(GRAY, 2, 2);
  EXPECT_FALSE(dark.plot);
  EXPECT_TRUE(dark.msb);
  EXPECT_TRUE(dark.lsb);

  // Light block (maxRaw 1 with enough coverage): MSB only.
  const auto light = grayplanes::planBlock(GRAY, 1, 2);
  EXPECT_FALSE(light.plot);
  EXPECT_TRUE(light.msb);
  EXPECT_FALSE(light.lsb);

  // Below ink threshold: nothing.
  const auto none = grayplanes::planBlock(GRAY, 1, 1);
  EXPECT_FALSE(none.plot);
  EXPECT_FALSE(none.msb);
  EXPECT_FALSE(none.lsb);
}

}  // namespace

// ── PagePaint quantization (§11 Q7 construction (a), §13 correction 11) ──────
// 8-bit engine coverage → the 2-bit GrayPlanes tones, using the .cpfont
// converter banding (>=144/96/48), NOT uniform quartiles.

#include "adapters/PagePaint.h"

TEST(PagePaintTone, ConverterBandingThresholds) {
  EXPECT_EQ(freeink::book::pagepaint::grayTone(0), 0);
  EXPECT_EQ(freeink::book::pagepaint::grayTone(47), 0);
  EXPECT_EQ(freeink::book::pagepaint::grayTone(48), 1);  // tone-1 boundary
  EXPECT_EQ(freeink::book::pagepaint::grayTone(95), 1);
  EXPECT_EQ(freeink::book::pagepaint::grayTone(96), 2);  // tone-2 boundary
  EXPECT_EQ(freeink::book::pagepaint::grayTone(143), 2);
  EXPECT_EQ(freeink::book::pagepaint::grayTone(144), 3);  // solid-ink boundary
  EXPECT_EQ(freeink::book::pagepaint::grayTone(255), 3);
}

TEST(GrayPlanesToneMapping, PagePaintTonesMatchLegacyPlaneBits) {
  using freeink::book::pagepaint::grayTone;
  for (int c = 0; c <= 255; ++c) {
    const uint8_t tone = grayTone(static_cast<uint8_t>(c));
    // The BW base plots tone >= 1; the plane pass flags MSB for tones 1|2 and
    // LSB only for tone 2 — identical to the bitmap path's raw tone values.
    if (tone == 0) continue;
    EXPECT_EQ(grayplanes::setMsb(tone), tone == 1 || tone == 2);
    EXPECT_EQ(grayplanes::setLsb(tone), tone == 2);
  }
}
