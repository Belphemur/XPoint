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

// Prefetch window: how many spines AHEAD of the entered chapter the worker
// indexes. 1 = next chapter only — whole-book prefetching measured as
// device-soak pathology (SD-write pressure through long background runs, and
// a task-watchdog abort on a 177-spine book) and burns battery/SD wear
// indexing chapters the user may never open. The window is (re)planned when
// the position trigger below fires for a new spine.
constexpr uint16_t kPrefetchLookaheadSpines = 1;

// Position trigger (owner directive, 2026-09-19): the next chapter is
// enqueued only once the reader has consumed all but the last
// ceil(pageCount * kPrefetchRemainingPercent / 100) pages of the CURRENT
// chapter — building chapters that may never be read wastes SD writes and
// battery. page is the 0-based current page inside the chapter; the ceil
// keeps the 1-page-remaining edge firing even for tiny page counts.
// pageCount == 0 (unknown) never triggers.
constexpr uint8_t kPrefetchRemainingPercent = 10;
inline bool shouldPrefetchNext(const uint16_t page, const uint16_t pageCount) {
  if (pageCount == 0) return false;
  const uint32_t thresholdPages = (static_cast<uint32_t>(pageCount) * kPrefetchRemainingPercent + 99) / 100;
  const uint32_t remaining = pageCount > page ? static_cast<uint32_t>(pageCount - page) : 0;
  return remaining <= thresholdPages;
}

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
