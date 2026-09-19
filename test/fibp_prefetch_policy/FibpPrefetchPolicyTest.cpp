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
