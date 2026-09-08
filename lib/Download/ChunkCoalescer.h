#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace download {

// Pure, host-testable chunk-coalescing sink. Accumulates incoming data
// fragments into a fixed-capacity buffer and flushes full blocks to a
// caller-supplied write callback. Designed so that HttpDownloader's
// write-coalescing can be unit-tested without any ESP32 dependencies.
//
// Usage:
//   ChunkCoalescer coalescer(8192);
//   coalescer.write(data, len, [](const uint8_t* buf, size_t n) { ... });
//   coalescer.flush([](const uint8_t* buf, size_t n) { ... });
class ChunkCoalescer {
 public:
  // `capacity` is the coalescing buffer size. Pass 0 for passthrough mode:
  // every write is forwarded to the callback immediately, no buffering.
  explicit ChunkCoalescer(size_t capacity)
      : cap_(capacity), len_(0), buf_(cap_ > 0 ? std::make_unique<uint8_t[]>(cap_) : nullptr) {}

  ChunkCoalescer(const ChunkCoalescer&) = delete;
  ChunkCoalescer& operator=(const ChunkCoalescer&) = delete;

  // Returns the configured buffer capacity, or 0 in passthrough mode.
  size_t capacity() const { return cap_; }

  // Feed `len` bytes from `data` into the coalescer. Each full buffer is
  // flushed via `onFlush`. If `onFlush` returns false, propagation stops and
  // `write` returns false. Returns true when all data was accepted.
  //
  // The same callback may be called multiple times if data spans multiple
  // buffer fills. The callback must consume the buffer contents before
  // returning — the data is not retained after the call.
  bool write(const uint8_t* data, size_t len, bool (*onFlush)(const uint8_t*, size_t, void*), void* ctx) {
    if (cap_ == 0) {
      // Passthrough: forward directly, no buffering.
      return onFlush(data, len, ctx);
    }
    size_t off = 0;
    while (off < len) {
      const size_t space = cap_ - len_;
      const size_t take = space < (len - off) ? space : (len - off);
      if (take > 0) {
        std::memcpy(buf_.get() + len_, data + off, take);
        len_ += take;
        off += take;
      }
      if (len_ == cap_) {
        if (!onFlush(buf_.get(), cap_, ctx)) return false;
        len_ = 0;
      }
    }
    return true;
  }

  // Flush any remaining buffered bytes via `onFlush`. Returns false if the
  // callback returns false, true if the buffer was empty or the flush
  // succeeded.
  bool flush(bool (*onFlush)(const uint8_t*, size_t, void*), void* ctx) {
    if (len_ == 0) return true;
    if (!onFlush(buf_.get(), len_, ctx)) return false;
    len_ = 0;
    return true;
  }

  // Number of bytes currently buffered (not yet flushed). Primarily for tests.
  size_t pending() const { return len_; }

 private:
  const size_t cap_;
  size_t len_;
  std::unique_ptr<uint8_t[]> buf_;
};

}  // namespace download
