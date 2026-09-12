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
// Builtin fallback: singleton FontChain over 4 BitmapBookFont instances
// placement-new'ed into PSRAM (each embeds coverage_[64*64]; formerly 16KB
// static BSS).
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

#if defined(CROSSPOINT_TTF_READER)
#include <builtinFonts/atkinson_hn_14_bold.h>
#include <builtinFonts/atkinson_hn_14_bolditalic.h>
#include <builtinFonts/atkinson_hn_14_italic.h>
#include <builtinFonts/atkinson_hn_14_regular.h>

#include "adapters/EpdBookFont.h"
#endif

#ifdef HOST_TEST
#include "Arduino.h"  // host-test stub for ESP.getFreeHeap
#endif

#include <algorithm>
#include <cctype>
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

// Device-lifetime fallback faces (owned by builtinFallback()'s singleton pool
// block). builtinFace() hands these out so appendFallbackTail() can register
// them as an active chain's tail without transferring ownership.
RenderFont* g_builtinFaces[4] = {};

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

// ── scanFonts — per-family TTF discovery (design §14.4) ──────────────────

#if defined(CROSSPOINT_TTF_READER) || defined(HOST_TEST)
namespace {
// Font roots. The hidden root is scanned first so it wins on family-name
// collisions, matching the SdCardFontRegistry sleep-folder pattern.
constexpr const char* kFontsRootHidden = "/.fonts";
constexpr const char* kFontsRootVisible = "/fonts";

// Case-insensitive ends-with on a null-terminated string.
bool endsWithIgnoreCase(const char* s, const char* suffix) {
  const size_t sLen = strlen(s);
  const size_t sufLen = strlen(suffix);
  if (sLen < sufLen) return false;
  for (size_t i = 0; i < sufLen; ++i) {
    if (tolower(static_cast<unsigned char>(s[sLen - sufLen + i])) != tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

// Word-boundary case-insensitive substring test: the token must start after
// a non-alphanumeric (or string start) and end before one, so "SemiBold"
// does not match "bold".
bool hasWord(const char* hay, const char* token) {
  const size_t tLen = strlen(token);
  for (size_t i = 0; hay[i] != '\0'; ++i) {
    if (i > 0 && isalnum(static_cast<unsigned char>(hay[i - 1]))) continue;
    size_t j = 0;
    while (token[j] != '\0' && hay[i + j] != '\0' && tolower(static_cast<unsigned char>(hay[i + j])) == token[j]) {
      ++j;
    }
    if (token[j] != '\0') continue;
    const char after = hay[i + tLen];
    if (after == '\0' || !isalnum(static_cast<unsigned char>(after))) return true;
  }
  return false;
}

// Style inference per design §14.4. Bold and italic are detected
// independently (so "Font-Bold-Italic.ttf" gets both flags); the fused
// "bolditalic"/"boldoblique" forms are matched explicitly because their
// halves never sit on word boundaries ("BoldOblique": "bold" ends inside
// the word, "oblique" starts inside it). "SemiBold" never matches "bold" —
// the weight heuristics own those names.
// `lower` is the lowercased filename stem.
bool inferStyleFlags(const char* lower, uint8_t& styleOut) {
  if (hasWord(lower, "bolditalic") || hasWord(lower, "boldoblique")) {
    styleOut = StyleBold | StyleItalic;
    return true;
  }
  const bool italic = hasWord(lower, "italic") || hasWord(lower, "oblique") || hasWord(lower, "ital");
  const bool bold = hasWord(lower, "bold");
  if (italic && bold) {
    styleOut = StyleBold | StyleItalic;
    return true;
  }
  if (italic) {
    styleOut = StyleItalic;
    return true;
  }
  if (bold) {
    styleOut = StyleBold;
    return true;
  }
  if (hasWord(lower, "regular") || hasWord(lower, "normal") || hasWord(lower, "book") || hasWord(lower, "roman") ||
      hasWord(lower, "text")) {
    styleOut = StyleNone;
    return true;
  }
  if (hasWord(lower, "semibold") || hasWord(lower, "demibold") || hasWord(lower, "medium") || hasWord(lower, "black") ||
      hasWord(lower, "heavy") || hasWord(lower, "extrabold")) {
    styleOut = StyleBold;
    return true;
  }
  if (hasWord(lower, "light") || hasWord(lower, "thin")) {
    styleOut = StyleNone;
    return true;
  }
  return false;
}

// Case-insensitive comparison for family dedupe and same-style duplicate
// resolution (lexicographically-first filename wins).
int ciCompare(const char* a, const char* b) {
  while (*a != '\0' && *b != '\0') {
    const int ca = tolower(static_cast<unsigned char>(*a));
    const int cb = tolower(static_cast<unsigned char>(*b));
    if (ca != cb) return ca - cb;
    ++a;
    ++b;
  }
  return tolower(static_cast<unsigned char>(*a)) - tolower(static_cast<unsigned char>(*b));
}
}  // namespace
#endif

// ── BookFontLoader implementation ────────────────────────────────────────────

BookFontLoader::BookFontLoader() = default;

BookFontLoader::~BookFontLoader() {
  // Delete loaded faces and release their byte/arena owners (glyphBacking_
  // releases its pool blocks via reset() in releaseResidentCaches()).
  releaseResidentCaches();
}

void BookFontLoader::begin() {
  // Release any live resident state FIRST (deleting loaded faces before
  // nulling their pointers), then reset the manifest and counters.
  releaseResidentCaches();
  familyCount_ = 0;
  families_ = {};
  dirty_.store(false, std::memory_order_relaxed);
#if defined(CROSSPOINT_TTF_READER)
  // Hidden root first so it wins on family-name collisions (§14.4).
  scanFonts(kFontsRootHidden, families_.data(), familyCount_);
  scanFonts(kFontsRootVisible, families_.data(), familyCount_);
#endif
  remainingBudget_ = 0;
  initBudget();
}

void BookFontLoader::ensureLoaded() {
  // loaded_ distinguishes "a load attempt completed" from "never attempted":
  // fingerprint 0 is a legitimate outcome (all faces rejected), so it cannot
  // be the loaded-state flag or every getReaderFont() re-runs the SD load.
  if (!dirty_.load(std::memory_order_relaxed) && loaded_) return;

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
    glyphBacking_[i].reset();
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
  appendFallbackTail(chain_);
  loaded_ = true;
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
    glyphBacking_[i].reset();
  }
  chain_ = FontChain{};
  fingerprint_ = 0;
  loaded_ = false;  // next getReaderFont() must re-attempt the load
}

uint32_t BookFontLoader::computeFingerprint() const {
  // FNV-1a over loaded face bytes (only valid ones) xor styleCoverage.
  // Never uses path or mtime — content-based (design §3.4). Returns 0 when
  // nothing loaded (sentinel distinct from any real FNV-1a result) so the
  // layout fingerprint doesn't depend on a failed load.
  bool anyLoaded = false;
  uint32_t h = 0x811c9dc5;
  for (uint8_t i = 0; i < 4; ++i) {
    if (fontBytes_[i] && fontFileSizes_[i] > 0) {
      h = fontFNV1a(static_cast<const uint8_t*>(fontBytes_[i]), fontFileSizes_[i], h);
      anyLoaded = true;
    }
  }
  if (!anyLoaded) return 0;
  h ^= static_cast<uint32_t>(chain_.styleCoverage());
  return h;
}

FontChain* BookFontLoader::builtinFallback() {
  // Singleton FontChain over 4 fallback faces (4 styles), placement-new'ed
  // into a pool block so their payloads live in PSRAM instead of static BSS
  // (PSRAM-only directive). The faces are intentional device-lifetime
  // singletons: destructors are never run so FontChain entries remain valid
  // after this function returns. The face pointers stay in a static table so
  // appendFallbackTail() can register the same faces as an active chain's
  // tail without owning them.
#if defined(CROSSPOINT_TTF_READER)
  // Reader chain (§14.5): four RenderFont adapters over the baked Atkinson
  // fonts so the whole chain speaks the same rasterize protocol.
  using FaceType = EpdBookFont;
#else
  using FaceType = freeink::ui::BitmapBookFont;
#endif
  static FontChain fallback;
  static PoolBytes backing;  // PoolBytes object itself is only a pointer of BSS
  static bool init = false;
  if (!init) {
    static constexpr size_t kFallbackBytes = 4 * sizeof(FaceType);
    backing = poolMakeBytes(kFallbackBytes);
    if (!backing) {
      LOG_ERR("BFNT", "OOM: %u bytes for builtin fallback fonts", static_cast<unsigned>(kFallbackBytes));
      return &fallback;  // empty chain (coverage 0); caller falls back further
    }
    // Slot addresses as byte offsets from the pool block: placement-new takes
    // void*, so do the byte arithmetic on char* (defined; void* arithmetic is
    // not — cppcheck portability gate) and let it implicitly convert to void*.
    // No typed pointer variable (cppcheck constVariablePointer), no destructor
    // call (see singleton note).
    // cppcheck-suppress constVariablePointer ; placement-new writes through these addresses
    auto* slots = reinterpret_cast<char*>(backing.get());
    constexpr auto faceSize = sizeof(FaceType);
#if defined(CROSSPOINT_TTF_READER)
    auto* r = new (slots + 0 * faceSize) FaceType(&atkinson_hn_14_regular);
    auto* b = new (slots + 1 * faceSize) FaceType(&atkinson_hn_14_bold);
    auto* i = new (slots + 2 * faceSize) FaceType(&atkinson_hn_14_italic);
    auto* bi = new (slots + 3 * faceSize) FaceType(&atkinson_hn_14_bolditalic);
#else
    auto* r = new (slots + 0 * faceSize) FaceType(freeink::ui::kNotoSansFont);
    auto* b = new (slots + 1 * faceSize) FaceType(freeink::ui::kNotoSansFont);
    auto* i = new (slots + 2 * faceSize) FaceType(freeink::ui::kNotoSansFont);
    auto* bi = new (slots + 3 * faceSize) FaceType(freeink::ui::kNotoSansFont);
#endif
    fallback.add(r, StyleNone);
    fallback.add(b, StyleBold);
    fallback.add(i, StyleItalic);
    fallback.add(bi, StyleBold | StyleItalic);
    g_builtinFaces[0] = r;
    g_builtinFaces[1] = b;
    g_builtinFaces[2] = i;
    g_builtinFaces[3] = bi;
    // Mark built only after full construction: a transient PSRAM failure
    // above must leave init false so the next call retries, instead of
    // permanently serving the empty chain.
    init = true;
  }
  return &fallback;
}

RenderFont* BookFontLoader::builtinFace(const uint8_t idx) {
  // Ensures the singleton is constructed, then hands out the device-lifetime
  // face pointer (null only when the pool backing failed; FontChain::add
  // treats a null font as a safe no-op).
  builtinFallback();
  return idx < 4 ? g_builtinFaces[idx] : nullptr;
}

void BookFontLoader::appendFallbackTail(FontChain& chain) {
  chain.add(builtinFace(0), StyleNone);
  chain.add(builtinFace(1), StyleBold);
  chain.add(builtinFace(2), StyleItalic);
  chain.add(builtinFace(3), StyleBold | StyleItalic);
}

#if defined(HOST_TEST)
void BookFontLoader::forceFallbackTailForTest() { appendFallbackTail(chain_); }
#endif
#if defined(CROSSPOINT_TTF_READER) || defined(HOST_TEST)
void BookFontLoader::scanFonts(const char* rootPath, FamilyInfo* families, uint8_t& familyCount) {
  HalFile root = Storage.open(rootPath);
  if (!root || !root.isDirectory()) {
    LOG_DBG("BFNT", "Font root not found: %s", rootPath);
    return;
  }

  // The walk frame would need ~550B of stack locals (over the 256B stack
  // budget, and scanFonts runs from boot wiring) — one heap scratch instead.
  struct ScanScratch {
    char dirName[48];   // FamilyInfo::name cap; longer folder names are skipped
    char fileName[64];  // FontFaceInfo::file cap minus dir prefix headroom
    char lower[64];     // lowercased stem
    char subPath[160];  // SdCardCacheStorage::kDirMax
    char soloFile[64];  // §14.4 rule 7: the lone candidate's name
    FamilyInfo fam;     // 552B manifest row — heap, reset per family
    uint32_t soloSize = 0;
  };
  // sizeof() on the decayed pointers would measure the pointer, not the
  // buffer — the walk uses the struct's member sizes everywhere.
  const auto scratch = makeUniqueNoThrow<ScanScratch>();
  if (!scratch) {
    LOG_ERR("BFNT", "OOM: scan scratch");
    return;
  }
  char* dirName = scratch->dirName;
  char* fileName = scratch->fileName;
  char* lower = scratch->lower;
  char* subPath = scratch->subPath;
  constexpr size_t kDirNameCap = sizeof(ScanScratch::dirName);
  constexpr size_t kFileNameCap = sizeof(ScanScratch::fileName);
  constexpr size_t kLowerCap = sizeof(ScanScratch::lower);
  constexpr size_t kSubPathCap = sizeof(ScanScratch::subPath);
  while (true) {
    HalFile dir = root.openNextFile();
    if (!dir) break;
    if (!dir.isDirectory()) continue;
    const size_t nameLen = dir.getName(dirName, kDirNameCap);

    // Skip hidden/system folders (macOS ._*, .Trashes, _folders).
    if (dirName[0] == '.' || dirName[0] == '_') continue;
    // Hidden root wins on dedupe: the later (visible) pass skips existing names.
    bool exists = false;
    for (uint8_t i = 0; i < familyCount; ++i) {
      if (ciCompare(families[i].name, dirName) == 0) {
        exists = true;
        break;
      }
    }
    if (exists) continue;
    if (familyCount >= kMaxDiscoveredFamilies) {
      LOG_DBG("BFNT", "Family cap reached, skipping %s", dirName);
      continue;
    }
    if (nameLen >= kDirNameCap - 1) {
      LOG_DBG("BFNT", "Family name too long: %s", dirName);
      continue;
    }

    FamilyInfo& fam = scratch->fam;
    fam = {};
    strncpy(fam.name, dirName, sizeof(fam.name) - 1);

    const int subLen = snprintf(subPath, kSubPathCap, "%s/%s", rootPath, dirName);
    if (subLen < 0 || static_cast<size_t>(subLen) >= kSubPathCap) continue;

    HalFile subdir = Storage.open(subPath);
    if (!subdir || !subdir.isDirectory()) continue;

    // Extension-accepted candidates (§14.4 rule 7: a family folder with
    // exactly one .ttf/.otf registers it as Regular even without style
    // tokens in the name).
    char* const soloFile = scratch->soloFile;
    uint32_t& soloSize = scratch->soloSize;
    uint8_t candidateCount = 0;

    while (true) {
      HalFile entry = subdir.openNextFile();
      if (!entry) break;
      if (entry.isDirectory()) continue;
      entry.getName(fileName, kFileNameCap);

      // Skip macOS resource forks, hidden files, editor backups.
      if (fileName[0] == '.' || fileName[0] == '_') continue;
      const size_t nameLen = strlen(fileName);
      if (nameLen > 0 && fileName[nameLen - 1] == '~') continue;
      const bool isTtf = endsWithIgnoreCase(fileName, ".ttf");
      if (!isTtf && !endsWithIgnoreCase(fileName, ".otf")) continue;
      if (candidateCount < UINT8_MAX) ++candidateCount;
      if (candidateCount == 1) {
        snprintf(soloFile, kFileNameCap, "%s", fileName);
        soloSize = entry.fileSize();
      }

      // Stem for style inference (extension stripped, lowercased).
      const size_t stemLen = nameLen - 4;
      if (stemLen == 0 || stemLen >= kLowerCap) continue;
      for (size_t i = 0; i < stemLen; ++i) {
        lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(fileName[i])));
      }
      lower[stemLen] = '\0';

      uint8_t style = 0;
      if (!inferStyleFlags(lower, style)) {
        LOG_DBG("BFNT", "No style tokens in %s/%s — skipped", fam.name, fileName);
        continue;
      }

      // Same-style duplicate: lexicographically-first filename wins.
      uint8_t slot = kMaxFacesPerFamily;
      for (uint8_t i = 0; i < fam.faceCount; ++i) {
        if (fam.faces[i].styleFlags == style) {
          slot = i;
          break;
        }
      }
      const bool replacingExisting = slot < kMaxFacesPerFamily;
      if (replacingExisting) {
        if (ciCompare(lower, fam.faces[slot].name) >= 0) continue;  // existing wins
      } else {
        if (fam.faceCount >= kMaxFacesPerFamily) continue;
      }
      // Validate the full path BEFORE touching the slot: a too-long path must
      // not clobber an existing face (or shrink the count of one).
      char newFile[kFileNameCap];
      if (snprintf(newFile, kFileNameCap, "%s/%s", subPath, fileName) >= static_cast<int>(kFileNameCap)) {
        LOG_DBG("BFNT", "Path too long for %s/%s", fam.name, fileName);
        continue;
      }
      if (!replacingExisting) slot = fam.faceCount++;

      FontFaceInfo& face = fam.faces[slot];
      face = {};
      snprintf(face.name, sizeof(face.name), "%s", lower);
      snprintf(face.file, sizeof(face.file), "%s", newFile);
      face.styleFlags = style;
      face.fileSize = entry.fileSize();
    }

    if (fam.faceCount == 0) {
      if (candidateCount != 1) continue;  // empty / unparseable family
      // Single-file family: register the lone face as Regular (§14.4).
      FontFaceInfo& face = fam.faces[0];
      fam.faceCount = 1;
      face = {};
      snprintf(face.name, sizeof(face.name), "%s", lower);
      if (snprintf(face.file, sizeof(face.file), "%s/%s", subPath, soloFile) >= static_cast<int>(sizeof(face.file))) {
        fam.faceCount = 0;
        continue;
      }
      face.fileSize = soloSize;
      face.styleFlags = StyleNone;
    } else {
      // A family with files but no Regular face promotes its lexicographically-
      // first face (case-insensitive) to Regular.
      bool hasRegular = false;
      for (uint8_t i = 0; i < fam.faceCount; ++i) {
        if (fam.faces[i].styleFlags == StyleNone) {
          hasRegular = true;
          break;
        }
      }
      if (!hasRegular) {
        int first = 0;
        for (uint8_t i = 1; i < fam.faceCount; ++i) {
          if (ciCompare(fam.faces[i].name, fam.faces[first].name) < 0) first = i;
        }
        fam.faces[first].styleFlags = StyleNone;
      }
    }

    families[familyCount++] = fam;
    LOG_DBG("BFNT", "Family %s: %u faces from %s", fam.name, fam.faceCount, rootPath);
  }
}
#endif

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
    // NOTE: poolMalloc cannot serve this fallback — on PSRAM builds it is
    // PSRAM-only (heap_caps_malloc MALLOC_CAP_SPIRAM, no runtime DRAM
    // fallback), so an exhausted PSRAM pool needs an explicit plain-DRAM
    // allocation, which is why the tier stays tracked in faceBytesOwner_.
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
  // Size by SDK profile (kGlyphArenaBytes in the header): TtfFont's slot
  // tables alone need 4.6KB (SMALL), 9.2KB (STANDARD), 36.9KB (LARGE)
  // before any glyph bitmap — 8KB fails STANDARD at TtfFont::init.
  // Maximal alignment: Arena::allocArray aligns the OFFSET from base_, and
  // TtfFont allocates GlyphSlot (uint64_t key) through it. Pool blocks come
  // from heap_caps_malloc (≥4-byte aligned), which covers GlyphSlot's
  // uint64_t key on ESP32 (its natural alignment is 4 on this 32-bit ABI).
  if (!glyphBacking_[faceIdx]) {
    glyphBacking_[faceIdx] = poolMakeBytes(kGlyphArenaBytes);
    if (!glyphBacking_[faceIdx]) {
      LOG_ERR("BFNT", "Glyph arena OOM for %s (%u bytes)", fi.file, static_cast<unsigned>(kGlyphArenaBytes));
      if (isPsram) {
        fontPsramBytes_[faceIdx].reset();
      } else {
        localDram.reset();
      }
      return false;
    }
  }
  arenas_[faceIdx] = Arena(glyphBacking_[faceIdx].get(), kGlyphArenaBytes);

  TtfFont* face = new (std::nothrow) TtfFont();
  if (!face) {
    LOG_ERR("BFNT", "TtfFont OOM for %s", fi.file);
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    glyphBacking_[faceIdx].reset();
    arenas_[faceIdx] = Arena{};
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
    glyphBacking_[faceIdx].reset();
    arenas_[faceIdx] = Arena{};
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
    glyphBacking_[faceIdx].reset();
    arenas_[faceIdx] = Arena{};
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
