#pragma once

// EpdBookFont — book::RenderFont adapter over a bundled EpdFontData
// (design §14.5). Lets the native-TTF reader chain (FontChain → layout →
// PageRenderer) fall back to the baked Atkinson bitmap fonts instead of
// NotoSans BitmapBookFont, so every face in the reader chain speaks the same
// RenderFont protocol (kerning/ligature-capable, 8-bit coverage rasterize).
//
// Metrics ignore sizePx: the baked font has one size, exactly like
// BitmapBookFont. Advances/kerning are snapped from the font's 12.4/4.4
// fixed-point fields with fp4::toPixel.
//
// Whole unit is compile-gated by CROSSPOINT_TTF_READER: on PSRAM-less builds
// this translation unit contributes nothing (zero flash/RAM cost, design
// §14.1).

#if defined(CROSSPOINT_TTF_READER)

#include <BookFont.h>
#include <EpdFont.h>
#include <EpdFontData.h>
#include <FontDecompressor.h>
#include <Memory.h>

namespace freeink {
namespace book {

class EpdBookFont : public RenderFont {
 public:
  explicit EpdBookFont(const EpdFontData* data);
  ~EpdBookFont() override;

  EpdBookFont(const EpdBookFont&) = delete;
  EpdBookFont& operator=(const EpdBookFont&) = delete;

  int16_t advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) override;
  int16_t lineHeight(uint16_t sizePx) override;
  int16_t ascent(uint16_t sizePx) override;
  uint32_t ligature(uint32_t left, uint32_t right, uint8_t styleFlags) override;
  int16_t kerning(uint32_t left, uint32_t right, uint16_t sizePx, uint8_t styleFlags) override;
  bool hasGlyph(uint32_t codepoint) const override;
  const GlyphBitmap* rasterize(uint32_t codepoint, uint16_t sizePx) override;

 private:
  const EpdFontData* data_;
  EpdFont font_;  // borrows data_; owns no memory
  FontDecompressor decomp_;
  PoolBytes coverage_;  // lazily-sized rasterize() scratch (PSRAM-backed)
  uint32_t coverageSize_ = 0;
  GlyphBitmap glyph_ = {};
};

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER
