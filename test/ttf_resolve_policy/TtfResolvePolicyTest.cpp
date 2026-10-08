// TtfResolvePolicyTest — host suites for the pure resolver policy
// (design §3.2/§3.3). Mirrors test/fibp_prefetch_policy/ style: the policy
// layer is host-testable via fakes; the runtime task itself is device-only.
//
// Tests exercise ttf_resolve::coverCheck(), ttf_resolve::evaluate() and
// ttf_resolve::deriveTarget() directly with mock accessors. No
// EpubReaderActivity or TtfBookRuntime instance needed. evaluate() and
// deriveTarget() are pure: latch side effects belong to the funnel caller
// (ttfResolveTargetPage), which clears the latch named by the target's origin.

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <optional>

#include "activities/reader/TtfResolvePolicy.h"

namespace {

// Convenience: make a CharOffset target (the most common kind for restore).
ttf_resolve::PendingTarget charOffsetTarget(uint32_t offset) {
  return {ttf_resolve::PendingTarget::CharOffset, offset, 0, 0, ttf_resolve::PendingTarget::OriginNone};
}

// Standard fake index: 2 chars per page, page N starts at char 2N.
auto pageForChar2PerPage(uint32_t offset, uint32_t& pageOut) -> bool {
  pageOut = offset / 2;
  return true;
}
auto pageCharStart2PerPage(uint16_t page) -> uint32_t { return page * 2; }

}  // namespace

// ── Cover check (§3.3) ──────────────────────────────────────────────────────

TEST(CoverCheckTest, WithinPrefixResolves) {
  // 5 pages built; target is on page 3 (within prefix).
  auto pageForChar = [](uint32_t offset) -> uint32_t { return offset / 2; };  // 0→0, 2→1, 4→2, ...
  auto result = ttf_resolve::coverCheck(3, 5, false, pageForChar, pageCharStart2PerPage);
  EXPECT_TRUE(result.covered);
  EXPECT_EQ(result.page, 1);  // char 3 → page 1
}

TEST(CoverCheckTest, ExactOnLastPage) {
  // 3 pages built; target == charStart(last) = 4 (page 2 starts at char 4).
  auto pageForChar = [](uint32_t offset) -> uint32_t { return offset / 2; };
  auto result = ttf_resolve::coverCheck(4, 3, false, pageForChar, pageCharStart2PerPage);
  EXPECT_TRUE(result.covered);
  EXPECT_EQ(result.page, 2);
}

TEST(CoverCheckTest, PastWatermarkPeekDontConsume) {
  // 3 pages built; target = 5 is past charStart(last)=4 (page 2 starts at 4,
  // page 2 covers chars 4-5 but we don't know if there are more chars).
  // pageForChar(5) returns page 2 (last page) — a clamped watermark hit.
  // Cover check must refuse: not covered (peek, don't consume).
  auto pageForChar = [](uint32_t offset) -> uint32_t { return offset / 2; };  // 5→2 (last page)
  auto result = ttf_resolve::coverCheck(5, 3, false, pageForChar, pageCharStart2PerPage);
  EXPECT_FALSE(result.covered);
}

TEST(CoverCheckTest, FullIndexAlwaysExact) {
  // With haveTotal=true (complete index), target within text resolves exactly.
  auto pageForChar = [](uint32_t offset) -> uint32_t { return offset / 10; };
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };  // unused when haveTotal=true
  auto result = ttf_resolve::coverCheck(50, 6, true, pageForChar, pageCharStart);
  EXPECT_TRUE(result.covered);
  EXPECT_EQ(result.page, 5);  // 50/10 = 5, and 5 < available=6
}

TEST(CoverCheckTest, NoPagesNotCovered) {
  // available=0: no pages built, nothing resolves.
  auto pageForChar = [](uint32_t) -> uint32_t { return 0; };
  auto result = ttf_resolve::coverCheck(0, 0, false, pageForChar, pageCharStart2PerPage);
  EXPECT_FALSE(result.covered);
}

TEST(CoverCheckTest, PageCountBeyondUint16DomainRefused) {
  // pageCharStart takes a uint16_t page index; a page count beyond that
  // domain must refuse the last-page charStart peek instead of truncating
  // the cast (which would read an unrelated page's charStart).
  auto pageForChar = [](uint32_t offset) -> uint32_t { return offset / 2; };  // clamps to last
  auto pageCharStart = [](uint16_t page) -> uint32_t { return page * 2; };
  ttf_resolve::CoverResult result = ttf_resolve::coverCheck(140000, 70000, false, pageForChar, pageCharStart);
  EXPECT_FALSE(result.covered);  // watermark ambiguity cannot be checked: not covered
}

// ── Evaluate policy (§3.2) ──────────────────────────────────────────────────

TEST(EvaluateTest, TargetWithinBuiltPrefixResolves) {
  // Case 1: target within built prefix → resolves.
  auto result = ttf_resolve::evaluate(charOffsetTarget(3), 5, pageForChar2PerPage, pageCharStart2PerPage, false);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 1);  // char 3 → page 1
  EXPECT_FALSE(result.needFullBuild);
}

TEST(EvaluateTest, NotCoveredKeepsPending) {
  // Case 3: target past watermark on partial → pending kept, no page served.
  auto result = ttf_resolve::evaluate(charOffsetTarget(5), 3, pageForChar2PerPage, pageCharStart2PerPage, false);
  EXPECT_FALSE(result.resolved);
  EXPECT_FALSE(result.needFullBuild);  // CharOffset kind → chunk build, not full
}

TEST(EvaluateTest, CompleteIndexBeyondTotalClamps) {
  // Case 2: complete index, target beyond totalChars → clamp to last page.
  auto pageForChar = [](uint32_t offset, uint32_t& pageOut) -> bool {
    pageOut = offset / 10;
    return true;
  };
  auto pageCharStart = [](uint16_t) -> uint32_t { return 0; };
  auto result = ttf_resolve::evaluate(charOffsetTarget(50), 5, pageForChar, pageCharStart, true);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 4);  // available=5, last page=4
  EXPECT_FALSE(result.needFullBuild);
}

TEST(EvaluateTest, PercentKindNeedsFullBuild) {
  // Percent kind → needFullBuild preserved until the total is known.
  auto pageForChar = [](uint32_t, uint32_t&) -> bool { return false; };
  ttf_resolve::PendingTarget target = {ttf_resolve::PendingTarget::Percent, 0, 0, 50,
                                       ttf_resolve::PendingTarget::OriginNone};
  auto result = ttf_resolve::evaluate(target, 5, pageForChar, pageCharStart2PerPage, false);
  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.needFullBuild);
}

TEST(EvaluateTest, LastPageKindNeedsFullBuild) {
  // LastPage kind → needFullBuild preserved until the total is known.
  auto pageForChar = [](uint32_t, uint32_t&) -> bool { return false; };
  ttf_resolve::PendingTarget target = {ttf_resolve::PendingTarget::LastPage, 0, 0, 0,
                                       ttf_resolve::PendingTarget::OriginNone};
  auto result = ttf_resolve::evaluate(target, 5, pageForChar, pageCharStart2PerPage, false);
  EXPECT_FALSE(result.resolved);
  EXPECT_TRUE(result.needFullBuild);
}

TEST(EvaluateTest, LastPageKindResolvesToLastPageWhenComplete) {
  auto pageForChar = [](uint32_t, uint32_t&) -> bool { return false; };
  ttf_resolve::PendingTarget target = {ttf_resolve::PendingTarget::LastPage, 0, 0, 0,
                                       ttf_resolve::PendingTarget::OriginNone};
  auto result = ttf_resolve::evaluate(target, 5, pageForChar, pageCharStart2PerPage, true);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 4);
  EXPECT_FALSE(result.needFullBuild);
}

