#pragma once

#include <Print.h>

#include <cstddef>
#include <string>
#include <string_view>

class TxtToHtml {
 public:
  template <typename Append>
  static void streamEscapedHtml(Append&& append, std::string_view text) {
    for (const char c : text) {
      switch (c) {
        case '&':
          append("&amp;");
          break;
        case '<':
          append("&lt;");
          break;
        case '>':
          append("&gt;");
          break;
        default:
          append(std::string_view(&c, 1));
          break;
      }
    }
  }

  static const char* cacheVersionTag(std::string_view filename);
  static size_t escapedHtmlSize(std::string_view text);
  static void appendEscapedHtml(std::string& out, std::string_view text);
  // allowEarlyStop: a short write from the sink means it has enough (probe
  // paths); conversion stops and the stream reports success. Without it a
  // short write is a write error.
  static bool stream(std::string_view filename, void* readerCtx, int (*readFn)(void* ctx, uint8_t* buf, size_t size),
                     Print& out, bool allowEarlyStop = false);
  static bool stream(std::string_view filename, std::string_view content, Print& out, bool allowEarlyStop = false);
};
