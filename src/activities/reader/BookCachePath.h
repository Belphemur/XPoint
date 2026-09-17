#pragma once

// Shared mapping from a book's card path to its reading-stats cache dir under
// /.crosspoint (see BookReadingStats::load). Empty for paths that have no
// cache dir (non-book files or unknown extensions).
#include <functional>
#include <string>
#include <string_view>

#include <FsHelpers.h>

inline std::string cachePathForBookPath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(std::string_view{path})) {
    return std::string("/.crosspoint/epub_") + std::to_string(std::hash<std::string>{}(path));
  }
  if (FsHelpers::hasXtcExtension(std::string_view{path})) {
    return std::string("/.crosspoint/xtc_") + std::to_string(std::hash<std::string>{}(path));
  }
  return {};
}
