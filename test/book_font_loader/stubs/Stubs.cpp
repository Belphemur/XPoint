// Host-test stub definitions for BookFontLoader's HAL/Arduino dependencies.
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
  return true;
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