// Host-test stub of HalFile — backed by HalStorage's controllable file map.
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
  HalStorage* storage = nullptr;

  bool isOpen() const { return data != nullptr || dir; }
  operator bool() const { return isOpen(); }
  void close() {
    data = nullptr;
    dir = false;
  }

  size_t fileSize() { return data ? data->size() : 0; }
  uint64_t fileSize64() { return data ? data->size() : 0; }
  int read(void* buf, size_t count) {
    if (!data) return 0;
    size_t n = count < data->size() ? count : data->size();
    for (size_t i = 0; i < n; ++i) static_cast<char*>(buf)[i] = (*data)[i];
    return static_cast<int>(n);
  }
  int read() { return -1; }

  bool isDirectory() const { return dir; }
  size_t getName(char* name, size_t len);
  // Defined in Stubs.cpp: enumerates the immediate children of the directory.
  HalFile openNextFile();

  // openNextFile() iteration cursor (last returned child path; "" = start).
  std::string nextFrom;
};