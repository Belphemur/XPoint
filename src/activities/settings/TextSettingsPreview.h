#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class GfxRenderer;
class TextBlock;

namespace textsettings {

// Settings + geometry that determine the laid-out lines; used to invalidate the cache.
struct PreviewKey {
  int fontId = -1;
  int fontPointSize = -1;
  int screenMargin = -1;
  int textWidth = -1;
  float lineCompression = -1.0f;
  uint8_t alignment = 0xFF;
  bool extraParagraphSpacing = false;
  bool focusReading = false;
  bool hyphenation = false;
  bool embeddedStyle = false;  // engine layout input (params.embeddedStyles)
  // Native-TTF preview (design §3.6): the engine chain's identity — engine
  // selection + content fingerprint + the continuous point size. A family
  // swap or file replacement (new fingerprint) re-lays the sample.
  uint32_t engine = 0xFFFFFFFFu;
  uint32_t fingerprint = 0;
  uint32_t ttfPointSize = 0;
  int previewHeight = -1;
  bool operator==(const PreviewKey&) const = default;
};

// One laid-out sample line on the native-TTF path: a copy of the engine run
// (PageTextRun text points into layout scratch and dies with it).
// std::string is intentional: at most kMaxPreviewRuns (24) runs of one pane
// line each (<512 B per run, a few KB total) held only while the settings
// activity is open — a DRAM-sized, short-lived UI payload, not a
// PSRAM-policy buffer.
struct PreviewRun {
  std::string text;
  int16_t x = 0;
  int16_t baselineY = 0;
  uint16_t sizePx = 0;
  uint8_t styleFlags = 0;
};

// Cached engine preview lines + the key that produced them
struct PreviewLayout {
  // Legacy bitmap path (ParsedText lines).
  std::vector<std::shared_ptr<TextBlock>> lines;
  // Native-TTF path: runs laid out by ChapterLayout over the sample source.
  std::vector<PreviewRun> ttfRuns;
  PreviewKey key;
};

// Draws the sample-text pane via the reader engine, reusing layout across redraws
void renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, int previewPadding, int labelGap, int top,
                   int height, const char* familyName, const char* sizeName);

}  // namespace textsettings
