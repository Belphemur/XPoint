#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <string_view>

class BookMetadataCache;

class Txt {
 public:
  // The single spine item a TXT/MD cache exposes to the EPUB pipeline.
  static constexpr const char* CONTENT_HREF = "content.html";

  static bool isTxtOrMd(std::string_view path);
  static bool validateCache(const std::string& filepath, const std::string& cachePath, size_t cachedSize);
  static void invalidateCache(const std::string& cachePath);
  // Head+tail FNV-1a over the source (8 KB max) for same-size edit detection.
  static bool sourceFingerprint(const std::string& filepath, uint32_t& outFp);
  // allowEarlyStop: a short write from the sink means it has enough (probe paths).
  static bool streamTxtToHtml(const std::string& filepath, Print& out, bool allowEarlyStop = false);
  static std::string findCompanionCoverImage(const std::string& filepath);
  static bool convertCoverImageToBmp(const std::string& imagePath, const std::string& destBmpPath, int thumbHeight = 0,
                                     bool cropped = false, bool originalThresholds = false);
  static bool buildTxtCache(const std::string& filepath, const std::string& cachePath,
                            std::unique_ptr<BookMetadataCache>& bookMetadataCache);
};
