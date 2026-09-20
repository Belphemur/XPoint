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

#include "MemSentinel.h"

#if defined(CROSSPOINT_TTF_READER)
#include <builtinFonts/atkinson_hn_14_bold.h>
#include <builtinFonts/atkinson_hn_14_bolditalic.h>
#include <builtinFonts/atkinson_hn_14_italic.h>
#include <builtinFonts/atkinson_hn_14_regular.h>

#include "adapters/EpdBookFont.h"
#endif

#ifdef HOST_TEST
#include "Arduino.h"  // host-test stub for ESP.getFreeHeap
#if defined(ARDUINO) && defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
#include <freertos/task.h>  // P2 hint stack probe: uxTaskGetStackHighWaterMark
#endif
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

namespace freeink {
namespace book {

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
// Style → FreeType axis coordinate (design §4.1): bold = wght 700, else 400.
// Italic is passed separately; FT synthesizes oblique/embolden when an axis
// is absent.
constexpr int styleToWeight(uint8_t styleFlags) { return (styleFlags & StyleBold) ? 700 : 400; }

// P2 effective render options, one slot per family face (REGULAR, BOLD,
// ITALIC, BOLD_ITALIC). Starts at the requested mode; probeHintStackSafety()
// may degrade a slot to unhinted. File-scope so renderOptionsFingerprintTag()
// stays a cheap register-fold, and so the prefetch worker (via the static
// accessor) folds the identical state into its parity hash. Written only on
// the loopTask inside ensureLoaded(); the worker reads it between loader
// generations, so the sequencing keeps the accesses non-overlapping.
freeink::font::FtFont::RenderOptions effectiveRenderOptions_[4] = {
    BookFontLoader::kCrispRenderOptions, BookFontLoader::kCrispRenderOptions, BookFontLoader::kCrispRenderOptions,
    BookFontLoader::kCrispRenderOptions};

// Additional stack depth a face's hinted render may consume before the probe
// degrades it. The bound protects the smallest consumer stack: the 32KB
// FibpPrefetchWorker task (R1: 24KB overflowed on device, its pipeline
// measured HWM 1792B — a ~22.2KB peak), so at most ~9.8KB remain for the
// Adobe interpreter's frames when it re-renders the same hinted glyphs;
// 8KB keeps ≥1.8KB of that task's headroom at the absolute peak.
constexpr uint32_t kHintProbeStackBudgetBytes = 8 * 1024;

// Fail-closed floor for the probe's lifetime-HWM hole: uxTaskGetStackHigh-
// WaterMark is a lifetime minimum, so the before/after delta UNDER-reports
// whenever the calling task had already gone deeper than the probe reaches.
// If the loopTask's lifetime free stack is below budget + reader-pipeline
// peak (~13KB) + margin by the time the probe samples, the measurement can
// no longer be attributed to the probe — degrade instead of trusting it.
constexpr uint32_t kHintProbeFloorBytes = kHintProbeStackBudgetBytes + 13 * 1024 + 2 * 1024;

// Stress set for the probe: hinted outlines with distinctive contours, at
// sizes spanning the reader's runtime range (body, ruby, large) so the
// deepest autohint/Adobe paths are exercised.
constexpr uint32_t kHintProbeCodepoints[] = {'A', 'g', 'M', '@', 0x00C6u, 0x2019u};
constexpr uint16_t kHintProbeSizeMultipliers[] = {1, 2, 4};

void BookFontLoader::degradeHint(uint8_t faceSlot) {
  freeink::font::FtFont::RenderOptions degraded = effectiveRenderOptions_[faceSlot];
  degraded.hinting = freeink::font::FtFont::HintingMode::None;
  // Route through setRenderOptions(): the P1 glyph-cache flush point, so no
  // stale hinted bitmap survives the mode change (review contract).
  applySlotRenderOptions(faces_[faceSlot], faceSlot, degraded, "stack probe");
  LOG_ERR("BFNT", "Hinting degraded to None for face slot %u (stack probe)", faceSlot);
}

void BookFontLoader::applySlotRenderOptions(NativeFace* face, uint8_t faceSlot,
                                            const freeink::font::FtFont::RenderOptions& requested, const char* label) {
  freeink::font::FtFont::RenderOptions opts = requested;
  if (face != nullptr && !face->setRenderOptions(opts)) {
    // Degrade to the nearest SUPPORTED set — never keep unsupported options:
    // a mono request without the compiled-in mono module rasterizes EVERY
    // glyph to nullptr (blank page, the device regression this guards).
    opts.monochrome = false;  // drop mono first: AA Smooth is the nearest render
    if (!face->setRenderOptions(opts)) {
      opts.hinting = freeink::font::FtFont::HintingMode::None;
      if (!face->setRenderOptions(opts)) {
        LOG_ERR("BFNT", "Render options unusable (slot %u, %s)", faceSlot, label);
      }
    } else {
      LOG_ERR("BFNT", "Render options degraded to AA (slot %u, %s)", faceSlot, label);
    }
  }
  // The effective set drives the fingerprint tag and every paint-path mode
  // decision (effectiveMonochrome), so FIBP identity always matches what
  // actually renders.
  effectiveRenderOptions_[faceSlot] = opts;
}

const freeink::font::FtFont::RenderOptions& BookFontLoader::effectiveRenderOptions(uint8_t faceSlot) {
  return effectiveRenderOptions_[faceSlot];
}

bool BookFontLoader::effectiveMonochrome() { return effectiveRenderOptions_[0].monochrome; }

void BookFontLoader::applyRenderMode(bool crispMode) {
  // Crisp/Smooth switch without a face reload: the mode changes glyph
  // RASTERIZATION only (advances are unchanged — Light hinting is on in
  // both modes), so the faces stay resident. setRenderOptions() is the P1
  // glyph-cache flush point; the fingerprint tag folds the new mode, so
  // FIBP cache identity regenerates on the next layoutGenerationHash. No
  // stack re-probe: the Adobe interpreter footprint is the same in both
  // modes (only the rasterizer differs), so the load-time probe verdict
  // stays valid.
  requestedMonochrome_ = crispMode;
  for (uint8_t i = 0; i < 4; ++i) {
    applySlotRenderOptions(faces_[i], i, currentRenderOptions(), "applyRenderMode");
  }
  // Re-derive the cached fingerprint so the next generation hash sees the
  // new tag (cheap post-P3.1; 0-consistent for unloaded/fallback states).
  if (loaded_) fingerprint_ = computeFingerprintCached();
}

uint32_t BookFontLoader::renderOptionsFingerprintTag() {
  // 3 bits per slot for HintingMode (values fit 0..4). ONLY hinting folds
  // here: hinting changes ADVANCES, so it is part of layout identity (FIBP
  // gen). The raster mode (monochrome vs AA) alters glyph BITMAPS only —
  // advances are byte-identical across modes — so folding it would
  // re-index every book on a firmware update that merely flips the Crisp
  // default (the soak observed exactly that); bitmap identity is governed
  // by the P1 glyph cache's setRenderOptions() flush instead.
  uint32_t tag = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    tag |= static_cast<uint32_t>(effectiveRenderOptions_[i].hinting) << (3 * i);
  }
  return tag;
}

#if defined(ARDUINO)
void BookFontLoader::probeHintStackSafety() {
  if (kRenderOptions.hinting == freeink::font::FtFont::HintingMode::None) return;
  for (uint8_t i = 0; i < 4; ++i) {
    NativeFace* face = faces_[i];
    if (face == nullptr) continue;
    const UBaseType_t before = uxTaskGetStackHighWaterMark(nullptr);  // bytes on ESP-IDF
    for (const uint32_t cp : kHintProbeCodepoints) {
      for (const uint16_t mult : kHintProbeSizeMultipliers) {
        face->rasterize(cp, static_cast<uint16_t>(kInitSizePx * mult));
      }
    }
    const UBaseType_t after = uxTaskGetStackHighWaterMark(nullptr);
    const uint32_t consumed = before > after ? before - after : 0;
    LOG_DBG("BFNT", "Hint probe slot %u consumed %u B", i, static_cast<unsigned>(consumed));
    if (after < kHintProbeFloorBytes) {
      // The task's lifetime high-water already ran deeper than the probe can
      // attribute (before/after delta under-reports by construction then).
      LOG_ERR("BFNT", "Hint probe slot %u unmeasurable (HWM %u B < floor)", i, static_cast<unsigned>(after));
      degradeHint(i);
    } else if (consumed > kHintProbeStackBudgetBytes) {
      degradeHint(i);
    }
  }
}
#else
void BookFontLoader::probeHintStackSafety() {}
#endif  // ARDUINO

void BookFontLoader::resetHintState() {
  for (uint8_t i = 0; i < 4; ++i) effectiveRenderOptions_[i] = currentRenderOptions();
}
#endif  // CROSSPOINT_FONT_BACKEND_FT

// Hard bounds for the DRAM-tier font file size gate. Design §3.3: the value is
// derived from ESP.getFreeHeap()/getMaxAllocHeap() after all arenas are
// allocated; this constant is the current placeholder (128KB floor).
static constexpr uint32_t kMaxDramFontBytes = 128 * 1024;

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

// ── SD fingerprint cache (P3.1) ───────────────────────────────────────────
// One 28-byte record per face under /.crosspoint/fonts/, keyed by the face's
// path hash: {magic, version, inSeed, hash, headHash, fileSize, mtime}.
// `hash` is the CHAINED FNV-1a over the face bytes with incoming seed
// `inSeed`, so it is only served when the recorded seed matches the running
// chain — a face's entry can never poison the fingerprint after a
// predecessor changed. mtime is only a rehash trigger, never a hash input
// (design P3.1). `headHash` is the FNV-1a over the first kFingerprintHeadBytes
// bytes: verifying it on a hit (~4 KB of the resident bytes, ~2% of a full
// walk) closes the same-size/same-mtime rewrite hole from review — a font
// replacement changes the header (checkSumAdjustment/head timestamps) with
// near-certainty. Residual window: a rewrite keeping size, mtime AND the
// whole first 4 KB identical — accepted and documented (design P3.1).
struct FingerprintCacheRecord {
  uint32_t magic;    // 'BFP2'
  uint32_t version;  // 2
  uint32_t inSeed;
  uint32_t hash;
  uint32_t headHash;
  uint32_t fileSize;
  uint32_t mtime;
};
static_assert(sizeof(FingerprintCacheRecord) == 28, "fixed-size SD record");
static_assert(alignof(FingerprintCacheRecord) <= alignof(uint32_t), "no alignment surprises");
constexpr uint32_t kFingerprintCacheMagic = 0x42465032u;  // 'BFP2'
constexpr uint32_t kFingerprintCacheVersion = 2;
constexpr size_t kFingerprintCacheBytes = sizeof(FingerprintCacheRecord);
constexpr size_t kFingerprintHeadBytes = 4096;

// Cache file path for a face's path hash. Fixed cap: 8 hex digits + dir.
void fingerprintCachePath(uint32_t pathHash, char (&out)[64]) {
  snprintf(out, sizeof(out), "/.crosspoint/fonts/fp_%08x.bin", static_cast<unsigned>(pathHash));
}

// Unaligned-buffer-safe record decode (RISC-V alignment rule): fields are
// uint32 at 4-byte stride, so memcpy each instead of casting the buffer.
bool decodeFingerprintRecord(const uint8_t* buf, size_t len, FingerprintCacheRecord& rec) {
  if (len != kFingerprintCacheBytes) return false;
  memcpy(&rec, buf, kFingerprintCacheBytes);  // 24-byte POD at a heap-aligned buffer start
  return rec.magic == kFingerprintCacheMagic && rec.version == kFingerprintCacheVersion;
}

// Returns true + fills `hash` when a valid cache record matches the face's
// current {fileSize, mtime, headHash} and the chain's incoming seed. mtime 0
// (missing SD timestamp) disables the cache for that face: size alone cannot
// tell a rewritten file apart, so fail closed and recompute.
bool BookFontLoader_readFingerprintCache(uint32_t pathHash, uint32_t fileSize, uint32_t mtime, uint32_t inSeed,
                                         uint32_t headHash, uint32_t& hash) {
  if (mtime == 0) return false;
  char path[64];
  fingerprintCachePath(pathHash, path);
  HalFile file;
  if (!Storage.openFileForRead("BFNT", path, file)) return false;
  if (file.fileSize() != kFingerprintCacheBytes) return false;  // corrupt/oversize → recompute
  uint8_t buf[kFingerprintCacheBytes];
  if (file.read(buf, kFingerprintCacheBytes) != static_cast<int>(kFingerprintCacheBytes)) return false;
  FingerprintCacheRecord rec{};
  if (!decodeFingerprintRecord(buf, kFingerprintCacheBytes, rec)) return false;
  if (rec.fileSize != fileSize || rec.mtime != mtime || rec.inSeed != inSeed || rec.headHash != headHash) {
    return false;
  }
  hash = rec.hash;
  return true;
}

// Writes the record atomically: stage to <path>.tmp, close, then rename over
// the final path. A partial write can never leave a torn final record (the
// rename publishes only complete 28-byte stages), and every failure path
// removes the temp file. Best effort — a failed write only costs the next
// open one byte-walk rehash.
void BookFontLoader_writeFingerprintCache(uint32_t pathHash, uint32_t fileSize, uint32_t mtime, uint32_t inSeed,
                                          uint32_t headHash, uint32_t hash) {
  if (mtime == 0) return;  // cache disabled without a usable rehash trigger
  char path[64];
  char tmpPath[70];
  fingerprintCachePath(pathHash, path);
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  Storage.ensureDirectoryExists("/.crosspoint/fonts");
  bool staged = false;
  {
    // Scope: the handle must be closed before the rename/remove below
    // (DESTRUCTOR_CLOSES_FILE=1 — close happens at the block exit).
    HalFile file;
    const FingerprintCacheRecord rec{
        kFingerprintCacheMagic, kFingerprintCacheVersion, inSeed, hash, headHash, fileSize, mtime};
    staged = Storage.openFileForWrite("BFNT", tmpPath, file) &&
             file.write(&rec, kFingerprintCacheBytes) == kFingerprintCacheBytes;
  }
  if (!staged) {
    LOG_DBG("BFNT", "fp-cache stage failed for %s", tmpPath);
    if (!Storage.remove(tmpPath)) LOG_ERR("BFNT", "fp-cache: stale temp %s", tmpPath);
    return;
  }
  // SdFat rename refuses an existing destination: publish by replace.
  if (Storage.exists(path) && !Storage.remove(path)) {
    LOG_ERR("BFNT", "fp-cache: cannot replace %s", path);
    if (!Storage.remove(tmpPath)) LOG_ERR("BFNT", "fp-cache: stale temp %s", tmpPath);
    return;
  }
  if (!Storage.rename(tmpPath, path)) {
    LOG_DBG("BFNT", "fp-cache publish failed for %s", path);
    if (!Storage.remove(tmpPath)) LOG_ERR("BFNT", "fp-cache: stale temp %s", tmpPath);
  }
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
    facePathHash_[i] = 0;
    faceMtime_[i] = 0;
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
    // Fresh load: re-probe hinting from the requested mode.
    resetHintState();
#endif
    arenas_[i] = Arena{};
    glyphBacking_[i].reset();
  }
  chain_ = FontChain{};
  if (familyCount_ == 0) {
    // No manifest: no real fingerprint exists. Zero it so a cache generation
    // derived from the stale value can't collide with a previously loaded
    // family's caches; dirty stays clear until the next begin()/selectFamily.
    fingerprint_ = 0;
    dirty_.store(false, std::memory_order_relaxed);
    loaded_ = true;  // a load attempt completed — the short-circuit must hold
    return;
  }

