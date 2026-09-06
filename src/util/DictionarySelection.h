#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace DictionarySelection {

struct TokenSpan {
  size_t start = 0;
  size_t length = 0;
};

struct LogicalGroup {
  uint32_t sourceGroup = 0;
  size_t segmentStart = 0;
  size_t segmentCount = 0;
};

struct GroupedTokens {
  std::vector<LogicalGroup> groups;
  std::vector<size_t> segments;
};

// A token is selectable when it contains a letter, digit, or non-punctuation
// non-ASCII codepoint. This keeps standalone punctuation out of word lookup.
bool isSelectableToken(std::string_view text);

// Remove punctuation and soft hyphens only from the two edges. Internal
// punctuation, including explicit hyphens and apostrophes, remains selectable.
TokenSpan trimTokenEdges(std::string_view text);

// Return the logical length of a rendered segment. A synthetic ASCII hyphen
// appended only at a line break is presentation-only and must not enter a
// dictionary query; an explicit hyphen remains part of the word.
size_t logicalSegmentLength(std::string_view text, bool syntheticHyphen);

// Group rendered token indexes by their source-visible word id while preserving
// first-seen group order and the original segment order. Wrapped line pieces
// therefore become one logical selectable word.
GroupedTokens groupTokens(const std::vector<uint32_t>& sourceGroups);

}  // namespace DictionarySelection
