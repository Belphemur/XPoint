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
