// Host-test stub of HalFile — owns its bytes while open; only writable
// handles sync back on close, so the HalStorage stub can erase/rename map
// entries without dangling.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class HalStorage;

class HalFile {
 public:
  // Wired by HalStorage::openFileFor{Read,Write}; only a writable handle
  // syncs back on close — a read handle never persists its copy.
  void openCopy(std::string bytes, HalStorage* storage, std::string path, bool writable = false) {
    open_ = true;
    cursor = 0;
    data_ = std::move(bytes);
    storage_ = storage;
    path_ = std::move(path);
    writable_ = writable;
  }
  size_t cursor = 0;

  bool isOpen() const { return open_; }
  uint64_t fileSize64() const { return data_.size(); }
  size_t position() const { return cursor; }
  bool seek64(uint64_t pos) {
    if (!open_ || pos > data_.size()) return false;
    cursor = static_cast<size_t>(pos);
    return true;
  }
  int read(void* buf, size_t count) {
    if (!open_ || cursor >= data_.size()) return 0;
    // Subtraction form: no unsigned overflow before the bound decision.
    const size_t avail = data_.size() - cursor;
    const size_t n = count <= avail ? count : avail;
    for (size_t i = 0; i < n; ++i) static_cast<char*>(buf)[i] = data_[cursor + i];
    cursor += n;
    return static_cast<int>(n);
  }
  size_t write(const void* buf, size_t count) {
    if (!open_ || cursor > data_.size()) return 0;
    const auto* b = static_cast<const char*>(buf);
    // Subtraction form for the span check, plus a wrap guard so the resize
    // argument cannot overflow: cursor + count must be representable.
    if (count > SIZE_MAX - cursor) return 0;
    if (count > data_.size() - cursor) data_.resize(cursor + count);
    for (size_t i = 0; i < count; ++i) data_[cursor + i] = b[i];
    cursor += count;
    return count;
  }
  bool flush() { return isOpen(); }
  bool close();
  // Mirrors DESTRUCTOR_CLOSES_FILE=1: the firmware handle closes at scope
  // exit, and the stub syncs its owned bytes back to the map at that point.
  ~HalFile() { close(); }

 private:
  std::string data_;
  std::string path_;
  HalStorage* storage_ = nullptr;  // set only by the host stub on open
  bool open_ = false;
  bool writable_ = false;
};
