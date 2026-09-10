#include <gtest/gtest.h>

#include "lib/ProgressFlush/ProgressFlush.h"

// The progress-flush state machine (docs/design/2026-09-10-progress-save-timer.md):
// capture-at-turn, dirty-gated 60s flush, change detection in memory. The
// visibleTextOffset participates in change detection (design decision log) and
// a failed write must not drop a newer capture.

namespace {

ProgressFlush::Record rec(uint16_t spine, uint16_t page, uint16_t count, uint32_t offset = 0, bool has = false) {
  ProgressFlush::Record r;
  r.spineIndex = spine;
  r.pageNumber = page;
  r.pageCount = count;
  r.visibleTextOffset = offset;
  r.hasOffset = has;
  return r;
}

}  // namespace

TEST(ProgressFlushTest, FirstCaptureIsDirty) {
  ProgressFlush::FlushState s;
  EXPECT_TRUE(s.capture(rec(0, 0, 10)));
  EXPECT_TRUE(s.shouldFlush());
  EXPECT_FALSE(s.hasFlushed());
}

TEST(ProgressFlushTest, SamePageCaptureIsNoOp) {
  ProgressFlush::FlushState s;
  s.capture(rec(0, 0, 10));
  ProgressFlush::Record written;
  EXPECT_TRUE(s.beginFlush(written));
  s.endFlush(written, true);
  EXPECT_TRUE(s.hasFlushed());
  EXPECT_FALSE(s.shouldFlush());

  // Reader parked on the same page: no new flush owed.
  EXPECT_FALSE(s.capture(rec(0, 0, 10)));
  EXPECT_FALSE(s.shouldFlush());
}

TEST(ProgressFlushTest, OffsetChangeCountsAsProgress) {
  ProgressFlush::FlushState s;
  s.capture(rec(0, 5, 10, 100, true));
  ProgressFlush::Record written;
  EXPECT_TRUE(s.beginFlush(written));
  s.endFlush(written, true);

  // Same spine/page/count but a shifted visible offset (same-page re-layout):
  // the old spine/page/count-only guard skipped this save; the design fixed it.
  EXPECT_TRUE(s.capture(rec(0, 5, 10, 260, true)));
  EXPECT_TRUE(s.shouldFlush());
}

TEST(ProgressFlushTest, PendingCaptureThenFlushWritesPending) {
  ProgressFlush::FlushState s;
  s.capture(rec(0, 3, 10));
  s.capture(rec(0, 4, 10));  // newer capture supersedes
  ProgressFlush::Record written;
  EXPECT_TRUE(s.beginFlush(written));
  EXPECT_EQ(written.pageNumber, 4);
  s.endFlush(written, true);
  EXPECT_EQ(s.lastFlushed().pageNumber, 4);
  EXPECT_FALSE(s.shouldFlush());
}

TEST(ProgressFlushTest, FailedFlushRetriesNextTick) {
  ProgressFlush::FlushState s;
  s.capture(rec(1, 2, 10));
  ProgressFlush::Record written;
  EXPECT_TRUE(s.beginFlush(written));
  s.endFlush(written, false);
  EXPECT_TRUE(s.shouldFlush());
  EXPECT_TRUE(s.beginFlush(written));  // retry carries the same record
  EXPECT_EQ(written.spineIndex, 1);
}

TEST(ProgressFlushTest, NewerCaptureDuringFailedWriteOwnsRetry) {
  ProgressFlush::FlushState s;
  s.capture(rec(1, 2, 10));
  ProgressFlush::Record written;
  EXPECT_TRUE(s.beginFlush(written));
  EXPECT_TRUE(s.capture(rec(1, 3, 10)));  // reader turned a page mid-write
  s.endFlush(written, false);             // the write failed
  ProgressFlush::Record out;
  EXPECT_TRUE(s.beginFlush(out));
  EXPECT_EQ(out.pageNumber, 3);           // retry writes the NEWER position
}

TEST(ProgressFlushTest, SuccessfulWriteBeatsMidWriteCapture) {
  ProgressFlush::FlushState s;
  s.capture(rec(1, 2, 10));
  ProgressFlush::Record written;
  EXPECT_TRUE(s.beginFlush(written));
  s.capture(rec(1, 3, 10));               // newer capture mid-write
  s.endFlush(written, true);              // the older record did land
  EXPECT_EQ(s.lastFlushed().pageNumber, 2);
  EXPECT_TRUE(s.shouldFlush());           // the newer one still owes a write
}

TEST(ProgressFlushTest, MarkFlushedClearsMatchingPendingOnly) {
  ProgressFlush::FlushState s;
  s.capture(rec(1, 2, 10));
  s.markFlushed(rec(1, 2, 10));  // external sync save wrote exactly this
  EXPECT_FALSE(s.shouldFlush());
  EXPECT_EQ(s.lastFlushed().pageNumber, 2);

  // A newer capture after the external save still owes a write.
  s.capture(rec(1, 3, 10));
  s.markFlushed(rec(1, 2, 10));  // stale external save must not clear it
  EXPECT_TRUE(s.shouldFlush());
}

TEST(ProgressFlushTest, AfterFlushEqualCaptureIsNoOp) {
  ProgressFlush::FlushState s;
  s.capture(rec(2, 7, 10, 55, true));
  ProgressFlush::Record written;
  EXPECT_TRUE(s.beginFlush(written));
  s.endFlush(written, true);

  EXPECT_FALSE(s.capture(rec(2, 7, 10, 55, true)));
  EXPECT_FALSE(s.shouldFlush());
}
