// TtfResolvePolicy — pure resolver policy for the TTF/FIBP path (design §3.2/§3.3).
// Extracted from EpubReaderActivity::ttfResolveTargetPage() so the core policy
// can be unit-tested in isolation with fakes (test/fibp_prefetch_policy/ style).
// ttfResolveTargetPage() orchestrates anchor resolution (SDK charForAnchor),
// applies the source-latch side effects, and delegates the derivation and the
// three-case policy (covered / terminal / pending) to this header.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>

namespace ttf_resolve {

// Pending target state for the unified resolver funnel (design §3.2).
// Single definition: EpubReaderActivity aliases this type, so the two-struct
// static_cast drift is impossible by construction.
struct PendingTarget {
  enum Kind : uint8_t { None, CharOffset, Page, AnchorHash, Percent, LastPage };
  enum Origin : uint8_t { OriginNone, Reflow, Seed, Saved, OffsetJump, Anchor };
  Kind kind = None;
  // CharOffset: chapter char offset. Page: page index (page-anchored records
  // and the reflow/preview seed — a page number is NOT a char offset).
  // AnchorHash: unused until charForAnchor() resolves it to CharOffset.
  uint32_t charOffset = 0;
  uint32_t idHash = 0;         // AnchorHash
  uint8_t percent = 0;         // Percent
  Origin origin = OriginNone;  // which source latch produced the target
};

// Source latches the funnel derives a target from, in priority order (§3.2).
// Pure data: the caller reads its own latches into this and applies the side
// effects (latch clearing) itself — evaluate()/deriveTarget() stay side-effect
// free so the funnel contract is testable.
struct LatchState {
  int32_t reflowSeedPage = -1;    // page-anchored preview/reflow seed
  bool hasSavedPosition = false;  // a saved TTF record exists for this open
  uint16_t savedSpine = 0;
  uint32_t savedCharOffset = 0;  // 0 → page-anchored record (use savedPage)
  uint32_t savedGeneration = 0;
  uint32_t generation = 0;  // current layout generation
  uint16_t currentSpine = 0;
  uint16_t savedPage = 0;  // page number carried by the record
  std::optional<uint32_t> offsetJump;
  bool hasAnchor = false;
  uint32_t anchorHash = 0;
};

// Saved-record degradation (§7): the record cannot map in this chapter.
enum class Degrade : uint8_t { None, ChapterStart };

struct DerivedTarget {
  PendingTarget target;
  Degrade degrade = Degrade::None;
};

// Derive the active pending target from the source latches (§3.2 priority).
// A saved record bound to another spine or a stale generation degrades to the
// chapter start (§7) — the caller clears the latch and opens at page 0.
inline DerivedTarget deriveTarget(const LatchState& s) {
  if (s.reflowSeedPage >= 0)
    return {{PendingTarget::Page, static_cast<uint32_t>(s.reflowSeedPage), 0, 0, PendingTarget::Origin::Seed},
            Degrade::None};
  if (s.hasSavedPosition) {
    if (s.currentSpine != s.savedSpine || s.savedGeneration != s.generation) return {{}, Degrade::ChapterStart};
    if (s.savedCharOffset > 0)
      return {{PendingTarget::CharOffset, s.savedCharOffset, 0, 0, PendingTarget::Origin::Saved}, Degrade::None};
    // Page-anchored record (e.g. KOReader remote-accept): restore by page.
    return {{PendingTarget::Page, s.savedPage, 0, 0, PendingTarget::Origin::Saved}, Degrade::None};
  }
  if (s.offsetJump.has_value())
    return {{PendingTarget::CharOffset, *s.offsetJump, 0, 0, PendingTarget::Origin::OffsetJump}, Degrade::None};
  if (s.hasAnchor)
    return {{PendingTarget::AnchorHash, 0, s.anchorHash, 0, PendingTarget::Origin::Anchor}, Degrade::None};
  return {{}, Degrade::None};
}

// Resolver outcome (one pass).
struct ResolveOutcome {
  bool resolved;       // target was resolved (covered or terminal)
  int page;            // resolved page index (valid when resolved)
  bool needFullBuild;  // full build required (Percent/LastPage kinds, or not yet covered)
};

// Cover check result (§3.3).
struct CoverResult {
  bool covered;
  uint32_t page;
};

// Cover check (§3.3). Returns {covered, page} or {false, 0} (not covered).
// pageForChar: maps a char offset → page index (returns 0 if no pages built).
// pageCharStart: maps a page index → charStart of that page (0 if out of range).
// available: availablePageCount() (pages built so far).
// haveTotal: session/cache complete.
CoverResult coverCheck(uint32_t target, uint32_t available, bool haveTotal,
                       const std::function<uint32_t(uint32_t charOffset)>& pageForChar,
                       const std::function<uint32_t(uint16_t pageIndex)>& pageCharStart);

// Resolve the pending target against the current build state (§3.2 policy).
// Pure: latch consumption lives in the caller, which knows the target's origin
// and owns the source latches (ttfResolveTargetPage clears them on resolved).
//   CharOffset / AnchorHash → charOffset holds the chapter char offset target
//   (the caller resolves AnchorHash to a char offset via charForAnchor first).
//   Page   → charOffset holds the target PAGE index (page-anchored records,
//            preview seed): covered as soon as that many pages exist.
//   Percent → active.percent; LastPage → sentinel. Both wait for the total.
// pageForChar: (charOffset) → page index, returns false if not built.
// pageCharStart: (pageIndex) → charStart (0 if out of range).
// haveTotal: session/cache complete.
ResolveOutcome evaluate(const PendingTarget& target, uint32_t available,
                        const std::function<bool(uint32_t charOffset, uint32_t& pageOut)>& pageForChar,
                        const std::function<uint32_t(uint16_t pageIndex)>& pageCharStart, bool haveTotal);

}  // namespace ttf_resolve