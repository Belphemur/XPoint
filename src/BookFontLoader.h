#pragma once

#include <BookFont.h>
#include <FreeInkBook.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Memory.h>
#include <render/TtfFont.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace freeink {
namespace book {

// ── public value types (discovery + settings) ────────────────────────────────

struct FontFaceInfo {
  char name[48] = {};      // family display name (manifest or filename stem)
  char file[64] = {};      // path under /fonts/
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

#if defined(HOST_TEST)
  // Host-test seams: seed the manifest deterministically and read the budget.
  FamilyInfo& editFamily(uint8_t idx) { return families_[idx]; }
  uint32_t dramBudgetForTest() const { return remainingBudget_; }
#endif

 private:
  std::array<FamilyInfo, kMaxDiscoveredFamilies> families_{};
  uint8_t familyCount_ = 0;

  TtfFont* faces_[4] = {};
  FontChain chain_;
  uint32_t fingerprint_ = 0;
  std::atomic<bool> dirty_{false};

  // Two-tier font-byte storage: each face has its own RAII owner.
  // PSRAM: PoolBytes (poolFree on reset). DRAM: unique_ptr<uint8_t[]> (delete[]).
  PoolBytes fontPsramBytes_[4] = {};
  std::unique_ptr<uint8_t[]> fontDramBytes_[4] = {};
  void* fontBytes_[4] = {};         // non-owning raw pointer for fingerprinting
  uint8_t faceBytesOwner_[4] = {};  // 0=none, 1=PSRAM, 2=DRAM
  uint32_t fontFileSizes_[4] = {};

  // Per-face glyph arenas — each has its own persistent backing buffer.
  // Size must fit TtfFont's profile-scaled slot tables before any glyph
  // bitmap: SMALL 4.6KB / STANDARD 9.2KB / LARGE 36.9KB (see TtfFont.h).
  // The backing storage is sized by profile too (kGlyphArenaBytes below) so
  // C3 (SMALL) does not burn 4x the full budget of static BSS on tables it
  // cannot use.
  Arena arenas_[4];
#if defined(FREEINK_BOOK_SMALL)
  static constexpr size_t kGlyphArenaBytes = 12 * 1024;
#elif defined(FREEINK_BOOK_LARGE)
  static constexpr size_t kGlyphArenaBytes = 48 * 1024;
#else
  static constexpr size_t kGlyphArenaBytes = 32 * 1024;
#endif

  // Aggregate DRAM budget: derived from free heap with floor guards.
  uint32_t remainingBudget_ = 0;

  static void scanFonts(const char* fontPath);
  static bool loadFaceBytes(const FontFaceInfo& fi);
  static FontChain* builtinFallback();

  // Load a single face into the live chain (member so it can access private
  // state: faces_, arenas_, fontBytes_, fontPsramBytes_, fontDramBytes_).
  bool tryLoadFace(uint8_t faceIdx, const FontFaceInfo& fi, FontChain& chain);

  // Initialize the aggregate DRAM budget from current free heap.
  void initBudget();

  // Max faces to load from one family (4: regular/bold/italic/bold-italic).
  static constexpr uint8_t kMaxFacesPerFamily = 4;
};

// Defined in main.cpp beside sdFontSystem (design §3.2).
extern BookFontLoader fontLoader;

}  // namespace book
}  // namespace freeink
