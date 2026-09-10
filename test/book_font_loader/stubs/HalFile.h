// Host-test stub of HalFile — real class lives in HalStorage.h (Arduino-bound).
#pragma once

class HalFile {
 public:
  size_t fileSize() { return 0; }
  uint64_t fileSize64() { return 0; }
  int read(void* buf, size_t count) { (void)buf; (void)count; return 0; }
  int read() { return -1; }
};