// Host-test stub of HalFile — backed by HalStorage's controllable file map.
// Mirrors the device HAL contract: close() on a never-opened handle is a
// no-op (the device HalStorage.cpp close() tolerates null impl); every other
// method on an unopened handle is misuse. closeOfNeverOpenedCounter lets a
// lifecycle test verify zero misuse without paying for a crash-safety
// regression through an abort.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class HalStorage;

class HalFile {
 public:
  // Wired by HalStorage::openFileForRead/open to the fake map entry.
  const std::string* data = nullptr;
  // Full path of the handle (map key); getName() reports the last component.
  const std::string* path = nullptr;
  // Directory handle state (scanFonts two-level walk).
  bool dir = false;
  // Set by openFileForWrite(): write() appends into the fake map entry.
  bool writable = false;
  HalStorage* storage = nullptr;
  // True once this instance went through any HalStorage open call — mirrors
  // the device's impl != nullptr lifetime (an open call allocates the Impl
  // even when the open itself fails). close() clears the live state but not
  // this flag, matching the device: close-on-closed stays a safe no-op.
  bool everOpened = false;
  // Counts close() calls on a never-opened handle (0 in well-behaved code).
  static int closeOfNeverOpenedCounter;

  bool isOpen() const { return data != nullptr || dir; }
  operator bool() const { return isOpen(); }
  void close() {
    if (!everOpened) ++closeOfNeverOpenedCounter;
    data = nullptr;
    dir = false;
    writable = false;
    path = nullptr;
  }

  size_t fileSize() { return data ? data->size() : 0; }
  uint64_t fileSize64() { return data ? data->size() : 0; }
  // Read cursor: seek() sets it, read() consumes from it and advances.
  // Default 0 keeps the historical read-from-start behavior for callers
  // that never seek.
  size_t cursor = 0;

  bool seek(size_t pos) {
    if (!data || pos > data->size()) return false;
    cursor = pos;
    return true;
  }
  int read(void* buf, size_t count) {
    if (!data) return 0;
    if (cursor >= data->size()) return 0;
    size_t n = count < data->size() - cursor ? count : data->size() - cursor;
    for (size_t i = 0; i < n; ++i) static_cast<char*>(buf)[i] = (*data)[cursor + i];
    cursor += n;
    return static_cast<int>(n);
  }
  int read() { return -1; }
  size_t write(const void* buf, size_t count);  // defined in Stubs.cpp
  uint32_t modificationTime();                  // defined in Stubs.cpp

  bool isDirectory() const { return dir; }
  size_t getName(char* name, size_t len);
  // Defined in Stubs.cpp: enumerates the immediate children of the directory.
  HalFile openNextFile();

  // openNextFile() iteration cursor (last returned child path; "" = start).
  std::string nextFrom;
};
