// Per-backend font invariants (design D9): each backend (stb TtfFont /
// FreeType FtFont) is asserted against its own invariants — positive advances
// on real fixtures, the glyphBounds ⊇ rasterize containment contract, and
// malformed-face rejection — with NO cross-backend metric equality (stb and
// FreeType legitimately differ by ±1 px of hinting). Built twice by
// add_font_backend_tests(): the stb default and the FT variant
// (CROSSPOINT_FONT_BACKEND_FT=1). The FT-only variable-font test asserts
// true-bold wght behavior stb cannot provide.

#include <Font.h>
#include <FtFont.h>
#include <gtest/gtest.h>
#include <render/TtfFont.h>

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {

using freeink::font::Arena;
using freeink::font::GlyphBitmap;

// Backend-agnostic face wrapper: the two backends share the RasterFont
// contract but differ in init (FtFont takes size/weight/italic, no arena).
#if defined(CROSSPOINT_FONT_BACKEND_FT)
using InvariantFace = freeink::font::FtFont;
bool loadFace(InvariantFace& face, const std::vector<uint8_t>& bytes) {
  return face.init(bytes.data(), static_cast<uint32_t>(bytes.size()), 16, 400, false);
}
#else
using InvariantFace = freeink::font::TtfFont;
bool loadFace(InvariantFace& face, const std::vector<uint8_t>& bytes) {
  // TtfFont stores Arena* — the arena must outlive the face, so re-init a
  // static one per load (each test holds one face at a time; tests never
  // span faces across calls).
  static uint8_t arenaBuf[64 * 1024];
  static Arena arena(arenaBuf, sizeof(arenaBuf));
  arena.init(arenaBuf, sizeof(arenaBuf));
  return face.init(bytes.data(), static_cast<uint32_t>(bytes.size()), arena);
}
#endif

std::vector<uint8_t> readFile(const char* path) {
  // RAII file handle: fclose runs at scope exit on every return path.
  const std::unique_ptr<std::FILE, int (*)(std::FILE*)> f(std::fopen(path, "rb"), &std::fclose);
  if (f == nullptr) return {};
  if (std::fseek(f.get(), 0, SEEK_END) != 0) return {};
  const long size = std::ftell(f.get());
  if (size < 0) return {};
  if (std::fseek(f.get(), 0, SEEK_SET) != 0) return {};
  std::vector<uint8_t> bytes;
  bytes.resize(static_cast<size_t>(size));
  const size_t got = std::fread(bytes.data(), 1, bytes.size(), f.get());
  if (got != bytes.size()) bytes.clear();
  return bytes;
}

// Box containment: [bx, bx+bw) x [by, by+bh) covers the rasterized bitmap box.
bool contains(int16_t bx, int16_t by, uint16_t bw, uint16_t bh, int16_t rx, int16_t ry, uint16_t rw, uint16_t rh) {
  return bx <= rx && by <= ry && (bx + static_cast<int>(bw)) >= (rx + static_cast<int>(rw)) &&
         (by + static_cast<int>(bh)) >= (ry + static_cast<int>(rh));
}

// gtest needs a fixed-arity predicate; unpack the GlyphBitmap last.
bool contains4(int16_t bx, int16_t by, uint16_t bw, uint16_t bh, const GlyphBitmap* bmp) {
  return contains(bx, by, bw, bh, bmp->xoff, bmp->yoff, bmp->width, bmp->height);
}

void expectContainment(InvariantFace& face) {
  static constexpr uint32_t kCodepoints[] = {'A', 'a', 'g', 'y', 'M', 'W', 'j', '@', 0x00E9, 0x20AC};
  for (const uint16_t sizePx : {12, 16, 24}) {
    for (const uint32_t cp : kCodepoints) {
      int16_t bx = 0;
      int16_t by = 0;
      uint16_t bw = 0;
      uint16_t bh = 0;
      const bool haveBounds = face.glyphBounds(cp, sizePx, bx, by, bw, bh);
      const GlyphBitmap* bmp = face.rasterize(cp, sizePx);
      ASSERT_EQ(haveBounds, bmp != nullptr) << "cp=" << cp << " size=" << sizePx;
      if (!haveBounds || bmp == nullptr) continue;
      EXPECT_PRED5(contains4, bx, by, bw, bh, bmp) << "cp=" << cp << " size=" << sizePx;
    }
  }
}

}  // namespace

