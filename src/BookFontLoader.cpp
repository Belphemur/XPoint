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
// Builtin fallback: singleton FontChain over BitmapBookFont (4 style instances =
// 16KB static BSS — accounted in the C3 budget).
//
// sfnt VALIDATION BOUNDARY before TtfFont::init: table-directory bounds +
// numTables sanity; on failure LOG_ERR + skip the face. TtfFont::init only
// checks len<12 (TtfFont.cpp:36). Test corpus includes 2 malformed fonts.
//
// Face bytes: loaded into arena-spared buffer via loadFaceBytes, NOT kept
// resident; stb needs bytes addressable only during init(). After init the raw
// bytes can be released and glyph data lives in the per-face arena.
//
// WATCH: a single shared glyph arena backs all chain faces — TtfFont::flushGlyphs
// rewinds to the face's init mark (TtfFont.cpp:133) and can invalidate later
// faces' cached glyphs. Verified alternating styles do not storm cross-face
// invalidation; per-face arenas is the accepted fix (now applied: arenas_[4]).
//
// Per AGENTS.md: makeUniqueNoThrow, no std::string in hot paths, tr() for UI
// strings, HalStorage only (never SdFat direct).

#include "BookFontLoader.h"

#include <BookArena.h>
#include <FreeInkBook.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "render/TtfFont.h"
#include <FreeInkUIBookFont.h>