// ── Page kind (page-anchored records, preview seed) ─────────────────────────

TEST(EvaluateTest, PageKindCoveredResolvesDirectly) {
  // A Page target resolves as soon as that many pages exist — no char-offset
  // mapping, no watermark ambiguity.
  ttf_resolve::PendingTarget target = {ttf_resolve::PendingTarget::Page, 3, 0, 0,
                                       ttf_resolve::PendingTarget::OriginNone};
  auto result = ttf_resolve::evaluate(target, 5, pageForChar2PerPage, pageCharStart2PerPage, false);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 3);
  EXPECT_FALSE(result.needFullBuild);
}

TEST(EvaluateTest, PageKindBeyondAvailableStaysPending) {
  ttf_resolve::PendingTarget target = {ttf_resolve::PendingTarget::Page, 6, 0, 0,
                                       ttf_resolve::PendingTarget::OriginNone};
  auto result = ttf_resolve::evaluate(target, 5, pageForChar2PerPage, pageCharStart2PerPage, false);
  EXPECT_FALSE(result.resolved);
  EXPECT_FALSE(result.needFullBuild);  // builds one chunk toward it
}

TEST(EvaluateTest, PageKindTerminalClampsWhenComplete) {
  // Complete index, page target past the end → clamp to last page.
  ttf_resolve::PendingTarget target = {ttf_resolve::PendingTarget::Page, 99, 0, 0,
                                       ttf_resolve::PendingTarget::OriginNone};
  auto result = ttf_resolve::evaluate(target, 5, pageForChar2PerPage, pageCharStart2PerPage, true);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 4);
}

// ── Latch derivation (§3.2 priority + §7 degrade) ───────────────────────────

TEST(DeriveTargetTest, SeedWinsPriority) {
  ttf_resolve::LatchState s;
  s.reflowSeedPage = 7;
  s.hasSavedPosition = true;
  s.savedCharOffset = 10;
  s.savedGeneration = s.generation;
  s.offsetJump = 42;
  s.hasAnchor = true;
  auto d = ttf_resolve::deriveTarget(s);
  EXPECT_EQ(d.degrade, ttf_resolve::Degrade::None);
  EXPECT_EQ(d.target.kind, ttf_resolve::PendingTarget::Page);
  EXPECT_EQ(d.target.charOffset, 7u);  // seed is a PAGE number, not a char offset
  EXPECT_EQ(d.target.origin, ttf_resolve::PendingTarget::Origin::Seed);
}

TEST(DeriveTargetTest, SavedOffsetGenerationMatchDerivesCharOffset) {
  ttf_resolve::LatchState s;
  s.hasSavedPosition = true;
  s.savedSpine = 3;
  s.currentSpine = 3;
  s.savedCharOffset = 10;
  s.savedGeneration = 5;
  s.generation = 5;
  auto d = ttf_resolve::deriveTarget(s);
  EXPECT_EQ(d.degrade, ttf_resolve::Degrade::None);
  EXPECT_EQ(d.target.kind, ttf_resolve::PendingTarget::CharOffset);
  EXPECT_EQ(d.target.charOffset, 10u);
  EXPECT_EQ(d.target.origin, ttf_resolve::PendingTarget::Origin::Saved);
}

TEST(DeriveTargetTest, SavedPageAnchoredRecordDerivesPageTarget) {
  // A record with charOffset==0 (e.g. KOReader remote-accept) restores by its
  // page number instead of mapping offset 0 → page 0.
  ttf_resolve::LatchState s;
  s.hasSavedPosition = true;
  s.savedSpine = 3;
  s.currentSpine = 3;
  s.savedCharOffset = 0;
  s.savedPage = 12;
  s.savedGeneration = 5;
  s.generation = 5;
  auto d = ttf_resolve::deriveTarget(s);
  EXPECT_EQ(d.degrade, ttf_resolve::Degrade::None);
  EXPECT_EQ(d.target.kind, ttf_resolve::PendingTarget::Page);
  EXPECT_EQ(d.target.charOffset, 12u);
  EXPECT_EQ(d.target.origin, ttf_resolve::PendingTarget::Origin::Saved);
}

