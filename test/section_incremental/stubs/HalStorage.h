#pragma once

// Host-side HalStorage shim over <cstdio> for the section-incremental test.
// Mirrors the device API surface used by Epub.cpp / Section.cpp /
// BookMetadataCache.cpp / ZipFile.cpp / CssParser.cpp: open-for-read/write,
// exists/remove/rename, recursive mkdir, removeDir — plus a HalFile that
// satisfies both the raw handle methods (seek/position/size/read/write) and
// the Print interface (Section streams EPUB items through Print&).

#include <Arduino.h>
#include <Print.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

class HalFile : public Print {
 public:
  HalFile() = default;
  ~HalFile() override { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool open(const char* path, const char* mode) {
    close();
    file_ = std::fopen(path, mode);
    return file_ != nullptr;
  }

  // Device semantics: buffer read returns bytes read, single-byte read
  // returns a negative int on error (ZipFile relies on this).
  int read(void* buffer, size_t count) {
    if (!file_) return -1;
    return static_cast<int>(std::fread(buffer, 1, count, file_));
  }
  int read() { return file_ ? std::fgetc(file_) : -1; }

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* buffer, size_t count) override {
    return file_ ? std::fwrite(buffer, 1, count, file_) : 0;
  }
  size_t write(const void* buffer, size_t count) { return write(static_cast<const uint8_t*>(buffer), count); }

  bool flush() { return file_ && std::fflush(file_) == 0; }
  bool seek(size_t pos) { return file_ && std::fseek(file_, static_cast<long>(pos), SEEK_SET) == 0; }
  bool seekSet(size_t pos) { return seek(pos); }
  bool seekCur(int64_t offset) { return file_ && std::fseek(file_, static_cast<long>(offset), SEEK_CUR) == 0; }
  bool close() {
    if (!file_) return false;
    const bool ok = std::fclose(file_) == 0;
    file_ = nullptr;
    return ok;
  }

  bool isOpen() const { return file_ != nullptr; }
  explicit operator bool() const { return isOpen(); }

  int available() const { return static_cast<int>(size() - position()); }
  size_t position() const { return file_ ? static_cast<size_t>(std::ftell(file_)) : 0; }
  size_t size() const {
    if (!file_) return 0;
    const long offset = std::ftell(file_);
    std::fseek(file_, 0, SEEK_END);
    const long end = std::ftell(file_);
    std::fseek(file_, offset, SEEK_SET);
    return end > 0 ? static_cast<size_t>(end) : 0;
  }
  size_t fileSize() const { return size(); }

 private:
  std::FILE* file_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool openFileForRead(const char*, const char* path, HalFile& file) { return file.open(path, "rb"); }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "rb"); }
  bool openFileForWrite(const char*, const char* path, HalFile& file) { return file.open(path, "wb"); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "wb"); }

  bool exists(const char* path) const {
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
  }
  bool exists(const std::string& path) const { return exists(path.c_str()); }

  bool remove(const char* path) { return std::remove(path) == 0; }
  bool remove(const std::string& path) { return remove(path.c_str()); }

  bool rename(const char* from, const char* to) { return std::rename(from, to) == 0; }

  // Device pFlag=true semantics: create intermediate directories, idempotent.
  bool mkdir(const char* path, bool = true) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec || std::filesystem::is_directory(path);
  }
  bool removeDir(const char* path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return !ec;
  }
};

#define Storage HalStorage::getInstance()
