#pragma once

// Host stand-in for the Arduino core header. Epub.cpp / Section.cpp /
// ChapterHtmlSlimParser.cpp / FsHelpers.cpp call millis()/delay()/ESP and use
// fixed-width types without including <cstdint> directly — on device those
// arrive transitively via Arduino.h. The harness force-includes this header
// (see -include Arduino.h in CMakeLists.txt).

#include <cstdint>
#include <cstring>
#include <cstring>

inline uint32_t millis() { return 0; }
inline void delay(uint32_t) {}

struct EspHostStub {
  uint32_t getFreeHeap() const { return UINT32_MAX; }
};

inline EspHostStub ESP;
