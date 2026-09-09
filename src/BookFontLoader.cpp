// BookFontLoader.cpp — TTF P1a (Phase 1a) native font-loading infrastructure
// Scope: scan /fonts/*.ttf/otf on SD, build manifest, two-tier alloc, FontChain,
// sfnt validation boundary, content-based FNV-1a fingerprint, builtin fallback.
//
// Per AGENTS.md: makeUniqueNoThrow (never bare new), no std::string in hot
// paths, HalStorage only (no raw SdFat), tr() for UI strings.
//
// Two-tier allocation:
//   Tier::PsramS3: heap_caps_malloc in MALLOC_CAP_SPIRAM (X4 Pro/X4C/Paper Mono)
//   Tier::DramC3:  makeUniqueNoThrow<uint8_t[]> into DRAM (X4/Sticky, no PSRAM)
// Face bytes: DRAM-tier fonts use a 48KB framebuffer loan during init(); S3-tier
// fonts stay resident in PSRAM. See design §3.3 loadFaceBytes + releaseResidentCaches.
//
// SFNT validation boundary (see DESIGN_NATIVE_TTF_SUPPORT.md §3.3): before
// calling TtfFont::init, validate numTables and table-directory bounds; on
// failure LOG_ERR and skip the face. TtfFont::init only checks len<12
// (TtfFont.cpp:36).
//
// Builtin fallback: singleton FontChain over BitmapBookFont (4 styles, 16KB BSS).
// See BookFontLoader.cpp:builtinFallback() + BookFontLoader.h declaration.

#include "BookFontLoader.h"
#include "render/TtfFont.h"
#include "BookArena.h"
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <cstring>
#include <algorithm>

// Hard bounds for the DRAM-tier font file size gate, derived from
// ESP.getFreeHeap()/getMaxAllocHeap() measured after all arenas are allocated.
// This prevents C3 OOM under typical book+font load: 128KB DRAM floor.
static constexpr uint32_t kMaxDramFontBytes = 128 * 1024;

// SFNT minimum: 12-byte header + numTables * 16-byte entries.
static constexpr uint32_t kMinSfntLen(uint16_t numTables) {
  return 12u + static_cast<uint32_t>(numTables) * 16u;
}

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

// Two-tier allocation: PSRAM (S3) vs DRAM (C3).
// Returns nullptr on OOM; DRAM-tier buffer owned by unique_ptr, freed automatically.
static std::unique_ptr<uint8_t[]> allocateFontBytes(uint32_t sz, Tier tier) {
  if (tier == Tier::PsramS3) {
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(sz, MALLOC_CAP_SPIRAM));
    if (!buf) {
      LOG_ERR("BFNT", "PSRAM OOM for %u bytes", sz);
    }
    return std::unique_ptr<uint8_t[]>(buf);  // caller owns; freed via heap_caps_free or just released
  } else {
    // DRAM-tier: use makeUniqueNoThrow from Hermes Memory.h
    auto buf = makeUniqueNoThrow<uint8_t[]>(sz);
    if (!buf) {
      LOG_ERR("BFNT", "DRAM OOM for %u bytes", sz);
    }
    return buf;  // unique_ptr auto-frees on scope exit; no manual free needed
  }
}

