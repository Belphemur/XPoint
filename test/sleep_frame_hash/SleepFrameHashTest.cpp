#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "SleepFrameHash.h"

namespace {

// Naive per-byte reference implementation (RFC 1950 definition) used to
// cross-check the deferred-modulo fast path.
uint32_t adler32Naive(const uint8_t* data, size_t len) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (size_t i = 0; i < len; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

}  // namespace

TEST(SleepFrameHash, EmptyInput) {
  uint8_t dummy;
  EXPECT_EQ(sleepFrameAdler32(&dummy, 0), 1u);
}

TEST(SleepFrameHash, KnownVectors) {
  // RFC 1950 / zlib reference values.
  const uint8_t abc[] = {'a', 'b', 'c'};
  EXPECT_EQ(sleepFrameAdler32(abc, sizeof(abc)), 0x024D0127u);

  const uint8_t wiki[] = {'W', 'i', 'k', 'i', 'p', 'e', 'd', 'i', 'a'};
  EXPECT_EQ(sleepFrameAdler32(wiki, sizeof(wiki)), 0x11E60398u);
}

TEST(SleepFrameHash, MatchesNaiveAcrossNmaxBoundary) {
  // Exercise multiple 5552-byte blocks plus partials, matching the naive
  // per-byte implementation.
  std::vector<uint8_t> buf(2 * 5552 + 777);
  uint32_t x = 0x12345678;
  for (auto& byte : buf) {
    x = x * 1664525u + 1013904223u;
    byte = static_cast<uint8_t>(x >> 24);
  }
  EXPECT_EQ(sleepFrameAdler32(buf.data(), buf.size()), adler32Naive(buf.data(), buf.size()));
}

TEST(SleepFrameHash, DetectsSingleByteChange) {
  std::vector<uint8_t> buf(1000, 0x41);
  const uint32_t before = sleepFrameAdler32(buf.data(), buf.size());
  buf[999] ^= 0x01;
  const uint32_t after = sleepFrameAdler32(buf.data(), buf.size());
  EXPECT_NE(before, after);
}
