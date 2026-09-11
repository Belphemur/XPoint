// Host-test stub of HalFile — backed by HalStorage's in-memory file map.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class HalStorage;

class HalFile {
 public:
  // Wired by HalStorage::openFileFor{Read,Write} to the fake map entry.
  std::string* data = nullptr;
  size_t cursor = 0;

  bool isOpen() const { return open_; }
  // Used by the HalStorage stub after a successful open.
  void markOpen(bool o) {
    open_ = o;
    cursor = 0;
  }
  uint64_t fileSize64() { return data ? data->size() : 0; }
  size_t position() const { return cursor; }
  bool seek(size_t pos) { return seek64(pos); }
  bool seek64(uint64_t pos) {
    if (!data || pos > data->size()) return false;
    cursor = static_cast<size_t>(pos);
    return true;
  }
  int read(void* buf, size_t count) {
    if (!data) return 0;
    const size_t n = (cursor + count <= data->size()) ? count : (data->size() > cursor ? data->size() - cursor : 0);
    for (size_t i = 0; i < n; ++i) static_cast<char*>(buf)[i] = (*data)[cursor + i];
    cursor += n;
    return static_cast<int>(n);
  }
  int read() { return -1; }
  size_t write(const void* buf, size_t count) {
    if (testFailWrite) return 0;
    if (!data) return 0;
    const auto* b = static_cast<const char*>(buf);
    if (cursor + count > data->size()) data->resize(cursor + count);
    for (size_t i = 0; i < count; ++i) (*data)[cursor + i] = b[i];
    cursor += count;
    return count;
  }
  size_t write(uint8_t b) { return write(&b, 1); }
  bool sync() { return open_; }
  bool close() {
    if (!open_) return false;
    if (testFailClose) return false;  // injection point: close stays open
    open_ = false;
    return true;
  }

  // Test control (defined in Stubs.cpp).
  static bool testFailClose;
  static bool testFailWrite;

 private:
  bool open_ = false;
};