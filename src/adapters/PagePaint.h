#pragma once

// PagePaint — CrossPoint-side painter for engine Page text runs (§11 Q7
// construction (a), the approved 4-level-gray parity path). Replicates
// PageRenderer::renderText's glyph loop (kerning, synthetic-bold double
// strike, underline/strikethrough arms) but routes every sample through
// GfxRenderer, so the renderer's orientation transform and the existing
// tiled dual-plane strip machinery apply — zero engine changes.
//
// Tone quantization (§13 correction 11 — the .cpfont converter banding):
//   >=144 -> tone 3 (solid ink: the BW base carries it, no plane bits)
//   >=96  -> tone 2 (dark gray: LSB + MSB)
//   >=48  -> tone 1 (light gray: MSB)
//   else 0 (no plot, no plane bits)
// The BW base plots every pixel with tone >= 1 (the tone-1 boundary), and
// the plane passes flag gray tones via grayplanes::setMsb/setLsb — exactly
// the contract GrayPlanes.h documents for the bitmap path.

#include <cstdint>

#include "layout/ChapterLayout.h"  // Page/PageTextRun
#include "render/TtfFont.h"        // FontChain

class GfxRenderer;

namespace freeink {
namespace book {

namespace pagepaint {
// 2-bit tone from 8-bit glyph coverage (0=white .. 3=black).
inline uint8_t grayTone(const uint8_t coverage) {
  if (coverage >= 144) return 3;
  if (coverage >= 96) return 2;
  if (coverage >= 48) return 1;
  return 0;
}
}  // namespace pagepaint

class PagePaint {
 public:
  // BW base pass: plots the page's text runs as solid ink (tone >= 1) in
  // logical coordinates (offset applied by the caller's own layout — engine
  // pages already carry reader-viewport coordinates). Replaces
  // PageRenderer::renderText when the dual-plane parity pass follows.
  static void paintText(const Page& page, FontChain& fonts, const GfxRenderer& renderer);

  // Dual-plane pass: call INSIDE a GRAYSCALE_DUAL beginStripTarget band.
  // Flags MSB/LSB plane bits per pixel; solid ink (tone 3) is skipped —
  // the base carries it, and plotting in the shared strip would corrupt it.
  static void paintPlanes(const Page& page, FontChain& fonts, const GfxRenderer& renderer);
};

}  // namespace book
}  // namespace freeink