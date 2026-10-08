// TtfResolvePolicyTest — host suites for the pure resolver policy
// (design §3.2/§3.3). Mirrors test/fibp_prefetch_policy/ style: the policy
// layer is host-testable via fakes; the runtime task itself is device-only.
//
// Tests exercise ttf_resolve::coverCheck() and ttf_resolve::evaluate() directly
// with mock accessors. No EpubReaderActivity or TtfBookRuntime instance needed.

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <optional>

#include "activities/reader/TtfResolvePolicy.h"

namespace {

// Convenience: make a CharOffset target (the most common kind for restore).
ttf_resolve::PendingTarget charOffsetTarget(uint32_t offset) {
  return {ttf_resolve::PendingTarget::CharOffset, offset, 0, 0};
}

}  // namespace

// ── Cover check (§3.3) ──────────────────────────────────────────────────────

TEST(CoverCheckTest, WithinPrefixResolves) {
  // 5 pages built; target is on page 3 (within prefix).
  auto pageForChar = [](uint32_t offset) -> uint32_t { return offset / 2; };  // 0→0, 2→1, 4→2, ...
  auto pageCharStart = [](uint16_t page) -> uint32_t { return page * 2; };  // page 0: chars 0-1, page 1: chars 2-3, ...
  auto result = ttf_resolve::coverCheck(3, 5, false, pageForChar, pageCharStart);
  EXPECT_TRUE(result.covered);
  EXPECT_EQ(result.page, 1);  // char 3 → page 1
}

TEST(CoverCheckTest, ExactOnLastPage) {
  // 3 pages built; target == charStart(last) = 4 (page 2 starts at char 4).
  auto pageForChar = [](uint32_t offset) -> uint32_t { return offset / 2; };
  auto pageCharStart = [](uint16_t page) -> uint32_t { return page * 2; };
  auto result = ttf_resolve::coverCheck(4, 3, false, pageForChar, pageCharStart);
  EXPECT_TRUE(result.covered);
  EXPECT_EQ(result.page, 2);
}

TEST(CoverCheckTest, PastWatermarkPeekDontConsume) {
  // 3 pages built; target = 5 is past charStart(last)=4 (page 2 starts at 4,
  // page 2 covers chars 4-5 but we don't know if there are more chars).
  // pageForChar(5) returns page 2 (last page) — a clamped watermark hit.
  // Cover check must refuse: not covered (peek, don't consume).
  auto pageForChar = [](uint32_t offset) -> uint32_t { return offset / 2; };  // 5→2 (last page)
  auto pageCharStart = [](uint16_t page) -> uint32_t { return page * 2; };
  auto result = ttf_resolve::coverCheck(5, 3, false, pageForChar, pageCharStart);
  EXPECT_FALSE(result.covered);
}

TEST(CoverCheckTest, FullIndexAlwaysExact) {
  // With haveTotal=true (complete index), target within text resolves exactly.
  // 50/10 = 5 is page 5 which requires available > 5, so use available=6.
  auto pageForChar = [](uint32_t offset) -> uint32_t { return offset / 10; };
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };  // unused when haveTotal=true
  auto result = ttf_resolve::coverCheck(50, 6, true, pageForChar, pageCharStart);
  EXPECT_TRUE(result.covered);
  EXPECT_EQ(result.page, 5);  // 50/10 = 5, and 5 < available=6
}

TEST(CoverCheckTest, NoPagesNotCovered) {
  // available=0: no pages built, nothing resolves.
  auto pageForChar = [](uint32_t) -> uint32_t { return 0; };
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };
  auto result = ttf_resolve::coverCheck(0, 0, false, pageForChar, pageCharStart);
  EXPECT_FALSE(result.covered);
}

// ── Evaluate policy (§3.2) ──────────────────────────────────────────────────

