#pragma once

#include <GfxRenderer.h>

#include <cstddef>

#include "Logo120.h"
#include "Memory.h"

// Blit Logo120 as a black mark on a transparent background. The asset stores
// bit=1 as white and bit=0 as black, so a plain drawImageTransparent() paints
// the logo's black backdrop as ink (a solid block with the mark cut out). XOR
// each byte to flip the roles — the same treatment the boot splash uses — so
// the mark itself becomes the ink and the surround is left untouched. Falls
// back to the raw blit on allocation failure (better a wrong-polarity logo
// than none).
inline void drawLogo120Inverted(const GfxRenderer& renderer, const int x, const int y) {
  constexpr int kSize = 120;
  constexpr size_t kBytes = static_cast<size_t>(kSize) * kSize / 8;  // 1800
  auto inverted = makeUniqueNoThrow<uint8_t[]>(kBytes);
  if (!inverted) {
    renderer.drawImageTransparent(Logo120, x, y, kSize, kSize);
    return;
  }
  for (size_t i = 0; i < kBytes; ++i) {
    inverted[i] = static_cast<uint8_t>(Logo120[i] ^ 0xFF);
  }
  renderer.drawImageTransparent(inverted.get(), x, y, kSize, kSize);
}