  // Recompute the DRAM budget: the release loop above freed the previous
  // faces' bytes, so a reload must not inherit the previously spent budget.
  initBudget();

  // Phase 3 family selection (design §3.6): the SETTINGS-driven reader and
  // preview call selectFamily() first; an empty selection means the built-in
  // fallback chain. An unselected loader keeps the legacy families_[0]
  // default so the debug rig (and any pre-settings consumer) still works.
  const FamilyInfo* famPtr = nullptr;
  if (!familySelected_) {
    famPtr = &families_[0];
  } else if (selectedFamily_[0] != '\0') {
    famPtr = findFamily(selectedFamily_);
    if (famPtr == nullptr) {
      LOG_DBG("BFNT", "Selected family '%s' not found — built-in fallback", selectedFamily_);
    }
  }  // explicit fallback selection: famPtr stays null
  if (famPtr == nullptr) {
    // Fallback chain (explicit or renamed family): fingerprint_ would
    // otherwise keep the previous family's value and derive a stale FIBP
    // generation from it.
    fingerprint_ = 0;
    dirty_.store(false, std::memory_order_relaxed);
    loaded_ = true;  // a load attempt completed — the short-circuit must hold
    return;          // chain stays empty; getReaderFont() serves the fallback
  }

  for (uint8_t i = 0; i < famPtr->faceCount && i < 4; ++i) {
    if (!tryLoadFace(i, famPtr->faces[i], chain_)) {
      // Face skipped (too large, invalid sfnt, OOM); continue with fewer faces.
    }
  }
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // P2 stack gate: probe the requested (Light) hinting BEFORE the fingerprint
  // is computed, so a probe-driven degrade participates in the FIBP identity
  // from the first cache write onward.
  probeHintStackSafety();
#endif
  fingerprint_ = computeFingerprintCached();
  memSentinelCheck("font ensureLoaded");
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

void BookFontLoader::selectFamily(const char* name) {
  const char* clean = name != nullptr ? name : "";
  if (familySelected_ && strncmp(selectedFamily_, clean, sizeof(selectedFamily_)) == 0) return;
  strncpy(selectedFamily_, clean, sizeof(selectedFamily_) - 1);
  selectedFamily_[sizeof(selectedFamily_) - 1] = '\0';
  familySelected_ = true;
  markDirty();
}

const FamilyInfo* BookFontLoader::findFamily(const char* name) const {
  if (name == nullptr || name[0] == '\0') return nullptr;
  // Case-insensitive per the loader contract (§14.4 display names): settings
  // can round-trip through the web UI/JSON with different casing, and a
  // renamed family on SD still degrades to the fallback.
  const auto match = [name](const FamilyInfo& fam) { return strcasecmp(fam.name, name) == 0; };
  const auto it = std::find_if(families_.begin(), families_.begin() + familyCount_, match);
  return it != families_.begin() + familyCount_ ? &*it : nullptr;
}

bool BookFontLoader::isFamilyAvailable(const FamilyInfo& fam) {
  if (HalMemory::getPsramHeap().totalBytes == 0) return false;
  for (uint8_t i = 0; i < fam.faceCount && i < 4; ++i) {
    if (fam.faces[i].fileSize > kMaxFaceBytes) return false;
  }
  return true;
}

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
    facePathHash_[i] = 0;
    faceMtime_[i] = 0;
    arenas_[i] = Arena{};
    glyphBacking_[i].reset();
  }
  chain_ = FontChain{};
  fingerprint_ = 0;
  loaded_ = false;  // next getReaderFont() must re-attempt the load
}

