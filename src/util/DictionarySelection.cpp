#include "DictionarySelection.h"

#include <Epub/hyphenation/HyphenationCommon.h>
#include <Utf8.h>

#include <cctype>

namespace DictionarySelection {

bool isSelectableToken(const std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      if (std::isalnum(c) != 0) return true;
      ++i;
      continue;
    }

    const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data() + i);
    const uint32_t cp = utf8NextCodepoint(&p);
    const size_t next = static_cast<size_t>(p - reinterpret_cast<const unsigned char*>(text.data()));
    // The shared punctuation helper covers the common ASCII and quote set;
    // General Punctuation is also never a word by itself (for example U+2014
    // EM DASH), so keep the complete U+2000..U+206F block non-selectable.
    if (cp != 0 && !isPunctuation(cp) && (cp < 0x2000 || cp > 0x206F) && cp != 0x00AD) return true;
    i = next > i ? next : i + 1;
  }
  return false;
}

TokenSpan trimTokenEdges(const std::string_view text) {
  const auto isBoundary = [](const uint32_t cp) {
    if (cp < 0x80) return std::isalnum(static_cast<unsigned char>(cp)) == 0;
    return isPunctuation(cp) || (cp >= 0x2000 && cp <= 0x206F) || cp == 0x00AD;
  };

  size_t start = 0;
  size_t end = text.size();
  while (start < end) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data() + start);
    const uint32_t cp = utf8NextCodepoint(&p);
    const size_t next = static_cast<size_t>(p - reinterpret_cast<const unsigned char*>(text.data()));
    if (!isBoundary(cp)) break;
    start = next > start ? next : start + 1;
  }

  while (end > start) {
    const unsigned char* base = reinterpret_cast<const unsigned char*>(text.data());
    const unsigned char* p = base + end;
    const unsigned char* previous = p;
    do {
      --previous;
    } while (previous > base + start && (*previous & 0xC0) == 0x80);
    const uint32_t cp = utf8NextCodepoint(&previous);
    const size_t nextEnd = static_cast<size_t>(previous - base);
    if (!isBoundary(cp)) break;
    end = nextEnd < end ? nextEnd : end - 1;
  }
  return {start, end - start};
}

size_t logicalSegmentLength(const std::string_view text, const bool syntheticHyphen) {
  if (syntheticHyphen && !text.empty() && text.back() == '-') return text.size() - 1;
  return text.size();
}

GroupedTokens groupTokens(const std::vector<uint32_t>& sourceGroups) {
  GroupedTokens result;
  result.groups.reserve(sourceGroups.size());
  result.segments.reserve(sourceGroups.size());

  for (size_t token = 0; token < sourceGroups.size(); token++) {
    size_t groupIndex = 0;
    while (groupIndex < result.groups.size() && result.groups[groupIndex].sourceGroup != sourceGroups[token]) {
      groupIndex++;
    }
    if (groupIndex == result.groups.size()) {
      result.groups.push_back({sourceGroups[token], result.segments.size(), 0});
    }
    (void)groupIndex;
  }

  // Keep each group's segment indexes contiguous so callers can walk one
  // logical word with [segmentStart, segmentStart + segmentCount).
  for (auto& group : result.groups) {
    group.segmentStart = result.segments.size();
    for (size_t token = 0; token < sourceGroups.size(); token++) {
      if (sourceGroups[token] == group.sourceGroup) {
        result.segments.push_back(token);
        group.segmentCount++;
      }
    }
  }
  return result;
}

}  // namespace DictionarySelection
