// Host-test stub of the ESP Arduino runtime object (ESP.getFreeHeap etc.).
#pragma once

#include <cstddef>
#include <cstdint>

class EspClassStub {
 public:
  uint32_t getFreeHeap() { return 320 * 1024; }
  uint32_t getMaxAllocHeap() { return 320 * 1024; }
};

extern EspClassStub Esp;  // NOLINT — mirrors Arduino naming

// The real Arduino.h defines `ESP` as an instance; the stub translation unit
// defines it so BookFontLoader.cpp links unchanged.
extern EspClassStub ESP;