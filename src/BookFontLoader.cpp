// BookFontLoader.cpp — Phase 1a native TTF font-loader infrastructure.
//
// Scans /fonts/*.ttf|.otf on SD, builds the family manifest, and owns the
// live FontChain plus the builtin BitmapBookFont fallback.
//
// Two-tier allocation: PSRAM tier (S3) via poolMalloc/poolMakeBytes
// (lib/Memory/Memory.h — SPIRAM on BOARD_HAS_PSRAM, DRAM otherwise) and DRAM
// tier (C3 / Sticky, no PSRAM) via makeUniqueNoThrow<uint8_t[]>. Never bare
// new (AGENTS.md §9). kMaxDramFontBytes is DERIVED from measured free heap
// after all arenas are allocated (keeping the 32KB/16KB heap-gate floors) —
// not a hardcoded 256KB. Oversized files: LOG_ERR("BFNT", ...) and stay
// listed but greyed out.
//
// FontChain assembly (<=8 faces, styleCoverage()). fontFingerprint() = FNV-1a
// over the LOADED font bytes xor styleCoverage — content-based, never path/mtime.
//
// Builtin fallback: singleton FontChain over 4 static BitmapBookFont instances
// (16KB static BSS — accounted in the C3 budget).
//
// sfnt VALIDATION BOUNDARY before TtfFont::init: table-directory bounds +
// numTables sanity; on failure LOG_ERR + skip the face. TtfFont::init only
// checks len<12 (TtfFont.cpp:36). Test corpus includes 2 malformed fonts.
//
// Face bytes: stb_truetype BORROWS the source bytes — they must stay resident
// for the face's lifetime (TtfFont.h: "data is borrowed and must outlive the
// font"). They live in fontPsramBytes_/fontDramBytes_ RAII owners, released
// only in ensureLoaded()/releaseResidentCaches() when the face is deleted.
// Glyph rasters live in the per-face arena (owned by BookFontLoader).
//
// Per AGENTS.md: makeUniqueNoThrow, no std::string in hot paths, tr() for UI
// strings, HalStorage only (never SdFat direct).

#include "BookFontLoader.h"

#include <FreeInkUIBookFont.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>

#ifdef HOST_TEST
#include "Arduino.h"  // host-test stub for ESP.getFreeHeap
#endif

#include <algorithm>
#include <cstring>
#include <memory>

