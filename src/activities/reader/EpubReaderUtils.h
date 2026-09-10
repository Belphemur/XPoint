#pragma once

#include <Epub/PageLink.h>
#include <Logging.h>

#include <vector>

#include "../../ProgressManager.h"

namespace EpubReaderUtils {

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
