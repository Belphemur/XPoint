// TtfResolvePolicy — pure resolver policy for the TTF/FIBP path (design §3.2/§3.3).
// Extracted from EpubReaderActivity::ttfResolveTargetPage() so the core policy
// can be unit-tested in isolation with fakes (test/fibp_prefetch_policy/ style).
// ttfResolveTargetPage() orchestrates anchor resolution, spine/generation
// checks, and source-latch clearing, then delegates the three-case policy
// (covered / terminal / pending) to evaluate().

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ttf_resolve {

// Pending target state for the unified resolver funnel (design §3.2).
struct PendingTarget {
  enum Kind : uint8_t { None, CharOffset, AnchorHash, Percent, LastPage } kind;
  uint32_t charOffset;  // CharOffset / AnchorHash-resolved
  uint32_t idHash;      // AnchorHash
  uint8_t  percent;     // Percent
};

// Resolver outcome (one pass).
struct ResolveOutcome {
  bool resolved;      // target was resolved (covered or terminal)
  int  page;          // resolved page index (valid when resolved)
  bool needFullBuild; // full build required (Percent/LastPage kinds, or not yet covered)
};

// Cover check result (§3.3).
struct CoverResult { bool covered; uint32_t page; };

// Cover check (§3.3). Returns {covered, page} or {false, 0} (not covered).
// pageForChar: maps a char offset → page index (returns 0 if no pages built).
// pageCharStart: maps a page index → charStart of that page (0 if out of range).
// available: availablePageCount() (pages built so far).
// haveTotal: session/cache complete.
CoverResult coverCheck(uint32_t target, uint32_t available, bool haveTotal,
                       const std::function<uint32_t(uint32_t charOffset)>& pageForChar,
                       const std::function<uint32_t(uint16_t pageIndex)>& pageCharStart);

// Resolve the pending target against the current build state (§3.2 policy).
// All targets must already be resolved to CharOffset before calling:
//   AnchorHash → caller resolves via charForAnchor first, sets kind=CharOffset,
//                charOffset = result, idHash = 0.
//   CharOffset → active.charOffset holds the chapter char offset target.
//   Percent    → active.percent holds the target percent.
//   LastPage   → active.kind = LastPage (sentinel, active.charOffset unused).
// pageForChar: (charOffset) → page index, returns false if not built.
// pageCharStart: (pageIndex) → charStart (0 if out of range).
// haveTotal: session/cache complete.
// hasSavedPosition [in/out]: consumed (cleared) when target resolved/terminal
//   AND active.charOffset == *this ttfSavedCharOffset (funnel contract).
// offsetJump [in/out]: consumed (reset) when target resolved/terminal AND
//   active.charOffset == *offsetJump (funnel contract).
// pendingAnchor [in/out]: consumed (cleared) when target resolved/terminal
//   AND kind was AnchorHash (funnel contract).
// Returns the outcome for this pass.
ResolveOutcome evaluate(const PendingTarget& target, uint32_t available,
                        const std::function<bool(uint32_t charOffset, uint32_t& pageOut)>& pageForChar,
                        const std::function<uint32_t(uint16_t pageIndex)>& pageCharStart,
                        bool haveTotal,
                        bool& hasSavedPosition,
                        std::optional<uint32_t>& offsetJump,
                        std::string& pendingAnchor);

}  // namespace ttf_resolve