TEST(DeriveTargetTest, SavedGenerationMismatchDegradesToChapterStart) {
  // §7: a saved record whose layout generation no longer matches cannot map
  // under the current layout — degrade to the chapter start (caller clears
  // the latch and returns targetOut = 0).
  ttf_resolve::LatchState s;
  s.hasSavedPosition = true;
  s.savedSpine = 3;
  s.currentSpine = 3;
  s.savedCharOffset = 10;
  s.savedGeneration = 5;
  s.generation = 6;  // font changed between save and reopen
  auto d = ttf_resolve::deriveTarget(s);
  EXPECT_EQ(d.degrade, ttf_resolve::Degrade::ChapterStart);
  EXPECT_EQ(d.target.kind, ttf_resolve::PendingTarget::None);
}

TEST(DeriveTargetTest, SavedSpineMismatchDegradesToChapterStart) {
  ttf_resolve::LatchState s;
  s.hasSavedPosition = true;
  s.savedSpine = 2;
  s.currentSpine = 3;
  s.savedCharOffset = 10;
  s.savedGeneration = s.generation;
  auto d = ttf_resolve::deriveTarget(s);
  EXPECT_EQ(d.degrade, ttf_resolve::Degrade::ChapterStart);
}

TEST(DeriveTargetTest, OffsetJumpDerivesCharOffset) {
  ttf_resolve::LatchState s;
  s.offsetJump = 42;
  auto d = ttf_resolve::deriveTarget(s);
  EXPECT_EQ(d.degrade, ttf_resolve::Degrade::None);
  EXPECT_EQ(d.target.kind, ttf_resolve::PendingTarget::CharOffset);
  EXPECT_EQ(d.target.charOffset, 42u);
  EXPECT_EQ(d.target.origin, ttf_resolve::PendingTarget::Origin::OffsetJump);
}

TEST(DeriveTargetTest, AnchorDerivesAnchorHash) {
  ttf_resolve::LatchState s;
  s.hasAnchor = true;
  s.anchorHash = 12345;
  auto d = ttf_resolve::deriveTarget(s);
  EXPECT_EQ(d.degrade, ttf_resolve::Degrade::None);
  EXPECT_EQ(d.target.kind, ttf_resolve::PendingTarget::AnchorHash);
  EXPECT_EQ(d.target.idHash, 12345u);
  EXPECT_EQ(d.target.origin, ttf_resolve::PendingTarget::Origin::Anchor);
}

TEST(DeriveTargetTest, NoLatchesYieldsNone) {
  ttf_resolve::LatchState s;  // all defaults: no seed, no saved record, no jump, no anchor
  auto d = ttf_resolve::deriveTarget(s);
  EXPECT_EQ(d.degrade, ttf_resolve::Degrade::None);
  EXPECT_EQ(d.target.kind, ttf_resolve::PendingTarget::None);
}

// ── Sequence (§8.1 case 6): save → generation bump → restore ────────────────

