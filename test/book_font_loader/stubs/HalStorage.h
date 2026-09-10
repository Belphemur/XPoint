// Host-test stub of HalStorage.h — enough surface for BookFontLoader tests.
// Mirrors the real class shape; the singleton never opens files in tests.
#pragma once

#include "HalFile.h"

// Minimal String stand-in (HalStorage's openFileForRead overload set uses it).
#include <string>
using String = std::string;

class HalStorage {
 public:
  static HalStorage& getInstance() { return instance; }

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
  }

 private:
  HalStorage() = default;
  static HalStorage instance;
};

#define Storage HalStorage::getInstance()
