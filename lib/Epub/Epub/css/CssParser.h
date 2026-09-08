#pragma once

#include <HalStorage.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "CssStyle.h"

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Grouped: selector1, selector2 { }
 *
 * Not supported (silently ignored):
 *   - Descendant/child selectors
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 */
class CssParser {
 public:
  enum class ParseResult : uint8_t {
    Complete,
    Partial,
    Error,
  };

  enum class CacheStatus : uint8_t {
    Missing,
    Complete,
    Partial,
    Invalid,
  };

  enum class CacheLoadResult : uint8_t {
    Complete,
    LowMemory,
    Invalid,
  };

  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  static constexpr uint8_t CSS_CACHE_VERSION = 11;

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() = default;

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return Complete unless bounded storage stopped rule growth or the source was invalid
   */
  ParseResult loadFromStream(HalFile& source);

  /**
   * Load and parse CSS from an in-memory buffer.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param data CSS text (not copied; must stay valid for the duration of the call)
   * @param len Length of the CSS text in bytes
   * @return Complete unless bounded storage stopped rule growth or the source was invalid
   */
  ParseResult loadFromMemory(const char* data, size_t len);

  /**
   * Look up the style for an HTML element, considering tag name and class attributes.
   * Applies CSS cascade: element style < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr) const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(std::string_view styleValue);

  /**
   * Check if any rules have been loaded
   */
  [[nodiscard]] bool empty() const { return entryCount_ == 0; }

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const { return entryCount_; }

  /**
   * Clear all loaded rules
   */
  void clear() {
    entries_.reset();
    selectorPool_.reset();
    stylePool_.reset();
    entryCount_ = entryCapacity_ = 0;
    selectorPoolSize_ = selectorPoolCapacity_ = 0;
    styleCount_ = styleCapacity_ = 0;
    ruleGrowthStopped_ = false;
  }

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /** Read the cache header without hydrating its rule map. */
  CacheStatus inspectCache() const;

  /**
   * Delete CSS rules cache file exists
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if cache was written successfully
   */
  bool saveToCache(bool complete) const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return Complete when loaded, LowMemory when it should be retried, otherwise Invalid
   */
  CacheLoadResult loadFromCache();

 private:
  enum class RuleInsertResult : uint8_t {
    Inserted,
    Merged,
    Limit,
    OutOfMemory,
  };

  enum class PoolResult : uint8_t {
    Ready,
    Limit,
    OutOfMemory,
  };

  // Stack-allocated string buffer to avoid heap reallocations during parsing
  // Provides string-like interface with fixed capacity
  struct StackBuffer {
    static constexpr size_t CAPACITY = 1024;
    char data[CAPACITY];
    size_t len = 0;

    bool push_back(char c) {
      if (len >= CAPACITY) return false;
      data[len++] = c;
      return true;
    }

    void clear() { len = 0; }
    bool empty() const { return len == 0; }
    size_t size() const { return len; }

    // Get string view of current content (zero-copy)
    std::string_view view() const { return std::string_view(data, len); }
    operator std::string_view() const noexcept { return view(); }
  };

  // Loop-carried state for the CSS character state machine, shared by
  // loadFromStream() and loadFromMemory().
  struct CssParseState {
    StackBuffer selector;
    StackBuffer declBuffer;
    bool inComment = false;
    bool maybeSlash = false;
    bool prevStar = false;
    bool inAtRule = false;
    int atDepth = 0;
    int bodyDepth = 0;
    bool skippingRule = false;
    bool selectorTruncated = false;
    bool declarationTruncated = false;
    bool inputTruncated = false;
    CssStyle currentStyle;
    size_t totalRead = 0;
  };

  struct SelectorEntry {
    uint32_t offset;
    uint16_t styleIndex;
    uint16_t length;
  };
  static_assert(sizeof(SelectorEntry) == 8);

  // Bounded flat storage keeps every growth operation fallible and avoids the
  // throwing node allocations used by std::unordered_map.
  std::unique_ptr<SelectorEntry[]> entries_;
  std::unique_ptr<char[]> selectorPool_;
  std::unique_ptr<CssStyle[]> stylePool_;
  uint16_t entryCount_ = 0;
  uint16_t entryCapacity_ = 0;
  uint32_t selectorPoolSize_ = 0;
  uint32_t selectorPoolCapacity_ = 0;
  uint16_t styleCount_ = 0;
  uint16_t styleCapacity_ = 0;
  bool ruleGrowthStopped_ = false;

  std::string cachePath;

  // Internal parsing helpers
  bool restoreCacheBackupIfNeeded() const;
  void processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style);
  // Character state machine shared by both load entry points
  void handleCssChar(char c, CssParseState& st);
  void processCssChars(const char* data, size_t len, CssParseState& st);
  ParseResult finishCssParse(CssParseState& st);
  [[nodiscard]] int compareEntryToPieces(const SelectorEntry& entry, std::string_view p0, std::string_view p1,
                                         std::string_view p2) const;
  [[nodiscard]] size_t lowerBound(std::string_view p0, std::string_view p1, std::string_view p2, bool& exact) const;
  [[nodiscard]] const CssStyle* findStyle(std::string_view p0, std::string_view p1 = {},
                                          std::string_view p2 = {}) const;
  [[nodiscard]] std::string_view selectorAt(size_t index) const;
  RuleInsertResult insertOrMerge(std::string_view selector, const CssStyle& style);
  PoolResult ensureEntryCapacity(size_t needed);
  PoolResult ensureSelectorPoolCapacity(size_t needed);
  PoolResult ensureStyleCapacity(size_t needed);
  PoolResult internStyle(const CssStyle& style, uint16_t& indexOut);
  static CssStyle parseDeclarations(std::string_view declBlock);
  static void parseDeclarationIntoStyle(std::string_view decl, CssStyle& style);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(std::string_view val);
  static CssFontStyle interpretFontStyle(std::string_view val);
  static CssFontWeight interpretFontWeight(std::string_view val);
  static CssTextDecoration interpretDecoration(std::string_view val);
  static CssLength interpretLength(std::string_view val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(std::string_view val, CssLength& out);
};