TEST(SequenceTest, SaveRestoreViaCharOffsetAfterGenerationMismatch) {
  // The funnel-level sequence in two pure steps, matching ttfResolveTargetPage:
  // 1) deriveTarget on the reopen latches (saved record, generation bumped).
  // 2) evaluate on whatever target the derivation produced.
  //
  // Step 1: generation mismatch → degrade, NOT a silent stale-page fallback.
  ttf_resolve::LatchState reopen;
  reopen.hasSavedPosition = true;
  reopen.savedSpine = 3;
  reopen.currentSpine = 3;
  reopen.savedCharOffset = 10;
  reopen.savedGeneration = 5;
  reopen.generation = 6;
  auto derived = ttf_resolve::deriveTarget(reopen);
  ASSERT_EQ(derived.degrade, ttf_resolve::Degrade::ChapterStart);

  // Step 2: same record under a MATCHING generation (e.g. the settings were
  // reverted) → CharOffset target maps through the rebuilt prefix.
  reopen.generation = 5;
  derived = ttf_resolve::deriveTarget(reopen);
  ASSERT_EQ(derived.degrade, ttf_resolve::Degrade::None);
  ASSERT_EQ(derived.target.kind, ttf_resolve::PendingTarget::CharOffset);
  auto result = ttf_resolve::evaluate(derived.target, 10, pageForChar2PerPage, pageCharStart2PerPage, true);
  EXPECT_TRUE(result.resolved);
  EXPECT_EQ(result.page, 5);  // char 10 → page 5
  EXPECT_FALSE(result.needFullBuild);
  // Latch consumption: the target's origin is Saved, so the funnel clears
  // ttfHasSavedPosition when this outcome resolves (caller side effect).
  EXPECT_EQ(derived.target.origin, ttf_resolve::PendingTarget::Origin::Saved);
}

// ── Build advances one chunk → next resolve succeeds (§8.1 case 4) ──────────

TEST(SequenceTest, NotCoveredThenCoveredAfterBuild) {
  // First resolve → not covered (pending); build advances; next resolve →
  // covered (the target is now within the built prefix).
  uint32_t builtPages = 2;  // initially 2 pages built
  auto pageForChar = [&builtPages](uint32_t offset, uint32_t& pageOut) -> bool {
    if (offset >= builtPages * 2) return false;  // beyond built prefix
    pageOut = offset / 2;
    return true;
  };

  ttf_resolve::PendingTarget target = charOffsetTarget(6);
  auto result1 = ttf_resolve::evaluate(target, builtPages, pageForChar, pageCharStart2PerPage, false);
  EXPECT_FALSE(result1.resolved);
  EXPECT_FALSE(result1.needFullBuild);

  // Build advances: now 4 pages built (covers chars 0-7). Target 6 is within.
  builtPages = 4;
  auto result2 = ttf_resolve::evaluate(target, builtPages, pageForChar, pageCharStart2PerPage, false);
  EXPECT_TRUE(result2.resolved);
  EXPECT_EQ(result2.page, 3);  // 6/2 = 3
  EXPECT_FALSE(result2.needFullBuild);
}

// ── Stored-target restore from a partial cache (gate sequence) ──────────────

TEST(SequenceTest, DerivedRestoreTargetIsStoredAndPumps) {
  // The funnel stores every derived target into pendingRestoreTarget_, so a
  // restore derived from a source latch opens the §5.1/§5.2 gates exactly
  // like one set by invalidation. Policy-side contract: a derivation from a
  // saved latch yields a non-None target the caller can store, and evaluate
  // keeps it pending (not full-build) until the build covers it.
  ttf_resolve::LatchState s;
  s.hasSavedPosition = true;
  s.savedSpine = 0;
  s.currentSpine = 0;
  s.savedCharOffset = 10;
  s.savedGeneration = s.generation;
  auto derived = ttf_resolve::deriveTarget(s);
  ASSERT_EQ(derived.target.kind, ttf_resolve::PendingTarget::CharOffset);

  // Partial cache covers chars 0-5 only: pending, chunk-build (not full).
  auto pageForChar = [](uint32_t offset, uint32_t& pageOut) -> bool {
    if (offset > 5) return false;
    pageOut = offset / 2;
    return true;
  };
  auto result = ttf_resolve::evaluate(derived.target, 3, pageForChar, pageCharStart2PerPage, false);
  EXPECT_FALSE(result.resolved);
  EXPECT_FALSE(result.needFullBuild);  // the pump loop keeps running across passes
}