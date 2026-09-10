// Host-test stub definitions for BookFontLoader's HAL/Arduino dependencies.
#include "Arduino.h"
#include "HalMemory.h"
#include "HalStorage.h"

EspClassStub ESP;
EspClassStub Esp;

HalStorage HalStorage::instance;

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  (void)path;
  (void)file;
  return false;  // no SD in host tests
}

// No PSRAM in host tests — forces the DRAM tier so budget logic is exercised.
HalMemory::HeapStats HalMemory::getPsramHeap() { return {0, 0, 0, 0}; }
HalMemory::HeapStats HalMemory::getDefaultHeap() { return {320 * 1024, 320 * 1024, 0, 0}; }
HalMemory::HeapStats HalMemory::getInternalHeap() { return {320 * 1024, 320 * 1024, 0, 0}; }