uint32_t BookFontLoader::fontBytesHash(const uint8_t* data, const size_t len, const uint32_t seed) {
  return fontFNV1a(data, len, seed);
}

bool BookFontLoader::validateSfntBytes(const uint8_t* data, const uint32_t size) {
  if (data == nullptr) return false;
  if (size < 12) {
    LOG_ERR("BFNT", "Font too small for sfnt header (%u bytes)", size);
    return false;
  }
  const uint16_t numTables = static_cast<uint16_t>((data[4] << 8) | data[5]);
  if (numTables == 0) {
    LOG_ERR("BFNT", "Font invalid numTables 0");
    return false;
  }
  const uint32_t minSz = kMinSfntLen(numTables);
  if (size < minSz) {
    LOG_ERR("BFNT", "Font too small for table directory (%u < %u)", size, minSz);
    return false;
  }
  for (uint16_t i = 0; i < numTables; ++i) {
    const uint8_t* entry = data + 12 + static_cast<size_t>(i) * 16;
    // Guard against overflow in offset+length (uint32_t wraparound).
    const uint32_t offset = static_cast<uint32_t>(entry[8]) << 24 | static_cast<uint32_t>(entry[9]) << 16 |
                            static_cast<uint32_t>(entry[10]) << 8 | static_cast<uint32_t>(entry[11]);
    const uint32_t length = static_cast<uint32_t>(entry[12]) << 24 | static_cast<uint32_t>(entry[13]) << 16 |
                            static_cast<uint32_t>(entry[14]) << 8 | static_cast<uint32_t>(entry[15]);
    if (length > size || offset > size || offset + length < offset || offset + length > size) {
      LOG_ERR("BFNT", "Font table %u O/L %u/%u exceeds size %u", i, offset, length, size);
      return false;
    }
  }
  return true;
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
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // D4: backend tag ("FTU1"). FreeType's advances/kerning differ from stb's
  // (different hinting), so a stale stb-layout section cache must invalidate.
  // Folding the tag into the byte-hash touches only TTF-family caches; the
  // format is unchanged → no SECTION_FILE_VERSION bump.
  h ^= 0x46545531u;
  // Render-affecting options (hinting) participate in cache identity —
  // see kRenderOptions / renderOptionsFingerprintTag().
  h ^= renderOptionsFingerprintTag();
#endif
  return h;
}

