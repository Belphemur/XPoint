#pragma once

#include <cstring>

// StarDict .ifo sametypesequence classification. Kept free of Arduino/HAL
// includes so host tests pin the plain-vs-styled format decision
// (DictionaryDefinitionRoutingTest).
namespace dict_ifo {

// Does a sametypesequence value declare a markup type the styled layout can
// render? The spec declares h (HTML), x (XDXF), g (pango), k (lingvo), y,
// r (RTF) and w as markup; 'm' is plain text. Only single-type sequences are
// claimed: multi-type sequences have per-type field semantics the single
// plain/styled split cannot honor.
inline bool declaresMarkupType(const char* value) {
  if (value == nullptr || value[0] == '\0' || value[1] != '\0') {
    return false;
  }
  switch (value[0]) {
    case 'h':
    case 'x':
    case 'g':
    case 'k':
    case 'y':
    case 'r':
    case 'w':
      return true;
    default:
      return false;
  }
}

}  // namespace dict_ifo