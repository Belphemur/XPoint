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

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
#include <FtFont.h>
#endif

namespace freeink {
namespace book {

// Active native-TTF backend face (design D2): FreeType under the FT backend
// flag, stb_truetype otherwise. Both satisfy the RasterFont contract the
// FontChain consumes; the stb path stays compilable for rollback.
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
using NativeFace = freeink::font::FtFont;
#else
using NativeFace = TtfFont;
#endif

// ── public value types (discovery + settings) ────────────────────────────────

struct FontFaceInfo {
  // Full SD path including the root. Matches SdCardCacheStorage::kDirMax so
  // long vendor names ("Atkinson Hyperlegible Next/...-Regular.otf") fit.
  static constexpr size_t kFileCap = 160;

  char name[48] = {};        // family display name (manifest or filename stem)
  char file[kFileCap] = {};  // full path under the font root
  uint8_t styleFlags = 0;    // BookFont::StyleFlags this file provides
  uint32_t fileSize = 0;
  uint32_t mtime = 0;  // for fingerprinting
};

struct FamilyInfo {
  char name[48] = {};
  uint8_t faceCount = 0;  // up to 4: REGULAR/BOLD/ITALIC/BOLD_ITALIC
  FontFaceInfo faces[4] = {};
  bool isBuiltinFallback = false;  // the BitmapBookFont chain
};

// The manifest lives in the loader's BSS. Keep the enlarged path storage
// bounded: 32 rows must remain a modest DRAM allocation (~30KB), not a
// runaway buffer.
static_assert(sizeof(FamilyInfo) <= 1024, "FamilyInfo manifest row is too large");

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

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // Initial pixel size for FreeType faces. Per-run sizes (ruby, preview) adapt
  // at runtime through FtFont::ensureSize — the face is size-agnostic. Mirrors
  // CrossPointSettings::DEFAULT_TTF_FONT_POINT_SIZE without coupling the
  // loader to the settings header.
  static constexpr uint16_t kInitSizePx = 14;

  // Single source of truth for the reader's FT render options (SDK 43fed43
  // setRenderOptions). HintingMode::None is the shipped behavior — fully
  // unhinted loads, as decided in the FreeType backend campaign (hinted CFF
  // enters the Adobe interpreter whose stack footprint overflows small task
  // stacks; AA e-ink gains nothing from grid-fitting). Deliberately NOT a
  // user setting: the mode is render-affecting and must stay in lockstep with
  // the FIBP cache identity (renderOptionsFingerprintTag).
  static constexpr freeink::font::FtFont::RenderOptions kRenderOptions{};

  // Fingerprint tag folding the active render options into the font
  // fingerprint. Render-affecting options MUST invalidate FIBP cache
  // identity (hinting changes advances → layout), so this tag is mixed into
  // BOTH fingerprint sites — computeFingerprint() and the FibpPrefetchWorker
  // parity hash — and must be extended whenever kRenderOptions gains a knob
  // that alters glyph output.
  static constexpr uint32_t renderOptionsFingerprintTag() {
    return static_cast<uint32_t>(kRenderOptions.hinting) << 24;
  }
#endif

  const FamilyInfo* families() const { return families_.data(); }
  uint8_t familyCount() const { return familyCount_; }

  void markDirty();

  // Scrub arenas + unload file bytes when leaving the reader with low heap.
  void releaseResidentCaches();

  // Public fingerprint helper — content-based, never path/mtime.
  uint32_t computeFingerprint() const;

  // Fingerprint with the SD-backed per-face hash cache (P3.1): chained
  // per-slot hashes under /.crosspoint/fonts/ keyed by face path hash, valid
  // only when {fileSize, mtime, incoming chain seed} all match. Pure-memory
  // fallback (computeFingerprint()) runs on any mismatch or absent cache —
  // content semantics are identical either way. Non-const: consults and
  // refreshes the cache.
  uint32_t computeFingerprintCached();

  // FNV-1a over font bytes with a chained seed. The prefetch worker hashes
  // the same face bytes in the same slot order to derive an identical
  // fingerprint (FibpPrefetchWorker).
  static uint32_t fontBytesHash(const uint8_t* data, size_t len, uint32_t seed);
  // sfnt table-directory sanity gate shared by tryLoadFace and the worker's
  // face builder: numTables != 0 and every table's offset/length in-bounds.
  static bool validateSfntBytes(const uint8_t* data, uint32_t size);

  // Appends the four Atkinson faces to `chain` as its non-selectable tail:
  // a selected TTF family that lacks a glyph or style degrades to the
  // fallback face instead of a missing glyph (§14.5 chain-tail semantics).
  // Public so the prefetch worker can build an identical tail — the chain's
  // style coverage (and with it the fingerprint / FIBP generation) must
  // match the reader's chain byte for byte.
  static void appendFallbackTail(FontChain& chain);

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

  NativeFace* faces_[4] = {};
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
  // Fingerprint-cache identity per slot, captured in tryLoadFace(): the
  // face's path hash (cache key) and mtime (rehash trigger beside size).
  uint32_t facePathHash_[4] = {};
  uint32_t faceMtime_[4] = {};

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