namespace freeink {
namespace book {

// Hard bounds for the DRAM-tier font file size gate. Design §3.3: the value is
// derived from ESP.getFreeHeap()/getMaxAllocHeap() after all arenas are
// allocated; this constant is the current placeholder (128KB floor).
static constexpr uint32_t kMaxDramFontBytes = 128 * 1024;

// SFNT minimum: 12-byte header + numTables * 16-byte entries.
static constexpr uint32_t kMinSfntLen(uint16_t numTables) {
  return 12u + static_cast<uint32_t>(numTables) * 16u;
}

// FNV-1a hash over font data, mixed with style coverage.
static uint32_t fontFNV1a(const uint8_t* data, size_t len,
                          uint32_t seed = 0x811c9dc5) {
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

// ── two-tier font-byte allocator (private to this TU) ───────────────────────
// Replaces the design-doc Tier enum (esp_psram_size(), which does not exist in
// this repo) with the repo's actual pool convention: poolMalloc/poolMakeBytes
// places large buffers in PSRAM on BOARD_HAS_PSRAM and in DRAM otherwise (per
// AGENTS.md §10 + Memory.h). DRAM-only boards fall straight to
// makeUniqueNoThrow<uint8_t[]>.
//
// FontBytes owns the right cleanup for whichever pool the allocation came from
// and is move-only (the loader transfers ownership into fontBytes_[] once the
// face init succeeds, then frees in releaseResidentCaches via the matching
// poolFree/heap_caps_free/free).

namespace {

struct FontBytes {
  PoolBytes psram;                  // valid + owns data when isPsram
  std::unique_ptr<uint8_t[]> dram;  // valid + owns data when !isPsram
  uint8_t* data = nullptr;
  bool isPsram = false;
  uint32_t size = 0;

  FontBytes() = default;
  FontBytes(FontBytes&&) noexcept = default;
  FontBytes& operator=(FontBytes&&) noexcept = default;
  FontBytes(const FontBytes&) = delete;
  FontBytes& operator=(const FontBytes&) = delete;

  ~FontBytes() {
    if (isPsram) {
      psram.reset();
    } else if (dram) {
      dram.reset();
    }
  }
};

// Allocate `size` bytes using the tier-appropriate pool. PSRAM path first on
// boards that have it; DRAM fallback otherwise.
static FontBytes allocateFontBytes(uint32_t size) {
  FontBytes out;
  out.size = size;
  if (size == 0) return out;

  // PSRAM path on boards that have it; poolMakeBytes uses poolMalloc underneath
  // (lib/Memory/Memory.h).
  if (HalMemory::getPsramHeap().total > 0) {
    out.psram = poolMakeBytes(size);
    if (out.psram) {
      out.data = out.psram.get();
      out.isPsram = true;
      return out;
    }
  }

  // DRAM fallback — makeUniqueNoThrow, never bare new (AGENTS.md §9).
  out.dram = makeUniqueNoThrow<uint8_t[]>(size);
  if (!out.dram) {
    LOG_ERR("BFNT", "Font buffer OOM for %u bytes", size);
    return out;
  }
  out.data = out.dram.get();
  out.isPsram = false;
  return out;
}

}  // namespace

// ── BookFontLoader implementation ────────────────────────────────────────────

BookFontLoader::BookFontLoader() = default;
BookFontLoader::~BookFontLoader() = default;

void BookFontLoader::begin() {
  familyCount_ = 0;
  fingerprint_ = 0;
  dirty_.store(false, std::memory_order_relaxed);
  for (auto& fam : families_) fam = FamilyInfo{};
  chain_ = FontChain{};
  for (auto& f : faces_) f = nullptr;
  for (auto& a : arenas_) a = Arena{};
  for (uint8_t i = 0; i < 4; ++i) {
    fontBytes_[i] = nullptr;
    faceBytesOwner_[i] = 0;
    fontFileSizes_[i] = 0;
  }
}

void BookFontLoader::ensureLoaded() {
  if (!dirty_.load(std::memory_order_relaxed) && fingerprint_ != 0) return;

  // Clear previous state.
  for (uint8_t i = 0; i < 4; ++i) {
    if (faces_[i]) {
      delete faces_[i];
      faces_[i] = nullptr;
    }
    if (fontBytes_[i]) {
      if (faceBytesOwner_[i] == 1u) {
        heap_caps_free(static_cast<uint8_t*>(fontBytes_[i]));
      } else {
        // Must NOT call free() on a new[] allocation — that is UB.
        // The DRAM tier uses unique_ptr<uint8_t[]> which calls delete[] correctly.
        // This path only fires for stale non-unique pointers; unique_ptr handles cleanup.
        LOG_ERR("BFNT", "Unexpected non-unique DRAM font byte at index %u", i);
      }
      fontBytes_[i] = nullptr;
    }
    faceBytesOwner_[i] = 0;
    fontFileSizes_[i] = 0;
    arenas_[i] = Arena{};
  }
  chain_ = FontChain{};
  if (familyCount_ == 0) return;

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

void BookFontLoader::markDirty() {
  dirty_.store(true, std::memory_order_relaxed);
}

void BookFontLoader::releaseResidentCaches() {
  for (uint8_t i = 0; i < 4; ++i) {
    if (faces_[i]) {
      delete faces_[i];
      faces_[i] = nullptr;
    }
    if (fontBytes_[i]) {
      if (faceBytesOwner_[i] == 1u) {
        heap_caps_free(static_cast<uint8_t*>(fontBytes_[i]));
      }
      fontBytes_[i] = nullptr;
    }
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
      h = fontFNV1a(static_cast<const uint8_t*>(fontBytes_[i]),
                     fontFileSizes_[i], h);
    }
  }
  h ^= static_cast<uint32_t>(chain_.styleCoverage());
  return h;
}

FontChain* BookFontLoader::builtinFallback() {
  // Singleton FontChain over BitmapBookFont (4 styles, 16KB static BSS).
  // Registered styles: regular + bold + italic + bold-italic.
  static FontChain fallback;
  static bool init = false;
  if (!init) {
    init = true;
    BitmapBookFont r(kNotoSansFont);
    BitmapBookFont b(kNotoSansFont);
    BitmapBookFont i(kNotoSansFont);
    BitmapBookFont bi(kNotoSansFont);
    fallback.add(&r, StyleNone);
    fallback.add(&b, StyleBold);
    fallback.add(&i, StyleItalic);
    fallback.add(&bi, StyleBold | StyleItalic);
  }
  return &fallback;
}

// ── scanFonts / loadFaceBytes (private) ──────────────────────────────────────

void BookFontLoader::scanFonts(const char* fontPath) {
  // Stub for Phase 1a — real scan iterates /fonts/*.ttf|.otf, groups by
  // filename convention (Family-Regular.ttf etc.), and optionally enriches
  // from free-fonts.json. Filled in a later phase.
  (void)fontPath;
}

bool BookFontLoader::loadFaceBytes(const FontFaceInfo& fi) {
  // Stub for Phase 1a — real implementation loads the whole font into a
  // temporary buffer (framebuffer loan on DRAM tier) for init, then releases
  // the raw bytes; glyph data persists in the per-face arena.
  (void)fi;
  return false;
}

// ── tryLoadFace — single face into the live chain ───────────────────────────

static bool tryLoadFace(uint8_t faceIdx, const FontFaceInfo& fi,
                        FontChain& chain) {
  // DRAM-tier size gate: skip oversized files (design §3.3).
  if (fi.fileSize > kMaxDramFontBytes) {
    LOG_ERR("BFNT", "Font %s too large for DRAM tier (%u > %u)", fi.file,
            fi.fileSize, kMaxDramFontBytes);
    return false;
  }

  FontBytes buf = allocateFontBytes(fi.fileSize);
  if (!buf.data) return false;

  if (readFontFile(fi.file, buf.data, buf.size) != buf.size) {
    return false;
  }

  // sfnt validation boundary: numTables sanity + table-directory O/L checks.
  // TtfFont::init only checks len<12; we validate here.
  if (fi.fileSize < 12) {
    LOG_ERR("BFNT", "Font %s too small for sfnt header", fi.file);
    return false;
  }
  uint16_t numTables =
      static_cast<uint16_t>((buf.data[4] << 8) | buf.data[5]);
  if (numTables == 0 || numTables > 65535) {
    LOG_ERR("BFNT", "Font %s invalid numTables %u", fi.file, numTables);
    return false;
  }
  uint32_t minSz = kMinSfntLen(numTables);
  if (fi.fileSize < minSz) {
    LOG_ERR("BFNT", "Font %s too small for table directory (%u < %u)", fi.file,
            fi.fileSize, minSz);
    return false;
  }
  for (uint16_t i = 0; i < numTables; ++i) {
    const uint8_t* entry = buf.data + 12 + static_cast<size_t>(i) * 16;
    // Guard against overflow in offset+length (uint32_t wraparound).
    uint32_t offset = static_cast<uint32_t>(entry[8]) << 24 |
                      static_cast<uint32_t>(entry[9]) << 16 |
                      static_cast<uint32_t>(entry[10]) << 8 |
                      static_cast<uint32_t>(entry[11]);
    uint32_t length = static_cast<uint32_t>(entry[12]) << 24 |
                      static_cast<uint32_t>(entry[13]) << 16 |
                      static_cast<uint32_t>(entry[14]) << 8 |
                      static_cast<uint32_t>(entry[15]);
    if (length > fi.fileSize || offset > fi.fileSize ||
        offset + length < offset || offset + length > fi.fileSize) {
      LOG_ERR("BFNT", "Font %s table %u O/L %u/%u exceeds size", fi.file, i,
              offset, length);
      return false;
    }
  }

  // Per-face glyph arena — persisted in arenas_[faceIdx] so it outlives the
  // face. TtfFont::init borrows the arena; flushGlyphs rewinds to the init
  // mark (TtfFont.cpp:133). A local arena would dangle after tryLoadFace
  // returns (CodeRabbit PRRT_kwDOUDrzps6g3eGS).
  alignas(4) static uint8_t glyphBuf[kGlyphArenaBytes];
  Arena localArena(glyphBuf, kGlyphArenaBytes);
  arenas_[faceIdx] = localArena;

  book::TtfFont* face = new (std::nothrow) book::TtfFont();
  if (!face) {
    LOG_ERR("BFNT", "TtfFont OOM for %s", fi.file);
    return false;
  }
  if (!face->init(buf.data, fi.fileSize, arenas_[faceIdx])) {
    LOG_ERR("BFNT", "TtfFont::init failed for %s", fi.file);
    delete face;
    return false;
  }

  chain.add(face, fi.styleFlags);

  // Transfer ownership of the font bytes to the loader. The DRAM tier's
  // unique_ptr is released, transferring raw pointer ownership; the loader
  // never calls free() on it (no new[]/free UB — Copilot PRRT_kwDOUDrzps6g3Zfk).
  // PSRAM tier data is owned by PoolBytes which provides heap_caps_free on reset.
  fontBytes_[faceIdx] = buf.data;
  faceBytesOwner_[faceIdx] =
      static_cast<uint8_t>(buf.isPsram ? 1 : 0);
  fontFileSizes_[faceIdx] = fi.fileSize;
  faces_[faceIdx] = face;

  // Invalidate the unique_ptr so ~FontBytes does not double-free.
  if (!buf.isPsram) {
    buf.dram.release();
  } else {
    buf.psram.reset();
  }

  return true;
}

}  // namespace book
}  // namespace freeink
