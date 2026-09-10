// Host-test stub of HalStorage.h — enough surface for BookFontLoader tests.
// The fake exposes a controllable file map so tests can exercise the real
// discovery/read paths (unlike a hard-failing stub).
#pragma once

#include <map>
#include <string>

#include "HalFile.h"

// Minimal String stand-in (HalStorage's openFileForRead overload set uses it).
using String = std::string;

class HalStorage {
 public:
  static HalStorage& getInstance() { return instance; }

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
  }

  // Test control: populate with path -> bytes. Empty map = all opens fail.
  std::map<std::string, std::string> files;

 private:
  HalStorage() = default;
  static HalStorage instance;
};

#define Storage HalStorage::getInstance()