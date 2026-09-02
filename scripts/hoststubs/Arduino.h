#pragma once

// Host-build stub for the board-features dump (scripts/board_features_dump.c).
// BoardConfig.h includes <Arduino.h>; the dump only reads constexpr board
// profiles, so these declarations only need to parse, never execute. Do not
// use for anything else — no real Arduino API surface lives here.

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#define INPUT 0x0
#define OUTPUT 0x1
#define HIGH 0x1
#define LOW 0x0

class HardwareSerial {
 public:
  void begin(unsigned long) {}
  operator bool() const { return true; }
};

extern HardwareSerial Serial;
extern HardwareSerial Serial0;

inline void pinMode(uint8_t, uint8_t) {}
inline void digitalWrite(uint8_t, uint8_t) {}
