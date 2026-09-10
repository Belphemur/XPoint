#ifndef BOOK_FONT_LOADER_H
#define BOOK_FONT_LOADER_H

#include <BookFont.h>
#include <FreeInkBook.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace freeink {
namespace book {

// ── public value types (discovery + settings) ────────────────────────────────

struct FontFaceInfo {
  char name[48] = {};       // family display name (manifest or filename stem)
  char file[64] = {};       // path under /fonts/
  uint8_t styleFlags = 0;  // BookFont::StyleFlags this file provides
  uint32_t fileSize = 0;
  uint32_t mtime = 0;  // for fingerprinting
};

struct FamilyInfo {
  char name[48] = {};
  uint8_t faceCount = 0;  // up to 4: REGULAR/BOLD/ITALIC/BOLD_ITALIC
  FontFaceInfo faces[4] = {};
  bool isBuiltinFallback = false;  // the BitmapBookFont chain
};

class BookFontLoader {
 public:
  static constexpr uint8_t kMaxDiscoveredFamilies = 32;

  BookFontLoader();
  ~BookFontLoader();

  // Scan /fonts/ for *.ttf/*.otf and build the family manifest.
  void begin();

  // (Re)load the active family if settings changed or registry dirty.
  // MUST be called before getReaderFont() or layoutGenerationHash().
  void ensureLoaded();

  // The live reader chain (never null — falls back to the builtin
  // BitmapBookFont chain when no TTF faces are loaded).
  FontChain* getReaderFont();

  // Content-based fingerprint: FNV-1a over loaded font bytes xor styleCoverage.
  uint32_t fontFingerprint() const;

  const FamilyInfo* families() const { return families_.data(); }
  uint8_t familyCount() const { return familyCount_; }

  void markDirty();

  // Scrub arenas + unload file bytes when leaving the reader with low heap.
  void releaseResidentCaches();

  // Public fingerprint helper — content-based, never path/mtime.
  uint32_t computeFingerprint() const;

 private:
  FamilyInfo families_[kMaxDiscoveredFamilies];
  uint8_t familyCount_ = 0;

  TtfFont* faces_[4] = {};
  FontChain chain_;
  uint32_t fingerprint_ = 0;
  std::atomic<bool> dirty_{false};

  // Two-tier font-byte storage: PSRAM via PoolBytes, DRAM via unique_ptr.
  // Owning pointers for the loaded font file bytes.
  void* fontBytes_[4] = {};
  uint8_t faceBytesOwner_[4] = {};  // 1 = PSRAM (heap_caps_free), 0 = DRAM (unique_ptr)
  uint32_t fontFileSizes_[4] = {};

  // Glyph arenas — one per face, persisted for the face's lifetime.
  // TtfFont::init borrows the arena; flushGlyphs rewinds to init mark.
  Arena arenas_[4];
  static constexpr size_t kGlyphArenaBytes = 8192;

  void scanFonts(const char* fontPath);
  bool loadFaceBytes(const FontFaceInfo& fi);
  FontChain* builtinFallback();
};

// Defined in main.cpp beside sdFontSystem (design §3.2).
extern BookFontLoader fontLoader;

}  // namespace book
}  // namespace freeink

#endif  // BOOK_FONT_LOADER_H
