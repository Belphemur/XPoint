#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Host test shim for HalStorage/HalFile: an in-memory filesystem keyed by
// path, so the binary stats stores can be exercised without an SD card.
// Supports exactly the operations the stores use: openFileForRead/Write
// (O_TRUNC semantics), exists, remove, rename, fileSize, read, write,
// flush/sync/close (no-ops), position.

class HalFile {
 public:
  struct Blob {
    std::vector<uint8_t> bytes;
    size_t pos = 0;
  };

  HalFile() = default;
  explicit HalFile(Blob* blob) : blob_(blob) {}

  int read(void* buf, size_t count) {
    if (!blob_) return -1;
    const size_t available = blob_->bytes.size() > blob_->pos ? blob_->bytes.size() - blob_->pos : 0;
    const size_t n = count < available ? count : available;
    memcpy(buf, blob_->bytes.data() + blob_->pos, n);
    blob_->pos += n;
    return static_cast<int>(n);
  }

  size_t write(const void* buf, size_t count) {
    if (!blob_) return 0;
    if (blob_->pos + count > blob_->bytes.size()) {
      blob_->bytes.resize(blob_->pos + count);
    }
    memcpy(blob_->bytes.data() + blob_->pos, buf, count);
    blob_->pos += count;
    return count;
  }

  size_t fileSize() const { return blob_ ? blob_->bytes.size() : 0; }
  size_t position() const { return blob_ ? blob_->pos : 0; }
  bool flush() { return true; }
  bool sync() { return true; }
  bool close() { return true; }
  bool isOpen() const { return blob_ != nullptr; }
  operator bool() const { return isOpen(); }

 private:
  Blob* blob_ = nullptr;
};

class HalStorage {
 public:
  bool openFileForRead(const char* /*moduleName*/, const std::string& path, HalFile& file) {
    auto it = files_.find(path);
    if (it == files_.end()) return false;
    it->second.pos = 0;  // fresh handle starts at position 0
    file = HalFile(&it->second);
    return true;
  }

  bool openFileForWrite(const char* /*moduleName*/, const std::string& path, HalFile& file) {
    // O_TRUNC semantics, matching SDCardManager::openFileForWrite.
    HalFile::Blob& blob = files_[path];
    blob.bytes.clear();
    blob.pos = 0;
    file = HalFile(&blob);
    return true;
  }

  bool exists(const std::string& path) { return files_.count(path) != 0; }

  bool remove(const std::string& path) {
    const size_t n = files_.erase(path);
    lastRemoveMissing_ = (n == 0);
    return true;
  }

  bool rename(const std::string& from, const std::string& to) {
    auto it = files_.find(from);
    if (it == files_.end()) return false;
    if (files_.count(to) != 0) return false;  // FatFile::rename fails on existing destination
    files_[to] = it->second;
    files_.erase(it);
    return true;
  }

  // Test hooks.
  bool lastRemoveTargetWasMissing() const { return lastRemoveMissing_; }
  void clear() { files_.clear(); }

  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

 private:
  std::map<std::string, HalFile::Blob> files_;
  bool lastRemoveMissing_ = false;
};

#define Storage HalStorage::getInstance()
