#include <ChunkCoalescer.h>
#include <Memory.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// Records each flush as a vector of bytes.
struct FlushRecorder {
  std::vector<std::vector<uint8_t>> chunks;

  static bool onFlush(const uint8_t* data, size_t len, void* ctx) {
    auto* self = static_cast<FlushRecorder*>(ctx);
    self->chunks.emplace_back(data, data + len);
    return true;
  }

  std::vector<uint8_t> concatenated() const {
    std::vector<uint8_t> result;
    for (const auto& chunk : chunks) {
      result.insert(result.end(), chunk.begin(), chunk.end());
    }
    return result;
  }
};

}  // namespace

TEST(ChunkCoalescerTest, PassthroughModeForwardsEachWriteImmediately) {
  download::ChunkCoalescer coalescer(nullptr, 0);
  FlushRecorder rec;
  const uint8_t data[] = {1, 2, 3, 4, 5};
  EXPECT_TRUE(coalescer.write(data, sizeof(data), FlushRecorder::onFlush, &rec));
  EXPECT_TRUE(coalescer.flush(FlushRecorder::onFlush, &rec));
  ASSERT_EQ(rec.chunks.size(), 1u);
  EXPECT_EQ(rec.chunks[0].size(), 5u);
  EXPECT_EQ(rec.chunks[0], (std::vector<uint8_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(coalescer.pending(), 0u);
}

TEST(ChunkCoalescerTest, SmallWritesAreBufferedAndFlushedWhenFull) {
  constexpr size_t CAP = 8;
  download::ChunkCoalescer coalescer(poolMakeBytes(CAP), CAP);
  FlushRecorder rec;
  const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};

  // 5 bytes — buffer not full, no flush yet
  EXPECT_TRUE(coalescer.write(data, 5, FlushRecorder::onFlush, &rec));
  EXPECT_EQ(rec.chunks.size(), 0u);
  EXPECT_EQ(coalescer.pending(), 5u);

  // 3 more bytes (total 8 = CAP) — should trigger one flush
  EXPECT_TRUE(coalescer.write(data, 3, FlushRecorder::onFlush, &rec));
  ASSERT_EQ(rec.chunks.size(), 1u);
  EXPECT_EQ(rec.chunks[0].size(), CAP);
  EXPECT_EQ(coalescer.pending(), 0u);
}

TEST(ChunkCoalescerTest, LargeWriteSpansMultipleBufferFills) {
  constexpr size_t CAP = 4;
  download::ChunkCoalescer coalescer(poolMakeBytes(CAP), CAP);
  FlushRecorder rec;
  const uint8_t data[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};

  // 10 bytes into a 4-byte buffer = 2 full flushes (8 bytes) + 2 pending
  EXPECT_TRUE(coalescer.write(data, 10, FlushRecorder::onFlush, &rec));
  ASSERT_EQ(rec.chunks.size(), 2u);
  EXPECT_EQ(rec.chunks[0], (std::vector<uint8_t>{10, 20, 30, 40}));
  EXPECT_EQ(rec.chunks[1], (std::vector<uint8_t>{50, 60, 70, 80}));
  EXPECT_EQ(coalescer.pending(), 2u);

  // Flush the remainder
  EXPECT_TRUE(coalescer.flush(FlushRecorder::onFlush, &rec));
  ASSERT_EQ(rec.chunks.size(), 3u);
  EXPECT_EQ(rec.chunks[2], (std::vector<uint8_t>{90, 100}));
  EXPECT_EQ(coalescer.pending(), 0u);
}

TEST(ChunkCoalescerTest, EmptyWriteIsNoOp) {
  download::ChunkCoalescer coalescer(poolMakeBytes(8), 8);
  FlushRecorder rec;
  EXPECT_TRUE(coalescer.write(nullptr, 0, FlushRecorder::onFlush, &rec));
  EXPECT_EQ(rec.chunks.size(), 0u);
  EXPECT_EQ(coalescer.pending(), 0u);
}

TEST(ChunkCoalescerTest, ExactMultipleProducesFullFlushesOnly) {
  constexpr size_t CAP = 4;
  download::ChunkCoalescer coalescer(poolMakeBytes(CAP), CAP);
  FlushRecorder rec;
  const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8};

  EXPECT_TRUE(coalescer.write(data, 8, FlushRecorder::onFlush, &rec));
  ASSERT_EQ(rec.chunks.size(), 2u);
  // No pending data after exactly-cap multiples
  EXPECT_TRUE(coalescer.flush(FlushRecorder::onFlush, &rec));
  EXPECT_EQ(rec.chunks.size(), 2u);  // flush should be a no-op
}

