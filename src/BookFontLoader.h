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

  // Phase 3 family selection (design §3.6/§14.2): the reader and the settings
  // preview call this from SETTINGS before getReaderFont(). An empty name is
  // an explicit "built-in fallback" selection; a never-selected loader keeps
  // the legacy families_[0] default (debug rig).
  void selectFamily(const char* name);
  // Case-insensitive manifest lookup (§14.4 display name); nullptr when absent.
  const FamilyInfo* findFamily(const char* name) const;
  // Static picker gates: PSRAM present AND every face within the per-face
  // size guard. Load failures (corrupt fonts) are runtime — they degrade to
  // the fallback chain instead of greying the row.
  static bool isFamilyAvailable(const FamilyInfo& fam);
  // Per-face PSRAM size guard (CWE-400); picker rows above it are greyed out.
  static constexpr uint32_t kMaxFaceBytes = 2u * 1024u * 1024u;

  const FamilyInfo* families() const { return families_.data(); }
  uint8_t familyCount() const { return familyCount_; }

  void markDirty();

  // Scrub arenas + unload file bytes when leaving the reader with low heap.
  void releaseResidentCaches();

  // Public fingerprint helper — content-based, never path/mtime.
  uint32_t computeFingerprint() const;

#if defined(HOST_TEST)
  // Host-test seams: seed the manifest deterministically and read the budget.
  // setFamilyCount drives ensureLoaded()'s familyCount_ > 0 gate so tests can
  // exercise the load/reject paths; editFamily alone never touches the count.
  FamilyInfo& editFamily(uint8_t idx) { return families_[idx]; }
  void setFamilyCountForTest(uint8_t n) { familyCount_ = n; }
  uint32_t dramBudgetForTest() const { return remainingBudget_; }
  // Appends the Atkinson tail to the live chain without a loadable TTF face,
  // so host tests can exercise the tail-append path.
  void forceFallbackTailForTest();
  const FontChain& chainForTest() const { return chain_; }
  // Drive the §14.4 two-root discovery walk against the stub storage.
  static void scanFontsForTest(const char* rootPath, FamilyInfo* families, uint8_t& familyCount) {
    scanFonts(rootPath, families, familyCount);
  }
#endif

 private:
  std::array<FamilyInfo, kMaxDiscoveredFamilies> families_{};
  uint8_t familyCount_ = 0;
  // Selection state (see selectFamily()).
  char selectedFamily_[48] = {};
  bool familySelected_ = false;

  TtfFont* faces_[4] = {};
  FontChain chain_;
  uint32_t fingerprint_ = 0;
  bool loaded_ = false;  // a load attempt completed (fingerprint 0 is valid)
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
  // Backing storage is sized by profile too, so C3 (SMALL) does not burn
  // static BSS on STANDARD/LARGE-sized tables it cannot use. Compare the
  // resolved profile VALUES (BookProfile.h defines inactive selectors to 0,
  // so defined() alone would always be true).
  Arena arenas_[4];

  // PSRAM-only directive (zero native-TTF DRAM BSS): arena backing is
  // allocated lazily from the pool per face slot (poolMakeBytes), never
  // statically. poolMakeBytes lands in PSRAM on PSRAM boards and falls to
  // DRAM malloc otherwise — per design §14.1 no TTF face is ever loaded on
  // PSRAM-less boards, so it never allocates there in practice.
  PoolBytes glyphBacking_[4] = {};

  // Native-TTF PSRAM budget (all well within the 8MB pool):
  //   glyph arenas: 4 x kGlyphArenaBytes (12/32/48KB by profile) = 48–192KB
  //   builtin BitmapBookFont fallback: 4 x sizeof(BitmapBookFont) ≈ 16.4KB
  //   debug-rig scratch: 256KB transient (TtfRenderDebugActivity)
  //   font file bytes: up to kMaxPsramFontBytes (2MB) per face
#include "BookProfile.h"
#if FREEINK_BOOK_PROFILE == FREEINK_BOOK_PROFILE_SMALL
  static constexpr size_t kGlyphArenaBytes = 12 * 1024;
#elif FREEINK_BOOK_PROFILE == FREEINK_BOOK_PROFILE_LARGE
  static constexpr size_t kGlyphArenaBytes = 48 * 1024;
#else
  static constexpr size_t kGlyphArenaBytes = 32 * 1024;
#endif

  // Aggregate DRAM budget: derived from free heap with floor guards.
  uint32_t remainingBudget_ = 0;

  // Two-level family walk (design §14.4): one subfolder per family under
  // `rootPath`, hidden root scanned first so it wins on name collisions.
  // Appends into the caller's manifest (capped at kMaxDiscoveredFamilies).
  static void scanFonts(const char* rootPath, FamilyInfo* families, uint8_t& familyCount);
  static FontChain* builtinFallback();
  // One of the four baked Atkinson fallback faces (§14.3), owned by the
  // builtin singleton; appended to the active chain as its tail.
  static RenderFont* builtinFace(uint8_t idx);
  // Appends the four Atkinson faces to `chain` as its non-selectable tail:
  // a selected TTF family that lacks a glyph or style degrades to the
  // fallback face instead of a missing glyph (§14.5 chain-tail semantics).
  static void appendFallbackTail(FontChain& chain);

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
