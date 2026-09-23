// TtfUiFallback implementation — see TtfUiFallback.h for the design contract.

#include "TtfUiFallback.h"

#if CROSSPOINT_TTF_UI_FALLBACK

#include <GfxRenderer.h>
#include <HalMemory.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"

namespace freeink {
namespace book {
namespace {

// PSRAM floor for funding the feature: three instances (lazy faces + 16-slot
// 1bpp rings each). Faces allocate PSRAM-first via FontAlloc; below this
// largest-block the allocation cascade would evict reader working set.
constexpr size_t kMinPsramLargestBlock = 256u * 1024u;

// One representative codepoint per script the built-in UI fonts may lack
// (same probe set as the SD-font UI fallbacks): Han, Hiragana, Katakana,
// Hangul, Greek, Cyrillic, Hebrew, Arabic, Thai, Devanagari.
constexpr uint32_t kFallbackProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00, 0x03B1,
                                        0x0430, 0x05D0, 0x0627, 0x0E01, 0x0905};

bool familyCoversFallbackScripts(FontChain& chain) {
  for (const uint32_t cp : kFallbackProbes) {
    if (chain.covers(cp)) return true;
  }
  return false;
}

}  // namespace

void TtfUiFallback::update(GfxRenderer& renderer) {
  // Engine gate: TTF UI fallback only exists on the native-TTF path with a
  // selected family. The built-in bitmap fonts stay the UI font otherwise.
  if (SETTINGS.readerFontEngine != CrossPointSettings::READER_ENGINE_TTF || SETTINGS.ttfFontFamilyName[0] == '\0') {
    release(renderer);
    return;
  }
  fontLoader.ensureLoaded();
  const uint32_t fingerprint = fontLoader.fontFingerprint();
  if (fingerprint == 0) {
    release(renderer);  // fallback chain active — nothing TTF to lend
    return;
  }
  if (registeredCount_ > 0 && registeredFingerprint_ == fingerprint) {
    return;  // fast path: registrations still borrow the same loaded bytes
  }
  release(renderer);

  // Heap gate: FreeType faces + glyph rings need PSRAM headroom.
  const HalMemory::HeapStats psram = HalMemory::getPsramHeap();
  if (psram.totalBytes == 0 || psram.largestBlockBytes < kMinPsramLargestBlock) {
    LOG_DBG("TTFUI", "PSRAM too tight for UI fallback (largest %zu)", psram.largestBlockBytes);
    return;
  }

  // Script gate: probe against the LOADED reader chain (no new faces) — a
  // family whose coverage matches the built-ins can never act as a fallback
  // and its UI faces would be dead weight.
  FontChain* chain = fontLoader.getReaderFont();
  if (chain == nullptr || !familyCoversFallbackScripts(*chain)) {
    LOG_DBG("TTFUI", "Family '%s' has no fallback-script coverage", SETTINGS.ttfFontFamilyName);
    return;
  }

  for (size_t i = 0; i < kUiSizeCount; ++i) {
    const UiFontSize& ui = kUiFontSizes[i];
    const int ttfId = computeTtfUiFontId(SETTINGS.ttfFontFamilyName, ui.pointSize);
    if (renderer.getFontMap().count(ttfId) != 0) {
      LOG_ERR("TTFUI", "Font id %d collision — skipping UI size %u", ttfId, static_cast<unsigned>(ui.pointSize));
      continue;
    }
    const void* bytes[4] = {};
    uint32_t sizes[4] = {};
    bool any = false;
    for (uint8_t s = 0; s < 4; ++s) {
      bytes[s] = fontLoader.slotFaceBytes(s);
      sizes[s] = fontLoader.slotFaceByteSize(s);
      any = any || bytes[s] != nullptr;
    }
    if (!any) {
      LOG_DBG("TTFUI", "Family '%s' has no resident bytes (streamed?) — no UI fallback", SETTINGS.ttfFontFamilyName);
      return;
    }
    if (!instances_[i].begin(bytes, sizes, ui.pointSize)) {
      LOG_DBG("TTFUI", "UI size %u not available in '%s'", static_cast<unsigned>(ui.pointSize),
              SETTINGS.ttfFontFamilyName);
      continue;
    }
    renderer.insertFont(ttfId, instances_[i].family());
    renderer.setFallbackFont(ui.fontId, ttfId);
    registeredIds_[registeredCount_] = ttfId;
    primaryIds_[registeredCount_] = ui.fontId;
    ++registeredCount_;
    LOG_DBG("TTFUI", "UI fallback %s @%upt -> id %d", SETTINGS.ttfFontFamilyName,
            static_cast<unsigned>(ui.pointSize), ttfId);
  }
  registeredFingerprint_ = fingerprint;
  LOG_INF("TTFUI", "%u TTF UI fallback size(s) registered", static_cast<unsigned>(registeredCount_));
}

void TtfUiFallback::release(GfxRenderer& renderer) {
  for (uint8_t i = 0; i < registeredCount_; ++i) {
    renderer.clearFallbackFont(primaryIds_[i]);
    renderer.removeFont(registeredIds_[i]);
  }
  // Faces release AFTER the font map entries are gone — no draw path can
  // reach the borrowed views while they are torn down.
  for (uint8_t i = 0; i < registeredCount_; ++i) {
    instances_[i].end();
  }
  if (registeredCount_ > 0) {
    LOG_DBG("TTFUI", "Released %u TTF UI fallback size(s)", static_cast<unsigned>(registeredCount_));
  }
  registeredCount_ = 0;
  registeredFingerprint_ = 0;
}

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_UI_FALLBACK