TEST(ChunkCoalescerTest, ConcatenatedOutputPreservesInputOrder) {
  constexpr size_t CAP = 8;
  download::ChunkCoalescer coalescer(poolMakeBytes(CAP), CAP);
  FlushRecorder rec;
  const uint8_t data[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};

  EXPECT_TRUE(coalescer.write(data, 18, FlushRecorder::onFlush, &rec));
  EXPECT_TRUE(coalescer.flush(FlushRecorder::onFlush, &rec));

  std::vector<uint8_t> expected(data, data + 18);
  EXPECT_EQ(rec.concatenated(), expected);
}

TEST(ChunkCoalescerTest, PassthroughEmptyFlushIsNoOp) {
  download::ChunkCoalescer coalescer(nullptr, 0);
  FlushRecorder rec;
  EXPECT_TRUE(coalescer.flush(FlushRecorder::onFlush, &rec));
  EXPECT_EQ(rec.chunks.size(), 0u);
}

TEST(ChunkCoalescerTest, BufferFlushFailurePropagates) {
  constexpr size_t CAP = 4;
  download::ChunkCoalescer coalescer(poolMakeBytes(CAP), CAP);

  auto failFlush = [](const uint8_t*, size_t, void*) -> bool { return false; };

  // Fill the buffer — onFlush returns false, write should abort
  const uint8_t data[] = {1, 2, 3, 4};
  EXPECT_FALSE(coalescer.write(data, 4, failFlush, nullptr));
}

TEST(ChunkCoalescerTest, FinalFlushFailureWithPendingBytesPropagates) {
  constexpr size_t CAP = 4;
  download::ChunkCoalescer coalescer(poolMakeBytes(CAP), CAP);
  const uint8_t data[] = {1, 2, 3};

  // Buffer a partial block, then the final flush fails
  auto failFlush = [](const uint8_t*, size_t, void*) -> bool { return false; };
  EXPECT_TRUE(coalescer.write(data, sizeof(data), failFlush, nullptr));
  EXPECT_EQ(coalescer.pending(), 3u);
  EXPECT_FALSE(coalescer.flush(failFlush, nullptr));
}

TEST(ChunkCoalescerTest, PassthroughWriteFailurePropagates) {
  download::ChunkCoalescer coalescer(nullptr, 0);

  auto failFlush = [](const uint8_t*, size_t, void*) -> bool { return false; };
  const uint8_t data[] = {1, 2, 3};
  EXPECT_FALSE(coalescer.write(data, sizeof(data), failFlush, nullptr));
}

TEST(ChunkCoalescerTest, NullBufferWithCapacityPassesThrough) {
  // OOM fallback on firmware: null buffer with a nonzero capacity must
  // degrade to passthrough, never dereference the null buffer.
  download::ChunkCoalescer coalescer(PoolBytes{}, 8);
  FlushRecorder rec;
  const uint8_t data[] = {1, 2, 3, 4, 5};
  EXPECT_TRUE(coalescer.write(data, sizeof(data), FlushRecorder::onFlush, &rec));
  EXPECT_TRUE(coalescer.flush(FlushRecorder::onFlush, &rec));
  ASSERT_EQ(rec.chunks.size(), 1u);
  EXPECT_EQ(rec.chunks[0], (std::vector<uint8_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(coalescer.pending(), 0u);
}
