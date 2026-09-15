// Host-test stub of the ESP Arduino runtime object (ESP.getFreeHeap etc.).
// Values are test-controllable via setForTest so initBudget()'s 48KB floor
// and zero-budget branches can be exercised exactly.
#pragma once

#include <cstddef>
#include <cstdint>

class EspClassStub {
 public:
  uint32_t getFreeHeap() const { return freeHeap_; }
  uint32_t getMaxAllocHeap() const { return maxAllocHeap_; }

  // Test control.
  void setForTest(uint32_t freeHeap, uint32_t maxAllocHeap) {
    freeHeap_ = freeHeap;
    maxAllocHeap_ = maxAllocHeap;
  }

 private:
  uint32_t freeHeap_ = 320 * 1024;
  uint32_t maxAllocHeap_ = 320 * 1024;
};

extern EspClassStub Esp;  // NOLINT — mirrors Arduino naming

// The real Arduino.h defines `ESP` as an instance; the stub translation unit
// defines it so BookFontLoader.cpp links unchanged.
extern EspClassStub ESP;