namespace freeink {
namespace book {

// Hard bounds for the DRAM-tier font file size gate. Design §3.3: the value is
// derived from ESP.getFreeHeap()/getMaxAllocHeap() after all arenas are
// allocated; this constant is the current placeholder (128KB floor).
static constexpr uint32_t kMaxDramFontBytes = 128 * 1024;

// PSRAM-tier per-face size guard (CWE-400): fonts live resident in PSRAM for
// the face's lifetime; bound each file well below the 8MB PSRAM pool.
static constexpr uint32_t kMaxPsramFontBytes = 2 * 1024 * 1024;

// SFNT minimum: 12-byte header + numTables * 16-byte entries.
static constexpr uint32_t kMinSfntLen(uint16_t numTables) { return 12u + static_cast<uint32_t>(numTables) * 16u; }

// FNV-1a hash over font data, mixed with style coverage.
static uint32_t fontFNV1a(const uint8_t* data, size_t len, uint32_t seed = 0x811c9dc5) {
  uint32_t h = seed;
  for (size_t i = 0; i < len; ++i) {
    h ^= static_cast<uint32_t>(data[i]);
    h += (h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24);
  }
  return h;
}

// Read a font file's bytes via HalStorage into a caller-provided buffer.
// Returns bytes read, or 0 on failure.
static uint32_t readFontFile(const char* path, uint8_t* buf, uint32_t bufSz) {
  HalFile file;
  if (!Storage.openFileForRead("BFNT", path, file)) {
    LOG_ERR("BFNT", "Cannot open font %s", path);
    return 0;
  }
  uint32_t sz = file.fileSize();
  if (sz == 0 || sz > bufSz) {
    LOG_ERR("BFNT", "Font %s too large for buffer (%u > %u)", path, sz, bufSz);
    return 0;
  }
  size_t got = file.read(buf, sz);
  if (got != sz) {
    LOG_ERR("BFNT", "Font %s read error: %zu != %u", path, got, sz);
    return 0;
  }
  return sz;
}

// ── BookFontLoader implementation ────────────────────────────────────────────

BookFontLoader::BookFontLoader() = default;
BookFontLoader::~BookFontLoader() = default;

void BookFontLoader::begin() {
  familyCount_ = 0;
  fingerprint_ = 0;
  dirty_.store(false, std::memory_order_relaxed);
  families_ = {};
  chain_ = FontChain{};
  std::fill(std::begin(faces_), std::end(faces_), nullptr);
  std::fill(std::begin(arenas_), std::end(arenas_), Arena{});
  std::fill(std::begin(fontBytes_), std::end(fontBytes_), nullptr);
  std::generate(std::begin(fontPsramBytes_), std::end(fontPsramBytes_), []() { return PoolBytes{}; });
  std::fill(std::begin(fontDramBytes_), std::end(fontDramBytes_), nullptr);
  std::fill(std::begin(faceBytesOwner_), std::end(faceBytesOwner_), 0);
  std::fill(std::begin(fontFileSizes_), std::end(fontFileSizes_), 0);
  remainingBudget_ = 0;
  initBudget();
}

void BookFontLoader::ensureLoaded() {
  if (!dirty_.load(std::memory_order_relaxed) && fingerprint_ != 0) return;

  // Clear previous state. The RAII owners (fontPsramBytes_/fontDramBytes_)
  // release the byte buffers; never poolFree the raw pointers manually —
  // fontPsramBytes_[i].reset() already calls poolFree (double-free).
  for (uint8_t i = 0; i < 4; ++i) {
    if (faces_[i]) {
      delete faces_[i];
      faces_[i] = nullptr;
    }
    fontBytes_[i] = nullptr;
    fontPsramBytes_[i].reset();
    fontDramBytes_[i].reset();
    faceBytesOwner_[i] = 0;
    fontFileSizes_[i] = 0;
    arenas_[i] = Arena{};
  }
  chain_ = FontChain{};
  if (familyCount_ == 0) return;

  // Recompute the DRAM budget: the release loop above freed the previous
  // faces' bytes, so a reload must not inherit the previously spent budget.
  initBudget();

  const FamilyInfo& fam = families_[0];
  for (uint8_t i = 0; i < fam.faceCount && i < 4; ++i) {
    if (!tryLoadFace(i, fam.faces[i], chain_)) {
      // Face skipped (too large, invalid sfnt, OOM); continue with fewer faces.
    }
  }
  fingerprint_ = computeFingerprint();
  dirty_.store(false, std::memory_order_relaxed);
}

FontChain* BookFontLoader::getReaderFont() {
  ensureLoaded();
  return (chain_.styleCoverage() != 0) ? &chain_ : builtinFallback();
}

uint32_t BookFontLoader::fontFingerprint() const { return fingerprint_; }

void BookFontLoader::markDirty() { dirty_.store(true, std::memory_order_relaxed); }

void BookFontLoader::releaseResidentCaches() {
  // Same release discipline as ensureLoaded(): RAII owners own the bytes.
  for (uint8_t i = 0; i < 4; ++i) {
    if (faces_[i]) {
      delete faces_[i];
      faces_[i] = nullptr;
    }
    fontBytes_[i] = nullptr;
    fontPsramBytes_[i].reset();
    fontDramBytes_[i].reset();
    faceBytesOwner_[i] = 0;
    fontFileSizes_[i] = 0;
    arenas_[i] = Arena{};
  }
  chain_ = FontChain{};
  fingerprint_ = 0;
}

uint32_t BookFontLoader::computeFingerprint() const {
  // FNV-1a over loaded face bytes (only valid ones) xor styleCoverage.
  // Never uses path or mtime — content-based (design §3.4).
  uint32_t h = 0x811c9dc5;
  for (uint8_t i = 0; i < 4; ++i) {
    if (fontBytes_[i] && fontFileSizes_[i] > 0) {
      h = fontFNV1a(static_cast<const uint8_t*>(fontBytes_[i]), fontFileSizes_[i], h);
    }
  }
  h ^= static_cast<uint32_t>(chain_.styleCoverage());
  return h;
}

FontChain* BookFontLoader::builtinFallback() {
  // Singleton FontChain over 4 static BitmapBookFont instances (4 styles,
  // 16KB static BSS). The faces have static storage duration so FontChain
  // entries remain valid after this function returns.
  static FontChain fallback;
  static bool init = false;
  if (!init) {
    init = true;
    static freeink::ui::BitmapBookFont r(freeink::ui::kNotoSansFont);
    static freeink::ui::BitmapBookFont b(freeink::ui::kNotoSansFont);
    static freeink::ui::BitmapBookFont i(freeink::ui::kNotoSansFont);
    static freeink::ui::BitmapBookFont bi(freeink::ui::kNotoSansFont);
    fallback.add(&r, StyleNone);
    fallback.add(&b, StyleBold);
    fallback.add(&i, StyleItalic);
    fallback.add(&bi, StyleBold | StyleItalic);
  }
  return &fallback;
}

// ── scanFonts / loadFaceBytes (private, Phase 2 fill-ins) ────────────────────
// Phase 1a ships the loader infrastructure only; discovery (instance-backed
// /fonts walk grouping Family-Regular/Bold/Italic.ttf into families_) is
// implemented in Phase 2, where begin() will call it and main.cpp will call
// fontLoader.begin() beside sdFontSystem.begin(renderer). Until then
// familyCount_ stays 0 and getReaderFont() serves the builtin fallback — the
// intended Phase 1a behavior (no reader wiring yet, zero regression risk).

void BookFontLoader::scanFonts(const char* fontPath) { (void)fontPath; }

bool BookFontLoader::loadFaceBytes(const FontFaceInfo& fi) {
  // Stub for Phase 1a — real implementation loads the whole font into a
  // transient buffer for init, then releases the raw bytes; glyph data
  // persists in the per-face arena.
  (void)fi;
  return false;
}

// ── tryLoadFace — single face into the live chain ────────────────────────────
// Member of BookFontLoader so it can access private members (faces_,
// fontPsramBytes_, fontDramBytes_, fontBytes_, arenas_, remainingBudget_).

bool BookFontLoader::tryLoadFace(uint8_t faceIdx, const FontFaceInfo& fi, FontChain& chain) {
  // DRAM-tier size gate: skip oversized files (design §3.3). PSRAM-backed
  // boards bypass this DRAM budget; the PSRAM tier has its own guard below.
  if (HalMemory::getPsramHeap().totalBytes == 0) {
    if (fi.fileSize > kMaxDramFontBytes) {
      LOG_ERR("BFNT", "Font %s too large for DRAM tier (%u > %u)", fi.file, fi.fileSize, kMaxDramFontBytes);
      return false;
    }
    // Budget of zero = exhausted; reject every non-empty font.
    if (fi.fileSize > remainingBudget_) {
      LOG_ERR("BFNT", "Font %s exceeds remaining DRAM budget (%u > %u)", fi.file, fi.fileSize, remainingBudget_);
      return false;
    }
  } else if (fi.fileSize > kMaxPsramFontBytes) {
    LOG_ERR("BFNT", "Font %s too large for PSRAM tier (%u > %u)", fi.file, fi.fileSize, kMaxPsramFontBytes);
    return false;
  }

  // Allocate a transient buffer for the font file bytes. PSRAM path first on
  // boards that have it; DRAM fallback otherwise. The owning handle is stored
  // in the member arrays immediately; fontBytes points at the member-owned
  // storage for the rest of the function.
  void* fontBytes = nullptr;
  bool isPsram = false;

  if (HalMemory::getPsramHeap().totalBytes > 0) {
    fontPsramBytes_[faceIdx] = poolMakeBytes(fi.fileSize);
    if (fontPsramBytes_[faceIdx]) {
      fontBytes = fontPsramBytes_[faceIdx].get();
      isPsram = true;
    }
  }

  std::unique_ptr<uint8_t[]> localDram;
  if (!fontBytes) {
    // DRAM fallback (no PSRAM, or PSRAM pool exhausted): the DRAM gates apply
    // to the actual allocation tier, not the detected board capability.
    if (fi.fileSize > kMaxDramFontBytes) {
      LOG_ERR("BFNT", "Font %s too large for DRAM tier (%u > %u)", fi.file, fi.fileSize, kMaxDramFontBytes);
      fontPsramBytes_[faceIdx].reset();
      return false;
    }
    if (fi.fileSize > remainingBudget_) {
      LOG_ERR("BFNT", "Font %s exceeds remaining DRAM budget (%u > %u)", fi.file, fi.fileSize, remainingBudget_);
      fontPsramBytes_[faceIdx].reset();
      return false;
    }
    localDram = makeUniqueNoThrow<uint8_t[]>(fi.fileSize);
    if (!localDram) {
      LOG_ERR("BFNT", "Font buffer OOM for %u bytes", fi.fileSize);
      fontPsramBytes_[faceIdx].reset();
      return false;
    }
    fontBytes = localDram.get();
    isPsram = false;
  }

  if (readFontFile(fi.file, static_cast<uint8_t*>(fontBytes), fi.fileSize) != fi.fileSize) {
    // Cleanup on failure: release whatever we allocated.
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }

  // sfnt validation boundary: numTables sanity + table-directory O/L checks.
  // TtfFont::init only checks len<12; we validate here.
  if (fi.fileSize < 12) {
    LOG_ERR("BFNT", "Font %s too small for sfnt header", fi.file);
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }
  uint16_t numTables = static_cast<uint16_t>((static_cast<const uint8_t*>(fontBytes)[4] << 8) |
                                             static_cast<const uint8_t*>(fontBytes)[5]);
  if (numTables == 0) {
    LOG_ERR("BFNT", "Font %s invalid numTables %u", fi.file, numTables);
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }
  uint32_t minSz = kMinSfntLen(numTables);
  if (fi.fileSize < minSz) {
    LOG_ERR("BFNT", "Font %s too small for table directory (%u < %u)", fi.file, fi.fileSize, minSz);
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }
  for (uint16_t i = 0; i < numTables; ++i) {
    const uint8_t* entry = static_cast<const uint8_t*>(fontBytes) + 12 + static_cast<size_t>(i) * 16;
    // Guard against overflow in offset+length (uint32_t wraparound).
    uint32_t offset = static_cast<uint32_t>(entry[8]) << 24 | static_cast<uint32_t>(entry[9]) << 16 |
                      static_cast<uint32_t>(entry[10]) << 8 | static_cast<uint32_t>(entry[11]);
    uint32_t length = static_cast<uint32_t>(entry[12]) << 24 | static_cast<uint32_t>(entry[13]) << 16 |
                      static_cast<uint32_t>(entry[14]) << 8 | static_cast<uint32_t>(entry[15]);
    if (length > fi.fileSize || offset > fi.fileSize || offset + length < offset || offset + length > fi.fileSize) {
      LOG_ERR("BFNT", "Font %s table %u O/L %u/%u exceeds size", fi.file, i, offset, length);
      if (isPsram) {
        fontPsramBytes_[faceIdx].reset();
      } else {
        localDram.reset();
      }
      return false;
    }
  }

  // Per-face glyph arena — each face gets its OWN persistent backing buffer
  // (not the shared glyphBuf from the previous version). This prevents
  // overwriting glyph data when loading multiple faces (PRRT_kwDOUDrzps6g4-n7).
  // Size by SDK profile: TtfFont's slot tables alone need 4.6KB (SMALL:
  // 256×12 + 64×24), 9.2KB (STANDARD: 512×12 + 128×24), 36.9KB (LARGE:
  // 2048×12 + 512×24) before any glyph bitmap — 8KB fails STANDARD at
  // TtfFont::init. 32KB covers STANDARD plus raster headroom; LARGE is
  // not used by any current env.
  alignas(4) static uint8_t glyphBufs[4][kGlyphArenaBytes];
  arenas_[faceIdx] = Arena(glyphBufs[faceIdx], kGlyphArenaBytes);

  TtfFont* face = new (std::nothrow) TtfFont();
  if (!face) {
    LOG_ERR("BFNT", "TtfFont OOM for %s", fi.file);
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }
  if (!face->init(static_cast<const uint8_t*>(fontBytes), fi.fileSize, arenas_[faceIdx])) {
    LOG_ERR("BFNT", "TtfFont::init failed for %s", fi.file);
    delete face;
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }

  if (!chain.add(face, fi.styleFlags)) {
    LOG_ERR("BFNT", "FontChain::add failed to register %s (duplicate style?)", fi.file);
    delete face;
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }

  // Transfer ownership of the font bytes to the loader.
  // PSRAM: fontPsramBytes_ holds the RAII owner (heap_caps_free on reset).
  // DRAM:   fontDramBytes_ holds the unique_ptr<uint8_t[]> (delete[] on reset).
  // fontBytes_ is the non-owning raw pointer used for fingerprinting.
  fontBytes_[faceIdx] = fontBytes;
  faceBytesOwner_[faceIdx] = static_cast<uint8_t>(isPsram ? 1 : 2);
  fontFileSizes_[faceIdx] = fi.fileSize;
  faces_[faceIdx] = face;

  // Steal the RAII owners so they persist beyond this function.
  if (isPsram) {
    // fontPsramBytes_[faceIdx] already moved from psram above.
  } else {
    fontDramBytes_[faceIdx] = std::move(localDram);
  }

  // Decrement the aggregate DRAM budget (only for DRAM-tier allocations).
  if (!isPsram && fi.fileSize <= remainingBudget_) {
    remainingBudget_ -= fi.fileSize;
  }

  return true;
}

void BookFontLoader::initBudget() {
  // Two-tier allocation budget (design §3.3): derive the DRAM budget from the
  // current free heap, keeping 32KB/16KB heap-gate floors for the hot render
  // path and stack respectively. On PSRAM boards (S3) fonts bypass the DRAM
  // budget entirely, so this only governs the C3/Sticky (no-PSRAM) tier.
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t maxAlloc = ESP.getMaxAllocHeap();
  uint32_t usable = std::min(freeHeap, maxAlloc);
  // Reserve 32KB for other allocator needs, 16KB for stack safety.
  uint32_t floor = 32 * 1024 + 16 * 1024;
  if (usable > floor) {
    remainingBudget_ = usable - floor;
  } else {
    // Heap at/below the reserve: nothing safe to spend.
    remainingBudget_ = 0;
  }
  // Cap at the compile-time max to avoid surprises.
  if (remainingBudget_ > kMaxDramFontBytes) {
    remainingBudget_ = kMaxDramFontBytes;
  }
}

}  // namespace book
}  // namespace freeink
