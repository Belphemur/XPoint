#pragma once

// FibpPrefetchPolicy — pure scheduling/stop policy for the FIBP prefetch
// worker (R3/R4). Header-only so host tests exercise it without device
// stubs; the worker class consumes it unchanged.

#if defined(CROSSPOINT_TTF_READER)

#include <stdint.h>

namespace freeink {
namespace book {
namespace fibp {

// "No chapter entered yet" sentinel for the queue planner.
constexpr uint16_t kNoChapter = 0xFFFF;

// R4 queue order: the chapter AFTER the entered one first, then onward
// (wrapping to the book start, ending at the entered chapter). With no
// chapter entered yet, the spine order is 0..spineCount-1. Writes at most
// `cap` indices into `out` and returns the count written.
inline uint16_t buildQueue(const uint16_t spineCount, const uint16_t notifiedSpine, uint16_t* out, const uint16_t cap) {
  if (out == nullptr || cap == 0 || spineCount == 0) return 0;
  const uint16_t start =
      (notifiedSpine == kNoChapter || notifiedSpine >= spineCount) ? 0 : static_cast<uint16_t>(notifiedSpine + 1);
  uint16_t n = 0;
  for (uint16_t i = 0; i < spineCount && n < cap; ++i) {
    out[n++] = static_cast<uint16_t>((start + i) % spineCount);
  }
  return n;
}

// R3 yield-gate between build chunks: the chunk loop stops when the book is
// closing (cancelled) or the layout generation moved (settings change).
inline bool shouldStopChunk(const bool cancelled, const uint32_t currentGen, const uint32_t sessionGen) {
  return cancelled || currentGen != sessionGen;
}

}  // namespace fibp
}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER
