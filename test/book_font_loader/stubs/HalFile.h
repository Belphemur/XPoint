// Host-test stub of HalFile — backed by HalStorage's controllable file map.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class HalStorage;

class HalFile {
 public:
  // Wired by HalStorage::openFileForRead to the fake map entry.
  const std::string* data = nullptr;

  size_t fileSize() { return data ? data->size() : 0; }
  uint64_t fileSize64() { return data ? data->size() : 0; }
  int read(void* buf, size_t count) {
    if (!data) return 0;
    size_t n = count < data->size() ? count : data->size();
    for (size_t i = 0; i < n; ++i) static_cast<char*>(buf)[i] = (*data)[i];
    return static_cast<int>(n);
  }
  int read() { return -1; }
};