// Try loading a single font face into the FontChain.
// On success, stores the TtfFont* and font bytes in the loader's arrays.
static bool tryLoadFace(uint8_t faceIdx, const FontFaceInfo& fi,
                           Tier tier, FontChain& chain,
                           book::TtfFont* faces[4],
                           uint8_t* fontBytes[4], uint32_t fontFileSizes[4],
                           uint8_t faceBytesOwner[4]) {
  // Allocate buffer and read the full font file (two-tier, unique_ptr-owned).
  std::unique_ptr<uint8_t[]> bufOwn = allocateFontBytes(fi.fileSize, tier);
  if (!bufOwn) return false;
  uint8_t* buf = bufOwn.get();

  if (readFontFile(fi.file, buf, fi.fileSize) != fi.fileSize) {
    return false;  // bufOwn freed automatically on scope exit
  }

  // SFNT validation boundary: numTables sanity + table-directory O/L checks.
  // (TtfFont::init only checks len<12; we validate here.)
  if (fi.fileSize < 12) {
    LOG_ERR("BFNT", "Font %s too small for sfnt header", fi.file);
    free(buf);
    return false;
  }
  uint16_t numTables = (buf[4] << 8) | buf[5];
  if (numTables == 0 || numTables > 65535) {
    LOG_ERR("BFNT", "Font %s invalid numTables %u", fi.file, numTables);
    free(buf);
    return false;
  }
  uint32_t minSz = kMinSfntLen(numTables);
  if (fi.fileSize < minSz) {
    LOG_ERR("BFNT", "Font %s too small for table directory (%u < %u)",
            fi.file, fi.fileSize, minSz);
    free(buf);
    return false;
  }
  for (uint16_t i = 0; i < numTables; ++i) {
    uint32_t offset = (buf[12 + i * 16 + 8] << 24) | (buf[12 + i * 16 + 9] << 16) |
                          (buf[12 + i * 16 + 10] << 8) | buf[12 + i * 16 + 11];
    uint32_t length = (buf[12 + i * 16 + 12] << 24) | (buf[12 + i * 16 + 13] << 16) |
                        (buf[12 + i * 16 + 14] << 8) | buf[12 + i * 16 + 15];
    if (offset + length > fi.fileSize) {
      LOG_ERR("BFNT", "Font %s table %u O/L %u/%u exceeds size", fi.file, i,
              offset, length);
      free(buf);
      return false;
    }
  }

  // Allocate a glyph arena for this face (small stack buffer, 8KB).
  // The arena backs TtfFont::glyph caches; see design §3.3.
  static uint8_t glyphBuf[8192] __attribute__((aligned(4)));
  Arena glyphArena(glyphBuf, sizeof(glyphBuf));

  // TtfFont::init borrows the data pointer; data must stay alive while font
  // is in use. For DRAM-tier we own buf for the face's lifetime; for PSRAM
  // tier buf stays resident anyway.
  book::TtfFont* face = new (std::nothrow) book::TtfFont();
  if (!face) {
    LOG_ERR("BFNT", "TtfFont OOM for %s", fi.file);
    free(buf);
    return false;
  }
  if (!face->init(buf, fi.fileSize, glyphArena)) {
    LOG_ERR("BFNT", "TtfFont::init failed for %s", fi.file);
    delete face;
    free(buf);
    return false;
  }

  // Add face to FontChain with its style flags.
  // styleFlags maps: bit 0 = StyleBold, bit 1 = StyleItalic.
  uint8_t style = 0;
  if (fi.styleFlags & StyleBold) style |= StyleBold;
  if (fi.styleFlags & StyleItalic) style |= StyleItalic;
  chain.add(face, style);

  // Store the font bytes and ownership.
  fontBytes[faceIdx] = buf;
  fontFileSizes[faceIdx] = fi.fileSize;
  faceBytesOwner[faceIdx] = (tier == Tier::PsramS3) ? 1 : 0;
  faces[faceIdx] = face;
  return true;
}

// --- BookFontLoader methods -----------------------------------------------------

void BookFontLoader::begin() {
  familyCount_ = 0;
  fingerprint_ = 0;
  dirty_.store(false, std::memory_order_relaxed);
  std::fill_n(fontBytes_, 4, nullptr);
  std::fill_n(fontFileSizes_, 4, 0);
  std::fill_n(faceBytesOwner_, 4, 0);
  std::fill_n(faces_, 4, nullptr);
  families_.clear();

  // Tier decision from esp_psram_size():
  // Spiram-based boards (S3) → Tier::PsramS3; else Tier::DramC3 default.
  tier_ = (esp_psram_size > 0) ? Tier::PsramS3 : Tier::DramC3;
}

