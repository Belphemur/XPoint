// FibpPrefetchPolicyTest — host suites for the FIBP prefetch worker's pure
// scheduling/stop policy (queue order per R4, chunk-level stop per R3). The
// worker task itself is device-only; the policy layer is the host-testable
// contract. The engine-level "cancelled session writes nothing" guarantee is
// pinned by ChapterIndexEngineTest (CancelBeforeFirstChunkWritesNothing).

#include <gtest/gtest.h>

#include <array>

#include "activities/reader/FibpPrefetchPolicy.h"

namespace fibp = freeink::book::fibp;

namespace {

// Convenience: build into a large buffer, return the plan as a vector.
std::vector<uint16_t> plan(const uint16_t spineCount, const uint16_t notified, const uint16_t cap = 512) {
  std::vector<uint16_t> out(cap, 0xFFFF);
  const uint16_t n = fibp::buildQueue(spineCount, notified, out.data(), cap);
  out.resize(n);
  return out;
}

}  // namespace

TEST(BuildQueueTest, NoChapterEnteredCoversInOrder) {
  const auto q = plan(5, fibp::kNoChapter);
  EXPECT_EQ(q, (std::vector<uint16_t>{0, 1, 2, 3, 4}));
}

TEST(BuildQueueTest, EnteredChapterDeferredToLast) {
  // R4: the chapter AFTER the entered one is built first; the entered one
  // (already on screen) is built last.
  const auto q = plan(5, 2);
  EXPECT_EQ(q, (std::vector<uint16_t>{3, 4, 0, 1, 2}));
}

TEST(BuildQueueTest, WrapsFromTailToHead) {
  // Entering the LAST chapter wraps the "next" chapter to spine 0.
  const auto q = plan(5, 4);
  EXPECT_EQ(q, (std::vector<uint16_t>{0, 1, 2, 3, 4}));
}

TEST(BuildQueueTest, OutOfRangeNotifiedFallsBackToInOrder) {
  // Defensive: a notified spine beyond the book's spine count (stale
  // navigation into a rebuilt catalog) degrades to the plain order.
  const auto q = plan(3, 7);
  EXPECT_EQ(q, (std::vector<uint16_t>{0, 1, 2}));
}

TEST(BuildQueueTest, CapLimitsOutput) {
  std::array<uint16_t, 3> buf{};
  const uint16_t n = fibp::buildQueue(10, 2, buf.data(), 3);
  EXPECT_EQ(n, 3u);
  EXPECT_EQ(buf, (std::array<uint16_t, 3>{3, 4, 5}));
}

TEST(BuildQueueTest, CappedWindowIsOnlyTheNextSpine) {
  // The prefetch window is capped at kPrefetchLookaheadSpines: the plan
  // never contains a spine more than that far ahead of the entered one
  // (ring distance), and the queue holds exactly one entry for a non-empty
  // book.
  const auto q = plan(10, 3, fibp::kPrefetchLookaheadSpines);
  EXPECT_EQ(q, (std::vector<uint16_t>{4}));
  ASSERT_EQ(q.size(), fibp::kPrefetchLookaheadSpines);
  for (const uint16_t spine : q) {
    const uint16_t ahead = static_cast<uint16_t>((spine + 10 - 3) % 10);
    EXPECT_LE(ahead, fibp::kPrefetchLookaheadSpines);
  }
}

TEST(BuildQueueTest, WindowSlidesOnNotify) {
  // Sliding the window = re-planning with the new notified spine; the plan
  // always names exactly the next chapter.
  EXPECT_EQ(plan(5, 0, fibp::kPrefetchLookaheadSpines), (std::vector<uint16_t>{1}));
  EXPECT_EQ(plan(5, 1, fibp::kPrefetchLookaheadSpines), (std::vector<uint16_t>{2}));
  EXPECT_EQ(plan(5, 3, fibp::kPrefetchLookaheadSpines), (std::vector<uint16_t>{4}));
  EXPECT_EQ(plan(5, 4, fibp::kPrefetchLookaheadSpines), (std::vector<uint16_t>{0}));  // tail wraps to head
}

