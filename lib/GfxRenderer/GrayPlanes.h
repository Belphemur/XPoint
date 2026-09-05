#pragma once

#include <stdint.h>

// Tone-to-plane mapping for the 2-bit (4-level) reader fonts, shared by the
// glyph renderers in GfxRenderer.cpp. Tones are the font's raw values:
// 0 = white, 1 = light gray, 2 = dark gray, 3 = black.
//
// A page turn renders the BW base first; the grayscale passes then flag plane
// bits per pixel (LSB = dark-gray plane, MSB = light-gray plane) and
// displayGray() drives the panel's AA waveform, which turns the planes into
// the single mid-gray edge tone. Solid ink (tone 3) never sets plane bits —
// the B/W base already carries it, and in the tiled path the plane passes
// share one scratch strip, so plotting there would corrupt the base.
//
// All helpers take RAW font tones (0 = white .. 3 = black), the encoding in
// the glyph bitmaps. Note renderCharImpl historically inverted this
// (bmpVal = 3 - raw) before comparing; keep passing raw tones here.
namespace grayplanes {

// Light and dark both mark the MSB; only dark marks the LSB.
inline bool setMsb(const uint8_t tone) { return tone == 1 || tone == 2; }
inline bool setLsb(const uint8_t tone) { return tone == 2; }

// Per 2x2 downsampled block (superscript/subscript/ruby at 50% scale): the
// block renders if any sample carries ink — maxRaw >= 2, or two weaker
// samples summing to >= 2 (preserves thin strokes nearest-neighbor can skip).
// In the grayscale passes the block's darkest sample picks the plane bits.
struct BlockPlan {
  bool plot;
  bool msb;
  bool lsb;
};

inline BlockPlan planBlock(const bool bwMode, const uint8_t maxRaw, const uint8_t coverageSum) {
  const bool inked = maxRaw >= 2 || coverageSum >= 2;
  if (bwMode) return {inked, false, false};
  if (!inked || maxRaw == 3) return {false, false, false};
  return {false, setMsb(maxRaw), setLsb(maxRaw)};
}

}  // namespace grayplanes
