// TtfWordSelect — see TtfWordSelect.h. Gated to CROSSPOINT_TTF_READER: engine
// pages only exist on TTF builds (§14.1 compile-time split).

#if defined(CROSSPOINT_TTF_READER)

#include "TtfWordSelect.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "util/DictionarySelection.h"

namespace freeink {
namespace book {
namespace {

// Byte-level whitespace test — UTF-8 continuation and lead bytes never
// collide with ASCII whitespace, same argument as the layout's isWsByte.
inline bool isWsByte(const char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

// Decodes the UTF-8 codepoint at text[i..to), advancing i past its bytes.
inline uint32_t decodeCp(const char* text, const uint16_t to, uint16_t& i) {
  if (i >= to) return 0;  // defensive: callers loop on i < to
  const auto b0 = static_cast<uint8_t>(text[i]);
  uint32_t cp = b0;
  uint32_t extra = 0;
  if (b0 >= 0xF0) {
    cp = b0 & 0x07;
    extra = 3;
  } else if (b0 >= 0xE0) {
    cp = b0 & 0x0F;
    extra = 2;
  } else if (b0 >= 0xC0) {
    cp = b0 & 0x1F;
    extra = 1;
  }
  ++i;
  while (extra > 0 && i < to && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) {
    cp = (cp << 6) | (static_cast<uint8_t>(text[i]) & 0x3F);
    ++i;
    --extra;
  }
  return cp;
}

}  // namespace

bool buildTtfWordSelectData(const Page& page, FontChain& fonts, TtfWordSelectData& out) {
  out.boxes.clear();
  out.footnotes.clear();

  // Pass 1: token boundaries (byte ranges per run) so the arena can be
  // allocated once.
  struct TokenRange {
    uint16_t run;
    uint16_t start;
    uint16_t end;
  };
  std::vector<TokenRange> tokens;
  size_t arenaBytes = 0;
  for (uint16_t r = 0; r < page.runCount; ++r) {
    const PageTextRun& run = page.runs[r];
    uint16_t i = 0;
    while (i < run.len) {
      while (i < run.len && isWsByte(run.text[i])) ++i;
      if (i >= run.len) break;
      const uint16_t start = i;
      while (i < run.len && !isWsByte(run.text[i])) ++i;
      tokens.push_back(TokenRange{r, start, i});
      arenaBytes += static_cast<size_t>(i - start) + 1;  // NUL-terminated
    }
  }
  if (tokens.empty()) return true;

  // Pass 2: copy text, measure, and group. A logical word's group id is the
  // index of its first fragment — a fragment continuing a previous run's
  // word reuses the carried group, which is exactly the pairing
  // DictionarySelection::groupTokens joins on.
  auto arena = makeUniqueNoThrow<char[]>(arenaBytes);
  if (!arena) {
    LOG_ERR("TTFWS", "OOM: token arena %u bytes", static_cast<unsigned>(arenaBytes));
    return false;
  }
  out.boxes.reserve(tokens.size());
  char* write = arena.get();

  // Width of a byte range inside a run: plain advance walk with kerning, the
  // same convention PagePaint uses when it draws the run.
  auto measure = [&fonts](const char* text, uint16_t from, uint16_t to, uint16_t sizePx, uint8_t styleFlags) {
    int32_t w = 0;
    uint32_t prev = 0;
    for (uint16_t i = from; i < to;) {
      const uint32_t cp = decodeCp(text, to, i);
      if (prev != 0) w += fonts.kerning(prev, cp, sizePx, styleFlags);
      w += fonts.advance(cp, sizePx, styleFlags);
      prev = cp;
    }
    return w;
  };

  bool carry = false;  // previous run's last token ends mid-word
  uint32_t carriedGroup = 0;
  // Rolling pen position: each inter-token gap is measured once. Measuring
  // from the run start for every token would be O(runLen·tokens) font
  // metric work (kody, PR #113).
  int32_t penX = 0;
  uint16_t measuredTo = 0;
  for (size_t t = 0; t < tokens.size(); ++t) {
    const TokenRange& token = tokens[t];
    const PageTextRun& run = page.runs[token.run];
    const char* text = run.text + token.start;
    const uint16_t rawLen = token.end - token.start;
    const std::string_view rawView(text, rawLen);

    const bool runFirst = t == 0 || tokens[t - 1].run != token.run;
    const bool runLast = t + 1 == tokens.size() || tokens[t + 1].run != token.run;
    const bool headFlag = runFirst && (run.layoutFlags & PageTextRun::LayoutFirstContinues) != 0;
    const bool tailFlag = runLast && token.end == run.len && (run.layoutFlags & PageTextRun::LayoutLastContinues) != 0;

    // Advance the rolling pen to this token's start (tokens are ordered by
    // run, then start; a run switch resets the pen). Skipped tokens keep the
    // pen correct for the next one — the next gap measure starts here.
    if (runFirst) {
      penX = run.x;
      measuredTo = 0;
    }
    penX += measure(run.text, measuredTo, token.start, run.sizePx, run.styleFlags);
    measuredTo = token.start;

    // Selection group: a continuation head joins the carried group; any
    // other token starts a fresh logical word. A pairing mismatch (head
    // without carry, carry without head — neither should happen) leaves the
    // fragment standing alone.
    uint32_t group = static_cast<uint32_t>(t);
    if (carry && headFlag) {
      group = carriedGroup;
    }
    carry = tailFlag;
    carriedGroup = group;

    // Trim after the group decision: the box stores the raw token; the
    // trimmed range is what gets looked up and measured.
    const DictionarySelection::TokenSpan span = DictionarySelection::trimTokenEdges(rawView);
    if (span.length == 0 || !DictionarySelection::isSelectableToken(rawView)) continue;

    TtfWordBox box{};
    box.text = write;
    memcpy(write, rawView.data(), rawView.size());
    write[rawView.size()] = '\0';
    write += rawView.size() + 1;
    box.x = static_cast<int16_t>(penX);
    box.y = static_cast<int16_t>(run.baselineY - fonts.ascent(run.sizePx));
    box.height = fonts.lineHeight(run.sizePx);
    box.width = static_cast<int16_t>(measure(text, span.start, span.start + span.length, run.sizePx, run.styleFlags));
    box.textOffset = static_cast<uint16_t>(span.start);
    box.textLength = static_cast<uint16_t>(span.length);
    box.rawLength = rawLen;
    box.styleFlags = run.styleFlags;
    box.selectionGroup = group;
    // A synthetic hyphen is the baked '-' that terminates a hyphenated run:
    // presentation-only, stripped from the dictionary query.
    box.syntheticHyphen = (run.layoutFlags & PageTextRun::LayoutHyphenated) != 0 && runLast && token.end == run.len &&
                          rawLen > 0 && text[rawLen - 1] == '-';
    out.boxes.push_back(box);
  }

  out.arena = std::move(arena);
  return true;
}

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER
