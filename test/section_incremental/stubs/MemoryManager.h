#pragma once

// Host-test stub for the SDK MemoryManager. The parser/layout path calls
// ensureFree() to evict rebuildable caches before large allocations; on the
// host there are no caches and no pressure, so it is a no-op that reports
// success. Matches only the surface the layout code uses.

#include <cstddef>

namespace freeink {

enum class MemPool : unsigned char { Internal, Psram, Default };

class MemoryManager {
 public:
  static MemoryManager& instance() {
    static MemoryManager inst;
    return inst;
  }
  // Always "succeeds": the host has nothing to evict and nothing to free.
  bool ensureFree(size_t /*bytes*/, MemPool /*pool*/ = MemPool::Default) { return true; }

 private:
  MemoryManager() = default;
};

}  // namespace freeink