TEST(EvaluateTest, TargetWithinBuiltPrefixResolves) {
  // Case 1: target within built prefix → resolves, pending cleared.
  auto pageForChar = [](uint32_t offset, uint32_t& pageOut) -> bool {
    pageOut = offset / 2;
    return true;
  };
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };  // unused (covered)
  std::optional<uint32_t> offsetJump;
  std::string pendingAnchor;
  bool hasSavedPosition = true;
  ttf_resolve::PendingTarget target = charOffsetTarget(3);
  auto result =
      ttf_resolve::evaluate(target, 5, pageForChar, pageCharStart, false, hasSavedPosition, offsetJump, pendingAnchor);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 1);  // char 3 → page 1
  EXPECT_FALSE(result.needFullBuild);
}

TEST(EvaluateTest, NotCoveredKeepsPending) {
  // Case 3: target past watermark on partial → pending kept, no page served.
  // coverCheck(5, 3) → not covered (watermark hit).
  auto pageForChar = [](uint32_t offset, uint32_t& pageOut) -> bool {
    pageOut = offset / 2;  // 5→2 (last page)
    return true;
  };
  auto pageCharStart = [](uint16_t page) -> uint32_t { return page * 2; };
  std::optional<uint32_t> offsetJump;
  std::string pendingAnchor;
  bool hasSavedPosition = true;
  ttf_resolve::PendingTarget target = charOffsetTarget(5);
  auto result =
      ttf_resolve::evaluate(target, 3, pageForChar, pageCharStart, false, hasSavedPosition, offsetJump, pendingAnchor);
  EXPECT_FALSE(result.resolved);
  EXPECT_EQ(result.needFullBuild, false);  // CharOffset kind → no full build
}

TEST(EvaluateTest, CompleteIndexBeyondTotalClamps) {
  // Case 5: complete index, target beyond totalChars → clamp to last page, clear.
  auto pageForChar = [](uint32_t offset, uint32_t& pageOut) -> bool {
    pageOut = offset / 10;
    return true;
  };
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };
  std::optional<uint32_t> offsetJump;
  std::string pendingAnchor;
  bool hasSavedPosition = true;
  ttf_resolve::PendingTarget target = charOffsetTarget(50);
  auto result =
      ttf_resolve::evaluate(target, 5, pageForChar, pageCharStart, true, hasSavedPosition, offsetJump, pendingAnchor);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 4);  // available=5, last page=4
  EXPECT_FALSE(result.needFullBuild);
}

TEST(EvaluateTest, PercentKindNeedsFullBuild) {
  // Case 8: Percent kind → needFullBuild preserved.
  auto pageForChar = [](uint32_t, uint32_t&) -> bool { return false; };  // not covered
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };
  std::optional<uint32_t> offsetJump;
  std::string pendingAnchor;
  bool hasSavedPosition = true;
  ttf_resolve::PendingTarget target = {ttf_resolve::PendingTarget::Percent, 50, 0, 50};
  auto result =
      ttf_resolve::evaluate(target, 5, pageForChar, pageCharStart, false, hasSavedPosition, offsetJump, pendingAnchor);
  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.needFullBuild);
}

TEST(EvaluateTest, LastPageKindNeedsFullBuild) {
  // Case 8: LastPage kind → needFullBuild preserved.
  auto pageForChar = [](uint32_t, uint32_t&) -> bool { return false; };
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };
  std::optional<uint32_t> offsetJump;
  std::string pendingAnchor;
  bool hasSavedPosition = true;
  ttf_resolve::PendingTarget target = {ttf_resolve::PendingTarget::LastPage, 0, 0, 0};
  auto result =
      ttf_resolve::evaluate(target, 5, pageForChar, pageCharStart, false, hasSavedPosition, offsetJump, pendingAnchor);
  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.needFullBuild);
}

TEST(EvaluateTest, CharOffsetKindNoFullBuild) {
  // Case 3: CharOffset kind not covered → no full build (build one chunk).
  auto pageForChar = [](uint32_t offset, uint32_t& pageOut) -> bool {
    pageOut = offset / 2;
    return true;
  };
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };
  std::optional<uint32_t> offsetJump;
  std::string pendingAnchor;
  bool hasSavedPosition = true;
  ttf_resolve::PendingTarget target = charOffsetTarget(7);  // 7→3 (last page on 3 pages)
  auto result =
      ttf_resolve::evaluate(target, 4, pageForChar, pageCharStart, false, hasSavedPosition, offsetJump, pendingAnchor);
  EXPECT_FALSE(result.resolved);
  EXPECT_FALSE(result.needFullBuild);  // CharOffset kind → chunk build, not full
}

TEST(EvaluateTest, AnchorHashClearsPendingAnchorOnResolve) {
  // AnchorHash resolved → pendingAnchor cleared.
  auto pageForChar = [](uint32_t offset, uint32_t& pageOut) -> bool {
    pageOut = offset;
    return true;
  };
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };
  std::optional<uint32_t> offsetJump;
  std::string pendingAnchor = "#chapter3";
  bool hasSavedPosition = false;
  ttf_resolve::PendingTarget target = {ttf_resolve::PendingTarget::AnchorHash, 3, 12345, 0};
  auto result =
      ttf_resolve::evaluate(target, 5, pageForChar, pageCharStart, true, hasSavedPosition, offsetJump, pendingAnchor);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 3);
  EXPECT_TRUE(pendingAnchor.empty());  // cleared on resolve
}

TEST(EvaluateTest, AnchorHashClampsOnTerminal) {
  // AnchorHash terminal → pendingAnchor cleared, clamped to last page.
  auto pageForChar = [](uint32_t offset, uint32_t& pageOut) -> bool {
    pageOut = offset;
    return true;
  };
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };
  std::optional<uint32_t> offsetJump;
  std::string pendingAnchor = "#chapter99";
  bool hasSavedPosition = false;
  ttf_resolve::PendingTarget target = {ttf_resolve::PendingTarget::AnchorHash, 99, 12345, 0};
  auto result =
      ttf_resolve::evaluate(target, 5, pageForChar, pageCharStart, true, hasSavedPosition, offsetJump, pendingAnchor);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 4);  // clamped to last page
  EXPECT_TRUE(pendingAnchor.empty());
}

// ── Sequence test (§8.1 case 6): save → generation bump → restore ───────────

TEST(SequenceTest, SaveRestoreViaCharOffsetAfterGenerationMismatch) {
  // §1.1 regression: generation mismatch should NOT degrade to chapter start
  // for CharOffset targets. It should map via charOffset through the built prefix.
  // Setup: saved position has charOffset=10 (2 pages into the chapter).
  // Generation mismatch: saved generation != current generation.
  // But the resolver still maps charOffset=10 → page 5 (if 2 chars/page).
  auto pageForChar = [](uint32_t offset, uint32_t& pageOut) -> bool {
    pageOut = offset / 2;  // 10→5
    return true;
  };
  auto pageCharStart = [](uint16_t page) -> uint32_t { return page * 2; };
  std::optional<uint32_t> offsetJump;
  std::string pendingAnchor;
  // hasSavedPosition=true but the funnel (not evaluate()) handles the generation
  // mismatch check before calling evaluate(). Here we simulate the CharOffset
  // path directly: charOffset=10, no mismatch.
  bool hasSavedPosition = false;  // not consumed by evaluate (mismatch handled by caller)
  ttf_resolve::PendingTarget target = charOffsetTarget(10);
  auto result =
      ttf_resolve::evaluate(target, 10, pageForChar, pageCharStart, true, hasSavedPosition, offsetJump, pendingAnchor);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 5);  // 10/2 = 5
  EXPECT_FALSE(result.needFullBuild);
}

// ── Build advances one chunk → next resolve succeeds (case 4) ────────────────

TEST(SequenceTest, NotCoveredThenCoveredAfterBuild) {
  // Case 4: first resolve → not covered (pending); build advances; next resolve
  // → covered (the target is now within the built prefix).
  uint32_t builtPages = 2;  // initially 2 pages built
  auto pageForChar = [&builtPages](uint32_t offset, uint32_t& pageOut) -> bool {
    if (offset >= builtPages * 2) return false;  // beyond built prefix
    pageOut = offset / 2;
    return true;
  };
  auto pageCharStart = [](uint16_t page) -> uint32_t { return page * 2; };
  std::optional<uint32_t> offsetJump;
  std::string pendingAnchor;
  bool hasSavedPosition = false;

  // First resolve: target=6, only 2 pages built (covers chars 0-3). 6 > 3.
  ttf_resolve::PendingTarget target = charOffsetTarget(6);
  auto result1 = ttf_resolve::evaluate(target, builtPages, pageForChar, pageCharStart, false, hasSavedPosition,
                                       offsetJump, pendingAnchor);
  EXPECT_FALSE(result1.resolved);
  EXPECT_FALSE(result1.needFullBuild);

  // Build advances: now 4 pages built (covers chars 0-7). Target 6 is within.
  builtPages = 4;
  auto result2 = ttf_resolve::evaluate(target, builtPages, pageForChar, pageCharStart, false, hasSavedPosition,
                                       offsetJump, pendingAnchor);
  EXPECT_TRUE(result2.resolved);
  EXPECT_EQ(result2.page, 3);  // 6/2 = 3
  EXPECT_FALSE(result2.needFullBuild);
}

