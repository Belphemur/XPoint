// EpdBookFont.cpp — book::RenderFont over bundled EpdFontData (design §14.5).
// See EpdBookFont.h for the rationale. Whole unit gated by
// CROSSPOINT_TTF_READER (§14.1: no native-TTF code on PSRAM-less boards).

#if defined(CROSSPOINT_TTF_READER)

#include "EpdBookFont.h"

#include <Logging.h>

namespace freeink {
namespace book {

EpdBookFont::EpdBookFont(const EpdFontData* data) : data_(data), font_(data) {}

EpdBookFont::~EpdBookFont() { decomp_.deinit(); }

int16_t EpdBookFont::advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) {
  (void)sizePx;
  (void)styleFlags;
  const EpdGlyph* g = font_.getGlyph(codepoint);
  if (g) return static_cast<int16_t>(fp4::toPixel(g->advanceX));
  // Missing glyph: advance by the space width so words don't collapse.
  const EpdGlyph* space = font_.getGlyph(' ');
  return static_cast<int16_t>(space ? fp4::toPixel(space->advanceX) : 0);
}

int16_t EpdBookFont::lineHeight(uint16_t sizePx) {
  (void)sizePx;
  return static_cast<int16_t>(data_->advanceY);
}

int16_t EpdBookFont::ascent(uint16_t sizePx) {
  (void)sizePx;
  return static_cast<int16_t>(data_->ascender);
}

uint32_t EpdBookFont::ligature(uint32_t left, uint32_t right, uint8_t styleFlags) {
  (void)styleFlags;
  return font_.getLigature(left, right);
}

int16_t EpdBookFont::kerning(uint32_t left, uint32_t right, uint16_t sizePx, uint8_t styleFlags) {
  (void)sizePx;
  (void)styleFlags;
  const int8_t kern = font_.getKerning(left, right);
  return static_cast<int16_t>(fp4::toPixel(kern));
}

bool EpdBookFont::hasGlyph(uint32_t codepoint) const { return font_.hasCodepoint(codepoint); }

const GlyphBitmap* EpdBookFont::rasterize(uint32_t codepoint, uint16_t sizePx) {
  (void)sizePx;
  const EpdGlyph* g = font_.getGlyph(codepoint);
  if (!g) return nullptr;

  const uint32_t glyphIndex = static_cast<uint32_t>(g - data_->glyph);
  const uint8_t* packed = nullptr;
  if (data_->groups != nullptr && data_->groupCount > 0) {
    // Grouped storage: decompress the glyph's group (hot-group caching inside
    // FontDecompressor, same path GfxRenderer::getGlyphBitmap uses).
    packed = decomp_.getBitmap(data_, g, glyphIndex);
    if (!packed) {
      LOG_ERR("BFNT", "Glyph decompress failed (cp=%u)", codepoint);
      return nullptr;
    }
  } else {
    packed = &data_->bitmap[g->dataOffset];
  }

  const uint32_t needed = static_cast<uint32_t>(g->width) * static_cast<uint32_t>(g->height);
  if (!coverage_ || coverageSize_ < needed) {
    coverage_ = poolMakeBytes(needed);
    if (!coverage_) {
      LOG_ERR("BFNT", "OOM: %u bytes for glyph coverage", static_cast<unsigned>(needed));
      coverageSize_ = 0;
      return nullptr;
    }
    coverageSize_ = needed;
  }

  // Expand packed 2-bit (raw 0..3, MSB-first, 4 px/byte) or 1-bit data into
  // 8-bit coverage — same decode as GfxRenderer::drawCharImpl.
  auto* dst = coverage_.get();
  for (uint16_t y = 0; y < g->height; ++y) {
    for (uint16_t x = 0; x < g->width; ++x) {
      const uint32_t pos = static_cast<uint32_t>(y) * g->width + x;
      uint8_t cov;
      if (data_->is2Bit) {
        const uint8_t raw = static_cast<uint8_t>((packed[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3);
        cov = static_cast<uint8_t>(raw * 85u);  // 0..3 → 0..255
      } else {
        cov = static_cast<uint8_t>(((packed[pos >> 3] >> (7 - (pos & 7))) & 0x1) ? 255 : 0);
      }
      dst[pos] = cov;
    }
  }

  // PageRenderer draws at baselineY + yoff, so "above baseline" is negative.
  glyph_.pixels = dst;
  glyph_.width = g->width;
  glyph_.height = g->height;
  glyph_.xoff = g->left;
  glyph_.yoff = static_cast<int16_t>(-g->top);
  glyph_.advance = static_cast<int16_t>(fp4::toPixel(g->advanceX));
  return &glyph_;
}

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER
