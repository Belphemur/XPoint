#pragma once

#include <Print.h>
#include <ZipFile.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  CssParser::ParseResult parseCssFiles(CssParser::CacheStatus existingCacheStatus) const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool clearCache() const;

  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize,
                                bool allowEarlyStop = false) const;
  // Extract an item to a file on SD. On failure the partial file is removed.
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;

#ifdef BOARD_HAS_PSRAM
  // ZipFileCache: parse the ZIP central directory once and cache FileStatSlim
  // entries in PSRAM, avoiding repeated central-directory scans on every
  // getItemSize() / readItemContentsToStream() call.
  class ZipFileCache {
    std::unordered_map<std::string, ZipFile::FileStatSlim> cache_;
    bool loaded_ = false;

   public:
    // Note: takes const std::string& (not const char*) because ZipFile stores
    // its filePath as a const std::string& member. A const char* here would force
    // a temporary std::string at the call site that dies at the end of the
    // full-expression, leaving ZipFile::filePath as a dangling reference.
    void load(const std::string& epubPath);
    // take std::string_view (no allocation) and compare to the map key directly.
    // The callers already have a normalized std::string in hand; passing it as a
    // view avoids the redundant FsHelpers::normalisePath() + std::string
    // allocation that was happening on every cache lookup (CodeRabbit IPV8;
    // AGENTS.md rule 4 prohibits std::string in hot paths).
    const ZipFile::FileStatSlim* get(std::string_view itemPath) const;
    // Drop one cached entry so the next read re-scans the ZIP central
    // directory. Used when a cached read fails (rare: shifted
    // localHeaderOffset, mid-stream inflate error). Does not invalidate
    // the whole cache — only the failing entry.
    void invalidate(std::string_view itemPath);
    // True if load() succeeded and the cache is populated. Callers must check
    // this before using the cache; a failed load leaves zipCache_ non-null but
    // empty, and a caller that iterates an empty cache (e.g. discoverCssFilesFromZip)
    // would skip the ZipFile fallback path and miss CSS files (CodeRabbit IPV2).
    bool isLoaded() const { return loaded_; }
    // Iterate the cache (PSRAM-backed on BOARD_HAS_PSRAM boards). Used by
    // discoverCssFilesFromZip to avoid opening a fresh ZipFile just to enumerate
    // paths. Returns a span-like pair (begin, end) of (path, FileStatSlim) entries.
    const std::unordered_map<std::string, ZipFile::FileStatSlim>& cacheRef() const { return cache_; }
#ifdef BOARD_HAS_PSRAM
    // PSRAM deleter for the cache object itself: calls dtor + heap_caps_free
    // to match the placement cap used by the heap_caps_malloc in load().
    // CodeRabbit IPVX: default_delete would call `delete` on PSRAM memory,
    // which works in practice (ESP-IDF routes operator delete to
    // heap_caps_free) but is not type-safe.
    static void deleteSelfPsram(ZipFileCache* p);
#endif
  };
#ifdef BOARD_HAS_PSRAM
  // Function-pointer deleter for the unique_ptr<ZipFileCache> below. Using a
  // function pointer (not a lambda) keeps the unique_ptr type uniform across
  // boards and avoids the deleter-template-instantiation ambiguity that
  // blocked D.1 / D.4 (CodeRabbit IPVX, IPV-).
  using ZipFileCacheDeleter = void (*)(ZipFileCache*);
  static constexpr ZipFileCacheDeleter kZipFileCacheDeleter = &ZipFileCache::deleteSelfPsram;
  std::unique_ptr<ZipFileCache, ZipFileCacheDeleter> zipCache_{static_cast<ZipFileCache*>(nullptr),
                                                               kZipFileCacheDeleter};
#else
  std::unique_ptr<ZipFileCache> zipCache_;
#endif
  // PSRAM placement: ZipFileCache is a leaf type (no nested unique_ptr), so
  // the static_assert issue that blocked D.1 / D.4 does not apply. On X4 Pro
  // this places the map (typically 64-150 KB for a 200-entry novel) in PSRAM
  // via a custom function-pointer deleter that calls heap_caps_free (matches
  // the placement cap). On non-PSRAM boards the deleter collapses to plain
  // free() (heap_caps_free handles cross-heap too, but the explicit branch
  // keeps cppcheck happy).
#endif
};
