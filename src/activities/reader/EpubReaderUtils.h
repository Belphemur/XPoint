#pragma once

#include <Epub.h>
#include <Epub/PageLink.h>
#include <Logging.h>

#include <algorithm>
#include <optional>
#include <vector>

#include "../../ProgressManager.h"

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
// Encoding lives in ProgressManager (single owner of the record format);
// this is the Epub-flavored convenience wrapper used by reader call sites.
inline bool saveProgress(const char* cachePath, int spineIndex, int pageNumber, int pageCount,
                         std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  return ProgressManager::saveRecord(cachePath, static_cast<uint16_t>(spineIndex), static_cast<uint16_t>(pageNumber),
                                     static_cast<uint16_t>(pageCount), visibleTextOffset.has_value(),
                                     visibleTextOffset.value_or(0));
}

inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount,
                         std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  return saveProgress(epub.getCachePath().c_str(), spineIndex, pageNumber, pageCount, visibleTextOffset);
}

inline const PageLink* linkAtPoint(const std::vector<PageLink>& links, const int x, const int y, const int marginLeft,
                                   const int marginTop) {
  // Finger slop, plus a floor on the target width: a note marker is often a single superscript
  // digit only a few pixels wide. The box is never grown vertically beyond its own line, so
  // taps on the lines above and below still reach the page-turn zones.
  constexpr int TOUCH_SLOP = 6;
  constexpr int MIN_TOUCH_WIDTH = 28;
  const int pageX = x - marginLeft;
  const int pageY = y - marginTop;
  const auto hit = std::find_if(links.begin(), links.end(), [pageX, pageY](const PageLink& link) {
    return link.contains(pageX, pageY, TOUCH_SLOP, MIN_TOUCH_WIDTH);
  });
  return (hit != links.end()) ? &*hit : nullptr;
}

}  // namespace EpubReaderUtils
