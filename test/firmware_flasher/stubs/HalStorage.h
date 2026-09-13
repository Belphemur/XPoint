#pragma once

#include <cstddef>
#include <map>
#include <string>

// Host-test stub of HalFile backed by HalStorage's in-memory file map. Only
// the operations FirmwareFlasher uses are implemented.
class HalFile {
 public:
  std::string* data = nullptr;
  size_t cursor = 0;

  bool isOpen() const { return open_; }
  explicit operator bool() const { return open_; }

  size_t fileSize() const { return data ? data->size() : 0; }
  bool seek(size_t pos) {
    if (!data || pos > data->size()) return false;
    cursor = pos;
    return true;
  }

  int read(void* buf, size_t count) {
    if (!data) return 0;
    const size_t n = (cursor + count <= data->size()) ? count : (data->size() > cursor ? data->size() - cursor : 0);
    for (size_t i = 0; i < n; ++i) static_cast<char*>(buf)[i] = (*data)[cursor + i];
    cursor += n;
    return static_cast<int>(n);
  }

  bool close() {
    open_ = false;
    return true;
  }

  void markOpen() { open_ = true; }

 private:
  bool open_ = false;
};

class HalStorage {
 public:
  static HalStorage& getInstance() { return instance; }

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file) {
    (void)moduleName;
    auto it = files.find(path);
    if (it == files.end()) return false;
    file.data = &it->second;
    file.cursor = 0;
    file.markOpen();
    return true;
  }

  std::map<std::string, std::string> files;

 private:
  HalStorage() = default;
  static HalStorage instance;
};

#define Storage HalStorage::getInstance()