// ── advances: positive on the real corpus fixtures ───────────────────────────

TEST(FontBackendInvariants, PositiveAdvancesDejaVu) {
  const auto bytes = readFile(DEJAVU_FIXTURE);
  ASSERT_FALSE(bytes.empty());
  InvariantFace face;
  ASSERT_TRUE(loadFace(face, bytes));
  for (const uint32_t cp : {'A', 'g', 'W', '0'}) {
    EXPECT_GT(face.advance(cp, 16, 0), 0) << "cp=" << cp;
    EXPECT_GT(face.lineHeight(16), 0);
    EXPECT_GT(face.ascent(16), 0);
  }
}

TEST(FontBackendInvariants, PositiveAdvancesEmber) {
  const auto bytes = readFile(EMBER_FIXTURES_DIR "/Amazon_Ember_Regular.ttf");
  ASSERT_FALSE(bytes.empty());
  InvariantFace face;
  ASSERT_TRUE(loadFace(face, bytes));
  for (const uint32_t cp : {'A', 'g', 'W', '0'}) {
    EXPECT_GT(face.advance(cp, 16, 0), 0) << "cp=" << cp;
  }
}

TEST(FontBackendInvariants, PositiveAdvancesAtkinsonOtf) {
  const auto bytes = readFile(ATKINSON_OTF_FIXTURE);
  ASSERT_FALSE(bytes.empty());
  InvariantFace face;
  ASSERT_TRUE(loadFace(face, bytes));
  for (const uint32_t cp : {'A', 'g', 'W', '0'}) {
    EXPECT_GT(face.advance(cp, 16, 0), 0) << "cp=" << cp;
  }
}

// ── glyphBounds ⊇ rasterize (the PagePaint band-cull contract) ──────────────

TEST(FontBackendInvariants, GlyphBoundsContainRasterizeDejaVu) {
  const auto bytes = readFile(DEJAVU_FIXTURE);
  ASSERT_FALSE(bytes.empty());
  InvariantFace face;
  ASSERT_TRUE(loadFace(face, bytes));
  expectContainment(face);
}

TEST(FontBackendInvariants, GlyphBoundsContainRasterizeEmber) {
  const auto bytes = readFile(EMBER_FIXTURES_DIR "/Amazon_Ember_Regular.ttf");
  ASSERT_FALSE(bytes.empty());
  InvariantFace face;
  ASSERT_TRUE(loadFace(face, bytes));
  expectContainment(face);
}

// ── malformed faces are rejected by the backend itself (D10: the loader's
//    sfnt boundary is the first gate; the backend must not accept garbage) ──

TEST(FontBackendInvariants, RejectsGarbageAndTruncation) {
  InvariantFace garbage;
  std::vector<uint8_t> junk(4096, 0xAB);
  EXPECT_FALSE(loadFace(garbage, junk));  // no sfnt magic

  const auto bytes = readFile(DEJAVU_FIXTURE);
  ASSERT_FALSE(bytes.empty());
  InvariantFace truncated;
  std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + 16);
  EXPECT_FALSE(loadFace(truncated, cut));

  InvariantFace empty;
  std::vector<uint8_t> none;
  EXPECT_FALSE(loadFace(empty, none));
}

// ── FreeType-only: variable-font wght axis drives real bold advances ─────────

#if defined(CROSSPOINT_FONT_BACKEND_FT)
TEST(FontBackendInvariantsFt, VariableFontTrueBoldAdvancesDifferFromRegular) {
  const auto bytes = readFile(VF_FIXTURE);
  ASSERT_FALSE(bytes.empty());
  freeink::font::FtFont regular;
  ASSERT_TRUE(regular.init(bytes.data(), static_cast<uint32_t>(bytes.size()), 16, 400, false));
  freeink::font::FtFont bold;
  ASSERT_TRUE(bold.init(bytes.data(), static_cast<uint32_t>(bytes.size()), 16, 700, false));
  // True bold via the wght axis: at least some caps must advance differently
  // from regular (per-glyph equality is legal for a few glyphs, not all).
  int differing = 0;
  for (const uint32_t cp : {'A', 'B', 'H', 'M', 'O', 'n', 'x'}) {
    if (bold.advance(cp, 16, 0) != regular.advance(cp, 16, 0)) ++differing;
  }
  EXPECT_GT(differing, 0);
}
#endif