TEST(BuildQueueTest, DegenerateInputs) {
  std::array<uint16_t, 4> buf{};
  EXPECT_EQ(fibp::buildQueue(0, 0, buf.data(), 4), 0u);  // no spines
  EXPECT_EQ(fibp::buildQueue(4, 0, nullptr, 4), 0u);     // no buffer
  EXPECT_EQ(fibp::buildQueue(4, 0, buf.data(), 0), 0u);  // no capacity
}

TEST(ShouldStopChunkTest, StopOnCancelOrGenerationBump) {
  EXPECT_TRUE(fibp::shouldStopChunk(true, 0xAAAA, 0xAAAA));    // book closing
  EXPECT_TRUE(fibp::shouldStopChunk(false, 0xBBBB, 0xAAAA));   // settings changed
  EXPECT_FALSE(fibp::shouldStopChunk(false, 0xAAAA, 0xAAAA));  // keep building
  // Generation 0 is a legitimate value (all-rejected face sets); the
  // comparison is plain equality, no sentinel special cases.
  EXPECT_FALSE(fibp::shouldStopChunk(false, 0, 0));
  EXPECT_TRUE(fibp::shouldStopChunk(false, 0, 1));
}

// ── Progress trigger (owner directive, 2026-09-19) ──────────────────────────

TEST(ShouldPrefetchNextTest, NeverTriggersWithUnknownPageCount) { EXPECT_FALSE(fibp::shouldPrefetchNext(0, 0)); }

TEST(ShouldPrefetchNextTest, NotBeforeTheThreshold) {
  // 200-page chapter: 10% = 20 pages. Early pages never trigger.
  EXPECT_FALSE(fibp::shouldPrefetchNext(0, 200));
  EXPECT_FALSE(fibp::shouldPrefetchNext(90, 200));
  EXPECT_FALSE(fibp::shouldPrefetchNext(170, 200));
  // 179 remaining 21 > 20 → no; 180 remaining 20 == 20 → yes (≤ 10%).
  EXPECT_FALSE(fibp::shouldPrefetchNext(179, 200));
  EXPECT_TRUE(fibp::shouldPrefetchNext(180, 200));
  EXPECT_TRUE(fibp::shouldPrefetchNext(199, 200));
}

TEST(ShouldPrefetchNextTest, TriggersExactlyAtTenPercent) {
  // 100-page chapter: threshold 10. With 0-based pages, page 89 is the 90th
  // page read → remaining 11 > 10 (no); page 90 → remaining 10 = the exact
  // 10% boundary → fires.
  EXPECT_FALSE(fibp::shouldPrefetchNext(88, 100));
  EXPECT_FALSE(fibp::shouldPrefetchNext(89, 100));
  EXPECT_TRUE(fibp::shouldPrefetchNext(90, 100));
}

TEST(ShouldPrefetchNextTest, CeilKeepsOnePageRemainingFiringOnTinyChapters) {
  // ceil keeps small chapters usable: any pageCount with a single page left
  // triggers, even where 10% < 1 page.
  EXPECT_TRUE(fibp::shouldPrefetchNext(4, 5));   // remaining 1; ceil(0.5)=1
  EXPECT_FALSE(fibp::shouldPrefetchNext(3, 5));  // remaining 2 > 1
  EXPECT_TRUE(fibp::shouldPrefetchNext(0, 1));   // single-page chapter: fires at entry
  EXPECT_TRUE(fibp::shouldPrefetchNext(2, 3));   // remaining 1; ceil(0.3)=1
}

TEST(ShouldPrefetchNextTest, EnterPastThresholdFiresImmediately) {
  // Entering a chapter already inside the last 10% (resume/short chapters)
  // triggers at page 0 of the position report — page here is the entry page.
  EXPECT_TRUE(fibp::shouldPrefetchNext(95, 100));
  EXPECT_TRUE(fibp::shouldPrefetchNext(0, 1));
}