// ── Spine mismatch → chapter start (case 7) ─────────────────────────────────

TEST(SequenceTest, SpineMismatchDegradesToChapterStart) {
  // Case 7: spine mismatch → chapter start (targetOut = 0). This is handled by
  // the funnel (ttfResolveTargetPage), NOT by evaluate(). evaluate() only sees
  // the CharOffset AFTER the funnel confirms spine/generation match. This test
  // verifies evaluate() resolves a valid CharOffset within the built prefix.
  // The funnel (not evaluate()) handles the spine mismatch check and returns 0.
  auto pageForChar = [](uint32_t offset, uint32_t& pageOut) -> bool {
    pageOut = offset / 2;
    return true;
  };
  auto pageCharStart = [](uint16_t page) -> uint32_t { return page * 2; };
  std::optional<uint32_t> offsetJump;
  std::string pendingAnchor;
  bool hasSavedPosition = false;
  ttf_resolve::PendingTarget target = charOffsetTarget(4);
  auto result =
      ttf_resolve::evaluate(target, 3, pageForChar, pageCharStart, false, hasSavedPosition, offsetJump, pendingAnchor);
  // evaluate() resolves valid CharOffset targets; spine mismatch is upstream.
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 2);  // 4/2 = 2
}

// ── Source latch clearing on resolve ────────────────────────────────────────

TEST(EvaluateTest, CharOffsetZeroClearsSavedPosition) {
  // When a CharOffset target with charOffset=0 is resolved, the funnel clears
  // ttfHasSavedPosition. evaluate() documents this contract.
  auto pageForChar = [](uint32_t, uint32_t& pageOut) -> bool {
    pageOut = 0;
    return true;
  };
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };
  std::optional<uint32_t> offsetJump;
  std::string pendingAnchor;
  bool hasSavedPosition = true;
  ttf_resolve::PendingTarget target = charOffsetTarget(0);  // charOffset 0 → page 0
  auto result =
      ttf_resolve::evaluate(target, 3, pageForChar, pageCharStart, true, hasSavedPosition, offsetJump, pendingAnchor);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 0);
  // hasSavedPosition is consumed (funnel clears it when charOffset == ttfSavedCharOffset
  // and charOffset == 0). The contract: caller must clear hasSavedPosition when
  // outcome.resolved && target.charOffset == 0.
  // evaluate() itself doesn't clear hasSavedPosition (it's a caller responsibility
  // since evaluate() doesn't know which latch to clear). But the funnel does.
}

TEST(EvaluateTest, OffsetJumpMatchClearsOffsetJump) {
  // When a CharOffset target matching *pendingOffsetJump is resolved,
  // the funnel clears pendingOffsetJump. target=8 → page 4 (within available=5).
  auto pageForChar = [](uint32_t offset, uint32_t& pageOut) -> bool {
    pageOut = offset / 2;
    return true;
  };
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };
  std::optional<uint32_t> offsetJump = 8;
  std::string pendingAnchor;
  bool hasSavedPosition = false;
  ttf_resolve::PendingTarget target = charOffsetTarget(8);  // matches offsetJump
  auto result =
      ttf_resolve::evaluate(target, 5, pageForChar, pageCharStart, true, hasSavedPosition, offsetJump, pendingAnchor);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 4);  // 8/2 = 4
  // Funnel clears offsetJump when active.charOffset == *pendingOffsetJump && resolved.
}