uint32_t BookFontLoader::computeFingerprintCached() {
  // Same content semantics as computeFingerprint(), but each slot's chained
  // byte-walk is served from (and refreshed into) the SD cache keyed by the
  // face's path hash. The incoming chain seed is part of every cache record,
  // so a slot can never be served for a different predecessor chain (e.g.
  // after slot 0's file was replaced). Faces without an SD mtime bypass the
  // cache entirely — readFingerprintCache fails closed for them.
  bool anyLoaded = false;
  uint32_t h = 0x811c9dc5;
  for (uint8_t i = 0; i < 4; ++i) {
    if (fontBytes_[i] && fontFileSizes_[i] > 0) {
      anyLoaded = true;
      const uint32_t inSeed = h;
      const auto* bytes = static_cast<const uint8_t*>(fontBytes_[i]);
      // Head hash: the cheap identity check that runs on every hit (~4 KB,
      // ~2% of a full walk). FNV-1a folds sequentially, so the miss path
      // chains the head walk straight into the full hash — no re-walk.
      const size_t headLen = fontFileSizes_[i] < kFingerprintHeadBytes ? fontFileSizes_[i] : kFingerprintHeadBytes;
      const uint32_t headHash = fontFNV1a(bytes, headLen, inSeed);
      uint32_t slotHash = 0;
      if (!BookFontLoader_readFingerprintCache(facePathHash_[i], fontFileSizes_[i], faceMtime_[i], inSeed, headHash,
                                               slotHash)) {
        slotHash = fontFNV1a(bytes + headLen, fontFileSizes_[i] - headLen, headHash);
        BookFontLoader_writeFingerprintCache(facePathHash_[i], fontFileSizes_[i], faceMtime_[i], inSeed, headHash,
                                             slotHash);
      }
      h = slotHash;
    }
  }
  if (!anyLoaded) return 0;
  h ^= static_cast<uint32_t>(chain_.styleCoverage());
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  h ^= 0x46545531u;  // backend tag, mirrors computeFingerprint()
  h ^= renderOptionsFingerprintTag();
#endif
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
  const int familiesBefore = familyCount;
  if (!root || !root.isDirectory()) {
    // Missing roots are normal (a card may use only one of the two roots),
    // but the owner-visible boot log must distinguish this from a family cap.
    LOG_INF("BFNT", "Font root unavailable: %s", rootPath);
    return;
  }

  // The walk frame would need over 1KB of stack locals (over the 256B stack
  // budget, and scanFonts runs from boot wiring) — one heap scratch instead.
  struct ScanScratch {
    char dirName[64];                            // FamilyInfo::name cap; longer folder names are skipped
    char fileName[FontFaceInfo::kFileCap];       // long vendor filenames fit untruncated
    char lower[64];                              // lowercased stem
    char subPath[160];                           // SdCardCacheStorage::kDirMax
    char newFile[FontFaceInfo::kFileCap];        // full-path validation scratch
    char soloFile[FontFaceInfo::kFileCap];       // §14.4 rule 7: the lone candidate's name
    char soloLower[64];                          // the lone candidate's stem — `lower` is stale by promotion time
    char tokenlessFile[FontFaceInfo::kFileCap];  // first no-token candidate
    char tokenlessLower[64];                     // its stem
    FamilyInfo fam;                              // ~940B manifest row — heap, reset per family
    uint32_t soloSize = 0;
    uint32_t tokenlessSize = 0;
    uint32_t soloMtime = 0;
    uint32_t tokenlessMtime = 0;
  };
  // sizeof() on the decayed pointers would measure the pointer, not the
  // buffer — the walk uses the struct's member sizes everywhere.
  // >1 KB of transient scan state: try PSRAM first, with an explicit DRAM
  // fallback for exhausted/no-PSRAM hosts.
  PoolBytes scratchPool = poolMakeBytes(sizeof(ScanScratch));
  std::unique_ptr<ScanScratch> scratchDram;
  if (!scratchPool) scratchDram = makeUniqueNoThrow<ScanScratch>();
  if (!scratchPool && !scratchDram) {
    LOG_ERR("BFNT", "OOM: scan scratch");
    return;
  }
  ScanScratch* scratch = scratchPool ? new (scratchPool.get()) ScanScratch : scratchDram.get();
  char* dirName = scratch->dirName;
  char* fileName = scratch->fileName;
  char* lower = scratch->lower;
  char* soloLower = scratch->soloLower;
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
    // Hidden-root family with the same name: MERGE this root's faces into it
    // (§14.4). A new face only fills an unused style slot; for a style both
    // roots provide, the already-stored hidden-root face wins outright.
    uint8_t existingIndex = kMaxDiscoveredFamilies;
    for (uint8_t i = 0; i < familyCount; ++i) {
      if (ciCompare(families[i].name, dirName) == 0) {
        existingIndex = i;
        break;
      }
    }
    const bool mergingExisting = existingIndex < kMaxDiscoveredFamilies;
    if (!mergingExisting && familyCount >= kMaxDiscoveredFamilies) {
      LOG_INF("BFNT", "Family cap reached (%u): skipping %s", static_cast<unsigned>(kMaxDiscoveredFamilies), dirName);
      continue;
    }
    if (nameLen >= kDirNameCap - 1 || nameLen >= sizeof(FamilyInfo::name)) {
      LOG_DBG("BFNT", "Family name too long: %s", dirName);
      continue;
    }

    FamilyInfo& fam = mergingExisting ? families[existingIndex] : scratch->fam;
    if (!mergingExisting) {
      fam = {};
      strncpy(fam.name, dirName, sizeof(fam.name) - 1);
    }

    const int subLen = snprintf(subPath, kSubPathCap, "%s/%s", rootPath, dirName);
    if (subLen < 0 || static_cast<size_t>(subLen) >= kSubPathCap) continue;

    HalFile subdir = Storage.open(subPath);
    if (!subdir || !subdir.isDirectory()) continue;

    // Extension-accepted candidates (§14.4 rule 7: a family folder with
    // exactly one .ttf/.otf registers it as Regular even without style
    // tokens in the name).
    char* const soloFile = scratch->soloFile;
    char* const tokenlessFile = scratch->tokenlessFile;
    char* const tokenlessLower = scratch->tokenlessLower;
    uint32_t& soloSize = scratch->soloSize;
    uint32_t& tokenlessSize = scratch->tokenlessSize;
    uint32_t& soloMtime = scratch->soloMtime;
    uint32_t& tokenlessMtime = scratch->tokenlessMtime;
    // These candidates persist in the shared scratch across the outer family
    // loop; reset them so one family's Regular candidates cannot leak into
    // the next family's post-loop resolution.
    soloFile[0] = '\0';
    soloLower[0] = '\0';
    soloSize = 0;
    soloMtime = 0;
    tokenlessFile[0] = '\0';
    tokenlessLower[0] = '\0';
    tokenlessSize = 0;
    tokenlessMtime = 0;
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
        soloMtime = entry.modificationTime();
        soloLower[0] = '\0';
      }

      // Stem for style inference (extension stripped, lowercased).
      const size_t stemLen = nameLen - 4;
      if (stemLen == 0 || stemLen >= kLowerCap) continue;
      for (size_t i = 0; i < stemLen; ++i) {
        lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(fileName[i])));
      }
      lower[stemLen] = '\0';
      if (candidateCount == 1) snprintf(soloLower, kLowerCap, "%s", lower);

      uint8_t style = 0;
      if (!inferStyleFlags(lower, style)) {
        // Do not drop it: remember the lexicographically-first no-token file
        // as the family's Regular candidate (fixes "Bookerly Display" and
        // tokenless Amazon Ember files being skipped, which previously made
        // Bold the promoted Regular).
        if (tokenlessLower[0] == '\0' || ciCompare(lower, tokenlessLower) < 0) {
          snprintf(tokenlessFile, kFileNameCap, "%s", fileName);
          snprintf(tokenlessLower, kLowerCap, "%s", lower);
          tokenlessSize = entry.fileSize();
          tokenlessMtime = entry.modificationTime();
        }
        continue;
      }

      // Same-style duplicate: within one root the lexicographically-first
      // filename wins; when merging from the second root the stored
      // (hidden-root) face always wins.
      uint8_t slot = kMaxFacesPerFamily;
      for (uint8_t i = 0; i < fam.faceCount; ++i) {
        if (fam.faces[i].styleFlags == style) {
          slot = i;
          break;
        }
      }
      const bool replacingExisting = slot < kMaxFacesPerFamily;
      if (replacingExisting) {
        if (mergingExisting || ciCompare(lower, fam.faces[slot].name) >= 0) continue;
      } else {
        if (fam.faceCount >= kMaxFacesPerFamily) continue;
      }
      // Validate the full path BEFORE touching the slot: a too-long path must
      // not clobber an existing face (or shrink the count of one).
      char* const newFile = scratch->newFile;
      if (snprintf(newFile, kFileNameCap, "%s/%s", subPath, fileName) >= static_cast<int>(FontFaceInfo::kFileCap)) {
        LOG_INF("BFNT", "Path too long for %s/%s", fam.name, fileName);
        continue;
      }
      if (!replacingExisting) slot = fam.faceCount++;

      FontFaceInfo& face = fam.faces[slot];
      face = {};
      snprintf(face.name, sizeof(face.name), "%s", lower);
      snprintf(face.file, sizeof(face.file), "%s", newFile);
      face.styleFlags = style;
      face.fileSize = entry.fileSize();
      face.mtime = entry.modificationTime();
    }

    if (fam.faceCount == 0) {
      if (candidateCount == 1) {
        // Single-file family: register the lone face as Regular (§14.4).
        FontFaceInfo& face = fam.faces[0];
        fam.faceCount = 1;
        face = {};
        snprintf(face.name, sizeof(face.name), "%s", soloLower);
        if (snprintf(face.file, sizeof(face.file), "%s/%s", subPath, soloFile) >= static_cast<int>(sizeof(face.file))) {
          fam.faceCount = 0;
          continue;
        }
        face.fileSize = soloSize;
        face.mtime = soloMtime;
        face.styleFlags = StyleNone;
      } else if (tokenlessLower[0] != '\0') {
        // Multiple no-token candidates: lexicographically-first becomes Regular.
        FontFaceInfo& face = fam.faces[0];
        fam.faceCount = 1;
        face = {};
        snprintf(face.name, sizeof(face.name), "%s", tokenlessLower);
        if (snprintf(face.file, sizeof(face.file), "%s/%s", subPath, tokenlessFile) >=
            static_cast<int>(sizeof(face.file))) {
          fam.faceCount = 0;
          continue;
        }
        face.fileSize = tokenlessSize;
        face.mtime = tokenlessMtime;
        face.styleFlags = StyleNone;
      } else {
        continue;  // empty / unparseable family
      }
    } else {
      bool hasRegular = false;
      for (uint8_t i = 0; i < fam.faceCount; ++i) {
        if (fam.faces[i].styleFlags == StyleNone) {
          hasRegular = true;
          break;
        }
      }
      if (!hasRegular && tokenlessLower[0] != '\0') {
        // Explicit Regular wins if present. Otherwise add the remembered
        // no-token candidate instead of promoting Bold to Regular.
        uint8_t slot = kMaxFacesPerFamily;
        for (uint8_t i = 0; i < fam.faceCount; ++i) {
          if (fam.faces[i].styleFlags == StyleNone) {
            slot = i;
            break;
          }
        }
        if (slot == kMaxFacesPerFamily && fam.faceCount < kMaxFacesPerFamily) slot = fam.faceCount++;
        if (slot < kMaxFacesPerFamily) {
          FontFaceInfo& face = fam.faces[slot];
          face = {};
          snprintf(face.name, sizeof(face.name), "%s", tokenlessLower);
          if (snprintf(face.file, sizeof(face.file), "%s/%s", subPath, tokenlessFile) >=
              static_cast<int>(sizeof(face.file))) {
            if (slot == fam.faceCount - 1) --fam.faceCount;
            continue;
          }
          face.fileSize = tokenlessSize;
          face.mtime = tokenlessMtime;
          face.styleFlags = StyleNone;
        }
      } else if (!hasRegular) {
        // No tokenless candidate either: preserve the legacy promotion of the
        // lexicographically-first face (case-insensitive) to Regular.
        int first = 0;
        for (uint8_t i = 1; i < fam.faceCount; ++i) {
          if (ciCompare(fam.faces[i].name, fam.faces[first].name) < 0) first = i;
        }
        fam.faces[first].styleFlags = StyleNone;
      }
    }

    if (mergingExisting) {
      LOG_DBG("BFNT", "Family %s: %u faces after merge from %s", fam.name, fam.faceCount, rootPath);
    } else {
      families[familyCount++] = fam;
      LOG_DBG("BFNT", "Family %s: %u faces from %s", fam.name, fam.faceCount, rootPath);
    }
  }

  LOG_INF("BFNT", "Font root %s: %d new families (total %d/%u)", rootPath, familyCount - familiesBefore, familyCount,
          static_cast<unsigned>(kMaxDiscoveredFamilies));
}
#endif

