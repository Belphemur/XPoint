// PagePaint — see PagePaint.h. Gated to CROSSPOINT_TTF_READER: engine pages
// only exist on TTF builds (§14.1 compile-time split).

#if defined(CROSSPOINT_TTF_READER)

#include "PagePaint.h"

#include <GfxRenderer.h>

#include "GrayPlanes.h"

namespace freeink {
namespace book {
namespace {

// Mirror of PageRenderer's TU-local decoder (PageRenderer.cpp:96).
uint32_t decodeUtf8(const char* text, const uint32_t len, uint32_t& i) {
  const auto b0 = static_cast<uint8_t>(text[i]);
  uint32_t cp = b0;
  uint32_t extra = 0;
  if (b0 >= 0xF0) {
    cp = b0 & 0x07;
    extra = 3;
  } else if (b0 >= 0xE0) {
    cp = b0 & 0x0F;
    extra = 2;
  } else if (b0 >= 0xC0) {
    cp = b0 & 0x1F;
    extra = 1;
  }
  ++i;
  while (extra > 0 && i < len && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) {
    cp = (cp << 6) | (static_cast<uint8_t>(text[i]) & 0x3F);
    ++i;
    --extra;
  }
  return cp;
}

// Per-pixel sink: receives the page-logical pixel and its quantized tone.
using ToneSink = void (*)(void* ctx, int32_t x, int32_t y, uint8_t tone);
// Optional per-glyph cull (tiled plane bands): false skips the rasterize.
using GlyphFilter = bool (*)(void* ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1);

// The glyph walk shared by the base and plane passes — replicates
// PageRenderer::renderText exactly (kerning, fontFor fallback, synthetic-bold
// double strike, underline/strikethrough arms) but hands every sample to the
// caller's tone sink instead of writing into a FrameTarget.
void walkText(const Page& page, FontChain& fonts, void* ctx, const ToneSink sink, const GlyphFilter glyphFilter) {
  for (uint16_t r = 0; r < page.runCount; ++r) {
    const PageTextRun& run = page.runs[r];
    int32_t penX = run.x;
    uint32_t i = 0;
    uint32_t prev = 0;
    while (i < run.len) {
      const uint32_t cp = decodeUtf8(run.text, run.len, i);
      if (prev != 0) penX += fonts.kerning(prev, cp, run.sizePx, run.styleFlags);
      uint8_t faceFlags = 0;
      RenderFont* font = fonts.fontFor(cp, run.styleFlags, &faceFlags);
      const GlyphBitmap* glyph = font != nullptr ? font->rasterize(cp, run.sizePx) : nullptr;
      if (glyph != nullptr) {
        if (glyphFilter != nullptr &&
            !glyphFilter(ctx, penX + glyph->xoff, run.baselineY + glyph->yoff, penX + glyph->xoff + glyph->width,
                         run.baselineY + glyph->yoff + glyph->height)) {
          // Outside the active plane band: skip the pixel walk, keep metrics.
          penX += fonts.advance(cp, run.sizePx, run.styleFlags);
          prev = cp;
          continue;
        }
        // Synthetic bold (double-strike, +1 px) when the run wants bold but
        // no bold face is registered; advance stays the measured one.
        const int strikes = (run.styleFlags & StyleBold) != 0 && (faceFlags & StyleBold) == 0 ? 2 : 1;
        for (int s = 0; s < strikes; ++s) {
          for (uint16_t gy = 0; gy < glyph->height; ++gy) {
            const uint8_t* srcRow = glyph->pixels + static_cast<uint32_t>(gy) * glyph->width;
            const int32_t dy = run.baselineY + glyph->yoff + static_cast<int32_t>(gy);
            for (uint16_t gx = 0; gx < glyph->width; ++gx) {
              sink(ctx, penX + glyph->xoff + static_cast<int32_t>(gx) + s, dy, srcRow[gx]);
            }
          }
        }
      }
      penX += fonts.advance(cp, run.sizePx, run.styleFlags);
      prev = cp;
    }
    if (run.styleFlags & StyleUnderline) {
      const int32_t y = run.baselineY + (run.sizePx >= 24 ? 3 : 2);
      const int32_t thickness = run.sizePx >= 28 ? 2 : 1;
      for (int32_t t = 0; t < thickness; ++t) {
        for (int32_t x = run.x; x < penX; ++x) sink(ctx, x, y + t, 255);
      }
    }
    if (run.styleFlags & StyleStrikethrough) {
      const int32_t y = run.baselineY - (run.sizePx / 2);
      const int32_t thickness = run.sizePx >= 28 ? 2 : 1;
      for (int32_t t = 0; t < thickness; ++t) {
        for (int32_t x = run.x; x < penX; ++x) sink(ctx, x, y + t, 255);
      }
    }
  }
}

struct BaseCtx {
  const GfxRenderer* renderer;
  int width;
  int height;
};

// BW base: tone >= 1 plots solid ink (the tone-1 boundary is the base rule).
void plotBase(void* ctx, const int32_t x, const int32_t y, const uint8_t coverage) {
  auto* self = static_cast<BaseCtx*>(ctx);
  if (pagepaint::grayTone(coverage) < 1) return;
  if (x < 0 || y < 0 || x >= self->width || y >= self->height) return;  // keep drawPixel's range quiet
  self->renderer->drawPixel(static_cast<int>(x), static_cast<int>(y), true);
}

struct PlaneCtx {
  const GfxRenderer* renderer;
};

// Dual-plane pass: tone 1 flags MSB, tone 2 flags MSB+LSB; tone 3 is the
// base's job (GrayPlanes.h: plotting solid ink in the shared strip corrupts
// the base) and tone 0 stays untouched.
void plotPlanes(void* ctx, const int32_t x, const int32_t y, const uint8_t coverage) {
  auto* self = static_cast<PlaneCtx*>(ctx);
  const uint8_t tone = pagepaint::grayTone(coverage);
  if (tone < 1 || tone == 3) return;
  self->renderer->drawGrayDualPixel(static_cast<int>(x), static_cast<int>(y),
                                    /*msb=*/true, /*lsb=*/tone == 2);
}

}  // namespace

void PagePaint::paintText(const Page& page, FontChain& fonts, const GfxRenderer& renderer) {
  BaseCtx ctx{&renderer, renderer.getScreenWidth(), renderer.getScreenHeight()};
  walkText(page, fonts, &ctx, plotBase, nullptr);
}

void PagePaint::paintPlanes(const Page& page, FontChain& fonts, const GfxRenderer& renderer) {
  PlaneCtx ctx{&renderer};
  // Band culling mirrors the legacy tiled walk (GfxRenderer.cpp): glyphs
  // entirely outside the active strip skip their rasterize entirely.
  walkText(page, fonts, &ctx, plotPlanes,
           [](void* c, const int32_t x0, const int32_t y0, const int32_t x1, const int32_t y1) {
             auto* self = static_cast<PlaneCtx*>(c);
             return self->renderer->glyphIntersectsStrip(static_cast<int>(x0), static_cast<int>(y0),
                                                         static_cast<int>(x1), static_cast<int>(y1));
           });
}

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER