// Host-test stub of HalStorage.h — in-memory file map with write/rename/
// remove, so ProgressFile::writeAtomic and the load path run for real.
// Host-only: this stub runs on the test host, so the firmware PSRAM policy
// (Rule 19) does not apply to its std::string-backed file map.
#pragma once

#include <map>
#include <string>

#include "HalFile.h"

using String = std::string;

class HalStorage {
 public:
  static HalStorage& getInstance() { return instance; }

  bool openFileForRead(const char*, const char* path, HalFile& file) {
    const auto it = files.find(path);
    if (it == files.end()) return false;
    // Keep the string alive even if the map entry is erased or renamed while
    // the handle is open: HalFile owns a copy. Read handles never sync back.
    file.openCopy(std::string(it->second), this, path, /*writable=*/false);
    return true;
  }
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
  }
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
    (void)moduleName;
    // Test control: transient write failure (final file stays untouched).
    if (failWriteCount > 0) {
      --failWriteCount;
      return false;
    }
    file.openCopy(std::string(), this, path, /*writable=*/true);  // truncate like SdFat's open-for-write
    return true;
  }
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
    return openFileForWrite(moduleName, path.c_str(), file);
  }
  bool exists(const char* path) { return path != nullptr && files.count(path) != 0; }
  bool remove(const char* path) { return path != nullptr && files.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (from == nullptr || to == nullptr) return false;
    const auto it = files.find(from);
    if (it == files.end()) return false;
    files[to] = std::move(it->second);
    files.erase(from);
    return true;
  }
  bool saveToPath(const std::string& path, const std::string& bytes) {
    files[path] = bytes;
    return true;
  }

  // Test control: path -> bytes.
  std::map<std::string, std::string> files;
  // Open-for-write failure counter (transient SD error simulation).
  int failWriteCount = 0;

 private:
  HalStorage() = default;
  static HalStorage instance;
};

#define Storage HalStorage::getInstance()

inline bool HalFile::close() {
  if (!open_) return false;
  open_ = false;
  if (storage_ != nullptr && writable_) return storage_->saveToPath(path_, data_);
  return true;
}