// ── tryLoadFace — single face into the live chain ────────────────────────────
// Member of BookFontLoader so it can access private members (faces_,
// fontPsramBytes_, fontDramBytes_, fontBytes_, arenas_, remainingBudget_).

bool BookFontLoader::tryLoadFace(uint8_t faceIdx, const FontFaceInfo& fi, FontChain& chain) {
  // Fingerprint-cache identity (P3.1): path hash is the SD cache key, mtime
  // the rehash trigger beside size. Captured up front; a later failure in
  // this slot leaves the identity set but harmless (no fontBytes_ = the slot
  // is skipped by the fingerprint walk).
  facePathHash_[faceIdx] = fontFNV1a(reinterpret_cast<const uint8_t*>(fi.file), strlen(fi.file));
  faceMtime_[faceIdx] = fi.mtime;

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
  } else if (fi.fileSize > kMaxFaceBytes) {
    LOG_ERR("BFNT", "Font %s too large for PSRAM tier (%u > %u)", fi.file, fi.fileSize,
            static_cast<unsigned>(kMaxFaceBytes));
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
  // Shared with the prefetch worker's face builder (one gate, one behavior).
  if (!validateSfntBytes(static_cast<const uint8_t*>(fontBytes), fi.fileSize)) {
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
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
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // D6: FreeType owns the glyph slot (FontAlloc routes all FT heap to PSRAM
  // when present), so the caller-side glyph arena and its backing pool are
  // skipped entirely on this backend.
  NativeFace* face = new (std::nothrow) NativeFace();
  if (!face) {
    LOG_ERR("BFNT", "FtFont OOM for %s", fi.file);
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }
  // Borrowed bytes: the file bytes outlive the face (same lifetime rules as
  // the stb path — released only in ensureLoaded()/releaseResidentCaches()).
  const bool initOk = face->init(static_cast<const uint8_t*>(fontBytes), fi.fileSize, kInitSizePx,
                                 styleToWeight(fi.styleFlags), (fi.styleFlags & StyleItalic) != 0);
  if (!initOk) {
    LOG_ERR("BFNT", "FtFont::init failed for %s", fi.file);
    delete face;
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }
  // Reader-wide render options (hinting + text render mode) through the
  // single supported/degrade funnel; the effective set lands in
  // effectiveRenderOptions_ either way.
  applySlotRenderOptions(face, faceIdx, effectiveRenderOptions(faceIdx), fi.file);
#else
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

  NativeFace* face = new (std::nothrow) NativeFace();
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
#endif

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
