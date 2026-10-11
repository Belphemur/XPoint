#pragma once

#include <cctype>
#include <cstddef>
#include <string>

// Routing helpers for dictionary definitions whose declared format lies.
// Kept header-only and free of Arduino/render includes so host tests can pin
// the plain-vs-styled decision (DictionaryDefinitionRoutingTest).
namespace dict_html {

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (see DictionaryDefinitionActivity), so bigger definitions take the
// span-based plain-text path regardless of format.
inline constexpr size_t kMaxStyledHtmlBytes = 16 * 1024;

// Content sniff: is this definition shaped like an HTML entry regardless of
// what sametypesequence declared? Some dictionaries (e.g. M-WAWLD,
// sametypesequence=x) ship HTML entries anyway. A leading '<' (after any
// whitespace) whose matching '>' falls within the headword-tag window
// (e.g. "<k>") is treated as HTML.
inline bool looksHtml(const std::string& definition) {
  size_t start = 0;
  while (start < definition.size() && std::isspace(static_cast<unsigned char>(definition[start]))) {
    ++start;
  }
  if (definition.size() - start < 4) {
    return false;
  }
  const size_t gt = definition.find('>', start);
  return definition[start] == '<' && gt != std::string::npos && gt - start <= 64;
}

// Plain-vs-styled routing decision: the size gate comes first (styled layout
// keeps the whole definition resident), then declared-or-sniffed HTML.
inline bool routesToStyled(const std::string& definition, bool ifoHtml) {
  if (definition.size() > kMaxStyledHtmlBytes) {
    return false;
  }
  return ifoHtml || looksHtml(definition);
}

}  // namespace dict_html
