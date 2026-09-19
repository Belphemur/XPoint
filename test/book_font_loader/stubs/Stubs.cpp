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

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  file = HalFile{};
  auto& bytes = files[path];  // create-or-truncate (production O_TRUNC semantics)
  bytes.clear();              // existing content must not leak into the new write
  file.data = &bytes;
  file.path = &files.find(path)->first;
  file.writable = true;
  file.storage = this;
  return true;
}

bool HalStorage::remove(const char* path) {
  files.erase(path);
  mtimes.erase(path);
  return true;
}

bool HalStorage::rename(const char* oldPath, const char* newPath) {
  if (files.count(oldPath) == 0 || files.count(newPath) != 0) return false;  // SdFat refuses existing dest
  files[newPath] = files[oldPath];
  files.erase(oldPath);
  return true;
}

size_t HalFile::write(const void* buf, size_t count) {
  if (!writable || path == nullptr || storage == nullptr) return 0;
  std::string& dst = storage->files[*path];
  dst.append(static_cast<const char*>(buf), count);
  return count;
}

uint32_t HalFile::modificationTime() {
  if (path == nullptr || storage == nullptr) return 0;
  auto it = storage->mtimes.find(*path);
  return it == storage->mtimes.end() ? 0u : it->second;
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
  if (path == nullptr || len == 0) {
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
  // Cursor lives on the PARENT directory handle (mirrors SdFat's per-handle
  // iteration state): advance it past every returned entry or a directory
  // containing one immediate child would loop forever. Files and subdirs
  // merge into one enumeration ordered by path so a root-level file cannot
  // shadow subdirectories that sort before it.
  const std::string prefix = *path + "/";
  const auto isChild = [&prefix](const std::string& p) {
    return p.compare(0, prefix.size(), prefix) == 0 && p.find('/', prefix.size()) == std::string::npos;
  };
  const std::string* bestFile = nullptr;
  for (const auto& [p, bytes] : storage->files) {
    (void)bytes;
    if (p > nextFrom && isChild(p)) {
      bestFile = &p;
      break;
    }
  }
  const std::string* bestDir = nullptr;
  for (const auto& d : storage->dirs) {
    if (d.size() > prefix.size() && d > nextFrom && isChild(d)) {
      bestDir = &d;
      break;
    }
  }
  const std::string* best = nullptr;
  bool bestIsDir = false;
  if (bestFile != nullptr && bestDir != nullptr) {
    bestIsDir = *bestDir < *bestFile;
    best = bestIsDir ? bestDir : bestFile;
  } else if (bestFile != nullptr) {
    best = bestFile;
  } else if (bestDir != nullptr) {
    best = bestDir;
    bestIsDir = true;
  }
  if (best == nullptr) return child;
  nextFrom = *best;
  if (!bestIsDir) {
    auto it = storage->files.find(*best);
    child.data = &it->second;
    child.path = &it->first;
  } else {
    child.dir = true;
    child.path = &*storage->dirs.find(*best);
    child.storage = storage;
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