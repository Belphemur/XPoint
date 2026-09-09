// FreeInk SDK — BookFontLoader for native TTF/OTF font support (Phase 1a).
// Scans /fonts/*.ttf|*.otf on SD, builds a family manifest, and owns the
// entire TTF lifecycle: loading, fingerprinting, and chain assembly.
// Two-tier allocation: PSRAM tier (S3) vs C3 DRAM tier. kMaxDramFontBytes
// is derived from ESP.getFreeHeap()/getMaxAllocHeap() measured after all
// arenas are allocated (keep the 32KB/16KB heap-gate floors) — NOT a
// hardcoded 256KB (§3.3). Oversized files: LOG_ERR("BFNT", "Font %s too
// large for DRAM tier (%u > %u)") and stay listed but greyed out.
//
// FontChain assembly (≤8 faces, styleCoverage()).
// fontFingerprint() = FNV-1a over the LOADED font bytes ⊕ styleCoverage
// — content-based, never path/mtime (§3.4).
//
// Builtin fallback: singleton FontChain over BitmapBookFont (4 style
// instances = 16KB static BSS — accounted in C3 budget).
//
// sfnt VALIDATION BOUNDARY before TtfFont::init: table-directory bounds +
// numTables sanity; on failure LOG_ERR + skip the face. TtfFont::init only
// checks len<12 (TtfFont.cpp:36).
//
// Face bytes: loaded through the framebuffer loan (loadFaceBytes), NOT kept
// resident; stb needs bytes addressable only during init(). On the PSRAM
// tier the bytes stay resident (design §3.3); on the DRAM tier they are
// borrowed from the 48KB framebuffer loan during init() and released.
//
// One shared glyph arena backs all chain faces — TtfFont::flushGlyphs
// rewinds to the face's init mark (TtfFont.cpp:133) and can invalidate
// later faces' cached glyphs. Verify alternating styles do not storm
// cross-face invalidation; per-face arenas is the accepted fix if it does.
//
// Canonical font fingerprints enable FIBP cache invalidation. A font file
// replaced on SD (same name, new bytes) produces a different fingerprint
// and correctly invalidates stale caches.
//
// Two-tier allocation at begin():
//   Tier::PsramS3: heap_caps_malloc in MALLOC_CAP_SPIRAM (X4 Pro/X4C/Paper Mono)
//   Tier::DramC3:  makeUniqueNoThrow<uint8_t[]> into DRAM (X4/Sticky, no PSRAM)
//
// Authoritative design: DESIGN_NATIVE_TTF_SUPPORT.md §3.2
#pragma once

#include "render/TtfFont.h"        // defines FontChain, TtfFont (includes BookFont.h)
#include "I18n.h"
#include "Logging.h"
#include "Memory.h"
#include <atomic>
#include <algorithm>

// Bounded, heap-free array for family manifests (replaces std::vector which
// requires bare new under -fno-exceptions). Provides [i] access + push()
// and contiguous begin()/end() iterators. Size is compile-time N.
template <typename T, uint8_t N>
struct StaticArray {
  T data[N]{};
  uint8_t count = 0;

  void push(const T& v) {
    if (count < N) {
      data[count] = v;
      ++count;
    }
  }
  T& operator[](uint8_t i) { return data[i]; }
  const T& operator[](uint8_t i) const { return data[i]; }
  void clear() { count = 0; }
  const T* begin() const { return data; }
  const T* end() const { return data + count; }
};

namespace freeink {
namespace book {

struct FontFaceInfo {
  char name[48];          // family display name (manifest or filename stem)
  char file[64];          // path under /fonts/
  uint8_t styleFlags;     // StyleFlags this file provides
  uint32_t fileSize;      // raw file size
  uint32_t mtime;         // for fingerprinting
};

struct FamilyInfo {
  char name[48];
  uint8_t faceCount;      // up to 4: REGULAR/BOLD/ITALIC/BOLD_ITALIC
  FontFaceInfo faces[4];  // only populated faces valid
  bool isBuiltinFallback; // the BitmapBookFont chain
};

// Tier: PsramS3 or DramC3, decided at begin() based on esp_psram_size()
enum class Tier { DramC3, PsramS3 };

class BookFontLoader {
public:
  static constexpr uint8_t kMaxDiscoveredFamilies = 32;

  void begin();                     // scan /fonts/, load manifest, probe PSRAM
  void ensureLoaded();              // (re)load the active family if settings changed
  // MUST be called before getReaderFont() or layoutGenerationHash()

  book::FontChain* getReaderFont(); // the live reader chain (never null)
  uint32_t fontFingerprint() const; // FNV-1a over loaded font bytes ⊕ chain->styleCoverage()

  const FamilyInfo* families() const { return families_.data; }
  uint8_t familyCount() const { return familyCount_; }

  void markDirty();                 // web upload / SD change (thread-safe, atomic flag)
  void releaseResidentCaches();     // scrub arenas + unload file bytes

private:
  // Bounded storage for discovered families (no std::vector — bare new aborts
  // under -fno-exceptions; see FontFaceInfo for the 4-byte filename key).
  StaticArray<FamilyInfo, kMaxDiscoveredFamilies> families_;
  uint8_t familyCount_ = 0;

  // Per-face byte ownership: TtfFont::init borrows the complete font buffer,
  // which must remain resident while FontChain uses the face.
  // faces_ owns TtfFont instances; faceBytes_ owns the raw file buffers;
  // release destroys TtfFont before freeing its buffer.
  bool loadFaceBytes(const FontFaceInfo& face);
  uint8_t faceBytesOwner_[4];   // 1 = BookFontLoader owns the buffer, 0 = borrowed

  // RAII owner with a no-op deleter (custom type, not a repository alias):
  // std::unique_ptr<uint8_t[], void(*)(const uint8_t*)> faceBytes_[4] with
  // [](const uint8_t*){}, matching the project rule of no bare `new` under
  // -fno-exceptions; buffers freed in the reverse of loadFaceBytes construction order.
  book::TtfFont* faces_[4];     // only active faces constructed; destroyed in onExit
  book::FontChain chain_;
  uint32_t fingerprint_ = 0;
  std::atomic<bool> dirty_{false};

  uint8_t* fontBytes_[4] = {nullptr};     // raw font buffer (owned or borrowed)
  uint32_t fontFileSizes_[4] = {0};       // file sizes for the size gate

  Tier tier_ = Tier::DramC3;

  // Compute the content-based fingerprint from the loaded face bytes and the
  // chain's style coverage. Declared here so ensureLoaded()/fontFingerprint()
  // share one implementation.
  uint32_t computeFingerprint() const;

  // Singleton builtin fallback chain over BitmapBookFont (4 styles).
  book::FontChain* builtinFallback();
};

} // namespace book
} // namespace freeink

// Globally defined in main.cpp, beside sdFontSystem
extern BookFontLoader fontLoader;