#include "TtfResolvePolicy.h"

namespace ttf_resolve {

CoverResult coverCheck(uint32_t target, uint32_t available, bool haveTotal,
                       const std::function<uint32_t(uint32_t charOffset)>& pageForChar,
                       const std::function<uint32_t(uint16_t pageIndex)>& pageCharStart) {
  // pageForChar() returns the last built page with charStart <= target and has
  // NO failure signal (reader-position-and-reindex.md: verify SDK accessor
  // signature). An offset past the watermark silently maps to the LAST BUILT
  // page, so the cover check must compare against the page's own charStart
  // range and the prefix watermark (§3.3).
  // NOTE: pageForChar may return 0 for offsets beyond the watermark (clamped).
  // The caller must verify the result is a valid page (p < available) to
  // distinguish "covered" from "clamped watermark hit".
  if (available == 0) return {false, 0};  // no pages: nothing resolves
  uint32_t p = pageForChar(target);
  if (p == UINT32_MAX) return {false, 0};  // offset not in built prefix: not covered
  // available > 0 guaranteed by the early-return guard above.
  const uint32_t last = available - 1;
  if (haveTotal) {
    // Full index: exact resolution, but only if the target maps to a valid
    // page. target > totalChars means beyond the text — terminal, not covered.
    if (p < available) return {true, p};
    return {false, 0};  // beyond text: terminal (handled as clamp in evaluate)
  }
  if (p < last) return {true, p};  // within prefix: exact
  if (p == last) {
    // pageCharStart takes a uint16_t page index: refuse a page count that
    // would truncate the cast (reads an unrelated page's charStart otherwise).
    if (last <= UINT16_MAX) {
      const uint32_t lastCharStart = pageCharStart(static_cast<uint16_t>(last));
      if (target == lastCharStart) return {true, p};  // exactly on last page
    }
  }
  return {false, 0};  // p == last && target > charStart(last): clamped watermark hit — peek, don't consume
}

ResolveOutcome evaluate(const PendingTarget& target, uint32_t available,
                        const std::function<bool(uint32_t charOffset, uint32_t& pageOut)>& pageForChar,
                        const std::function<uint32_t(uint16_t pageIndex)>& pageCharStart, bool haveTotal) {
  // Percent and LastPage kinds cannot resolve from a partial prefix (§3.2 case 3).
  // They always need a full build (wait for total). Skip cover check entirely.
  if (target.kind == PendingTarget::Percent || target.kind == PendingTarget::LastPage) {
    if (haveTotal) return {true, static_cast<int>(available > 0 ? available - 1 : 0), false};
    return {false, 0, true};  // needFullBuild = true
  }

  // Page kind: the index IS the target — covered as soon as that many pages
  // are built (page-anchored records and the preview seed, §3.2).
  if (target.kind == PendingTarget::Page) {
    if (target.charOffset < available) return {true, static_cast<int>(target.charOffset), false};
    if (haveTotal) return {true, static_cast<int>(available > 0 ? available - 1 : 0), false};
    return {false, 0, false};  // build grows toward the page
  }

  // CharOffset / pre-resolved AnchorHash policy per pass, in priority order (§3.2):
  CoverResult cover = coverCheck(
      target.charOffset, available, haveTotal,
      [&](uint32_t offset) -> uint32_t {
        uint32_t p = 0;
        if (!pageForChar(offset, p)) return UINT32_MAX;  // not built: signal failure
        return p;
      },
      pageCharStart);

  if (cover.covered) return {true, static_cast<int>(cover.page), false};  // 1. Covered

  // 2. Chapter ends before target, terminal: clamp to last page.
  if (haveTotal) return {true, static_cast<int>(available > 0 ? available - 1 : 0), false};

  // 3. Not covered, not terminal: keep pending, one chunk toward it.
  return {false, 0, false};
}

}  // namespace ttf_resolve