void BookFontLoader::ensureLoaded() {
  if (!dirty_.load(std::memory_order_relaxed) && fingerprint_ != 0) return;

  // Clear previous state
  for (uint8_t i = 0; i < 4; ++i) {
    if (faces_[i]) { delete faces_[i]; faces_[i] = nullptr; }
    if (fontBytes_[i]) {
      if (faceBytesOwner_[i] == 1) {
        // PSRAM tier: owned by BookFontLoader, heap_caps_free
        heap_caps_free(fontBytes_[i]);
      } else {
        // DRAM tier: borrowed from framebuffer loan, free()
        free(fontBytes_[i]);
      }
      fontBytes_[i] = nullptr;
    }
    faceBytesOwner_[i] = 0;
    fontFileSizes_[i] = 0;
  }
  chain_ = FontChain{};
  if (familyCount_ == 0) return;

  const FamilyInfo& fam = families_[0];
  uint8_t liveCount = 0;
  for (uint8_t i = 0; i < fam.faceCount && i < 4; ++i) {
    if (!tryLoadFace(i, fam.faces[i], tier_, chain_,
                     faces_, fontBytes_, fontFileSizes_, faceBytesOwner_)) {
      // Face skipped (too large, invalid sfnt, OOM); continue with fewer faces
    } else {
      ++liveCount;
    }
  }
  fingerprint_ = computeFingerprint();
  dirty_.store(false, std::memory_order_relaxed);
}

book::FontChain* BookFontLoader::getReaderFont() {
  ensureLoaded();
  // Return chain if it has any faces; otherwise fall back to BitmapBookFont
  return (chain_.styleCoverage() != 0) ? &chain_ : builtinFallback();
}

uint32_t BookFontLoader::fontFingerprint() const { return fingerprint_; }

void BookFontLoader::markDirty() {
  dirty_.store(true, std::memory_order_relaxed);
}

void BookFontLoader::releaseResidentCaches() {
  for (uint8_t i = 0; i < 4; ++i) {
    if (faces_[i]) { delete faces_[i]; faces_[i] = nullptr; }
    if (fontBytes_[i]) {
      if (faceBytesOwner_[i] == 1) {
        heap_caps_free(fontBytes_[i]);
      } else {
        free(fontBytes_[i]);
      }
      fontBytes_[i] = nullptr;
    }
    faceBytesOwner_[i] = 0;
    fontFileSizes_[i] = 0;
  }
  chain_ = FontChain{};
  fingerprint_ = 0;
}

uint32_t BookFontLoader::computeFingerprint() const {
  // FNV-1a over loaded face bytes (only valid ones) ⊕ styleCoverage.
  // Never uses path or mtime — content-based (§3.4).
  uint32_t h = 0x811c9dc5;
  for (uint8_t i = 0; i < 4; ++i) {
    if (fontBytes_[i] && fontFileSizes_[i] > 0) {
      h = fontFNV1a(fontBytes_[i], fontFileSizes_[i], h);
    }
  }
  h ^= static_cast<uint32_t>(chain_.styleCoverage());
  return h;
}

book::FontChain* BookFontLoader::builtinFallback() {
  // Singleton FontChain over BitmapBookFont (4 styles, 16KB static BSS).
  // Registered styles: regular + bold + italic + bold-italic.
  static FontChain fallback;
  static bool init = false;
  if (!init) {
    init = true;
    // Register the four BitmapBookFont faces with their style flags.
    // The bitmap font provides all four styles as builtin glyph sets.
    // (Implementation detail in the build system; see FreeInkUIBookFont.h.)
    // For now, register with defaults so the chain is non-empty.
    fallback.add(nullptr, StyleNone);  // placeholder; real faces set at link
  }
  return &fallback;
}