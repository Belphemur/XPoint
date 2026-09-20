#pragma once

// PagePaint — CrossPoint-side painter for engine Page text runs (§11 Q7
// construction (a), the approved 4-level-gray parity path). Replicates
// PageRenderer::renderText's glyph loop (kerning, synthetic-bold double
// strike, underline/strikethrough arms) but routes every sample through
// GfxRenderer, so the renderer's orientation transform and the existing
// tiled dual-plane strip machinery apply — zero engine changes.
//
// Tone quantization (E-Ink AA research, 2026-09 — the uniform baseline
// quantizer; the earlier 48/96/144 bands and the old 1/8/12 discussion
// thresholds both misplace the boundaries):
//   tone = (3*coverage + 127) / 255
//   0-42   -> tone 0 (no plot, no plane bits)
//   43-127 -> tone 1 (light gray: MSB)
//   128-212-> tone 2 (dark gray: LSB + MSB)
//   213-255-> tone 3 (solid ink: the BW base carries it, no plane bits)
// The BW base plots every pixel with tone >= 1 (the tone-1 boundary), and
// the plane passes flag gray tones via grayplanes::setMsb/setLsb — exactly
// the contract GrayPlanes.h documents for the bitmap path. This is the ONE
// quantizer for base and planes; a future calibrated profile (256-byte LUT
// from measured panel reflectance) replaces this function, never a local
// threshold copy.

#include <cstdint>

#include "layout/ChapterLayout.h"  // Page/PageTextRun
#include "render/TtfFont.h"        // FontChain

class GfxRenderer;

namespace freeink {
namespace book {

namespace pagepaint {
// 2-bit tone from 8-bit glyph coverage (0=white .. 3=black). Uniform
// baseline: equally spaced darkness levels, no gamma — stb coverage is
// linear pixel coverage, not gamma-encoded.
inline uint8_t grayTone(const uint8_t coverage) { return static_cast<uint8_t>((3u * coverage + 127u) / 255u); }
}  // namespace pagepaint

class PagePaint {
 public:
  // BW base pass: plots the page's text runs as solid ink (tone >= 1) in
  // logical coordinates (offset applied by the caller's own layout — engine
  // pages already carry reader-viewport coordinates). Replaces
  // PageRenderer::renderText when the dual-plane parity pass follows.
  static void paintText(const Page& page, FontChain& fonts, const GfxRenderer& renderer);

  // Sliced variant of paintText for the cooperative prerender pump: paints
  // from (firstRun, firstChar) and yields once `budgetMs` elapsed — the
  // cursor advances per RUN and, inside the run the budget hits, per GLYPH
  // (a single long line must not blow the 120 ms input-latency bound;
  // review r5). *nextRunOut/*nextCharOut receive the resume position: run
  // < runCount → text runs pending (nextChar = glyphs already painted in
  // that run); run >= runCount → rubies pending (index = run - runCount).
  // Returns true when everything is painted (runs, rubies AND rules are
  // the caller's job); the caller then owns a complete base pass. NOT for
  // the plane pass — the prerender paints the base only.
  static bool paintTextSliced(const Page& page, FontChain& fonts, const GfxRenderer& renderer, uint16_t firstRun,
                              uint32_t firstChar, uint16_t* nextRunOut, uint32_t* nextCharOut, uint32_t budgetMs);

  // Dual-plane pass: call INSIDE a GRAYSCALE_DUAL beginStripTarget band.
  // Flags MSB/LSB plane bits per pixel; solid ink (tone 3) is skipped —
  // the base carries it, and plotting in the shared strip would corrupt it.
  static void paintPlanes(const Page& page, FontChain& fonts, const GfxRenderer& renderer);
};

}  // namespace book
}  // namespace freeink