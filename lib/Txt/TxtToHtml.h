#pragma once

#include <Print.h>

#include <string_view>

class TxtToHtml {
 public:
  static const char* cacheVersionTag(std::string_view filename);
  // allowEarlyStop: a short write from the sink means it has enough (probe
  // paths); conversion stops and the stream reports success. Without it a
  // short write is a write error.
  static bool stream(std::string_view filename, void* readerCtx, int (*readFn)(void* ctx, uint8_t* buf, size_t size),
                     Print& out, bool allowEarlyStop = false);
  static bool stream(std::string_view filename, std::string_view content, Print& out, bool allowEarlyStop = false);
};
