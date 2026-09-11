// Host-test stub definitions for BookFontLoader's HAL/Arduino dependencies.
#include <string>

#include "Arduino.h"
#include "HalMemory.h"
#include "HalStorage.h"

EspClassStub ESP;
EspClassStub Esp;

HalStorage HalStorage::instance;

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  auto it = files.find(path);
  if (it == files.end()) return false;  // missing file = failed open
  file.data = &it->second;
  file.path = &it->first;
  return true;
}

HalFile HalStorage::open(const char* path) {
  HalFile f;
  if (dirs.count(path) != 0) {
    f.dir = true;
    f.path = &*dirs.find(path);
    f.storage = this;
  } else {
    auto it = files.find(path);
    if (it != files.end()) {
      f.data = &it->second;
      f.path = &it->first;
    }
  }
  return f;
}

namespace {
std::string lastComponent(const std::string& p) {
  const size_t slash = p.rfind('/');
  return slash == std::string::npos ? p : p.substr(slash + 1);
}
}  // namespace

size_t HalFile::getName(char* name, size_t len) {
  if (path == nullptr) {
    if (len > 0) name[0] = '\0';
    return 0;
  }
  const std::string n = lastComponent(*path);
  snprintf(name, len, "%s", n.c_str());
  return n.size() < len ? n.size() : len - 1;
}

HalFile HalFile::openNextFile() {
  HalFile child;
  if (!dir || storage == nullptr || path == nullptr) return child;
  const std::string prefix = *path + "/";
  // First immediate child (file, then directory) after the cursor, in path
  // order. Map/set keys are stable pointers, safe to hand out.
  for (const auto& [p, bytes] : storage->files) {
    (void)bytes;
    if (p > nextFrom && p.compare(0, prefix.size(), prefix) == 0 && p.find('/', prefix.size()) == std::string::npos) {
      child.data = &storage->files.find(p)->second;
      child.path = &storage->files.find(p)->first;
      child.nextFrom = p;
      return child;
    }
  }
  for (const auto& d : storage->dirs) {
    if (d.size() > prefix.size() && d.compare(0, prefix.size(), prefix) == 0 &&
        d.find('/', prefix.size()) == std::string::npos && d > nextFrom) {
      child.dir = true;
      child.path = &*storage->dirs.find(d);
      child.storage = storage;
      child.nextFrom = d;
      return child;
    }
  }
  return child;
}

// Test control for the memory tiers: settable from tests via these hooks.
static HalMemory::HeapStats fakePsram{0, 0, 0, 0};
static HalMemory::HeapStats fakeInternal{320 * 1024, 320 * 1024, 0, 0};

HalMemory::HeapStats HalMemory::getPsramHeap() { return fakePsram; }
HalMemory::HeapStats HalMemory::getDefaultHeap() { return fakeInternal; }
HalMemory::HeapStats HalMemory::getInternalHeap() { return fakeInternal; }

void testSetPsramHeap(HalMemory::HeapStats s) { fakePsram = s; }
void testSetInternalHeap(HalMemory::HeapStats s) { fakeInternal = s; }
void testSetFreeHeap(uint32_t freeHeap, uint32_t maxAllocHeap) { ESP.setForTest(freeHeap, maxAllocHeap); }