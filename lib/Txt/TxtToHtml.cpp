#include "TxtToHtml.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

const char* TxtToHtml::cacheVersionTag(std::string_view filename) {
  return FsHelpers::hasMarkdownExtension(filename) ? "<!-- MD_CACHE_VERSION: 1 -->" : "<!-- TXT_CACHE_VERSION: 1 -->";
}

bool TxtToHtml::stream(std::string_view filename, void* readerCtx, int (*readFn)(void* ctx, uint8_t* buf, size_t size),
                       Print& out, bool allowEarlyStop) {
  constexpr size_t IN_BUF_SIZE = 8192;
  constexpr size_t OUT_BUF_SIZE = 8192;

  auto inBuf = makeUniqueNoThrow<uint8_t[]>(IN_BUF_SIZE);
  auto outBuf = makeUniqueNoThrow<uint8_t[]>(OUT_BUF_SIZE);
  if (!inBuf || !outBuf) {
    LOG_ERR("TXT", "OOM: TXT/MD HTML streaming buffers");
    return false;
  }

  size_t outPos = 0;
  bool outputOk = true;
  bool sinkSatisfied = false;
  auto flushOut = [&]() {
    if (outPos == 0) return;
    const size_t written = out.write(outBuf.get(), outPos);
    if (written == outPos) {
      outPos = 0;
      return;
    }
    if (allowEarlyStop) {
      // ZipFile semantics: the sink asks us to stop once it has enough.
      sinkSatisfied = true;
      outPos = 0;
    } else {
      outputOk = false;
    }
  };
  auto stopRequested = [&]() { return sinkSatisfied; };

  auto writeByte = [&](uint8_t b) {
    outBuf[outPos++] = b;
    if (outPos == OUT_BUF_SIZE) flushOut();
  };

  auto writeStr = [&](std::string_view s) {
    for (char c : s) {
      writeByte(static_cast<uint8_t>(c));
    }
  };

  writeStr("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
  writeStr(cacheVersionTag(filename));
  writeByte('\n');
  writeStr("<!DOCTYPE html>\n<html>\n<head><title>");
  std::string title = FsHelpers::getFileNameWithoutExtension(filename);
  for (char c : title) {
    if (c == '&')
      writeStr("&amp;");
    else if (c == '<')
      writeStr("&lt;");
    else if (c == '>')
      writeStr("&gt;");
    else
      writeByte(static_cast<uint8_t>(c));
  }
  writeStr("</title></head>\n<body>\n");

  // Incremental UTF-8 validation (survives readFn chunk boundaries): any
  // invalid byte/sequence becomes U+FFFD so the section XML parser never sees
  // malformed input. Lead-byte bounds (overlong/surrogate/range) are checked
  // via the first continuation's allowed window.
  uint8_t utf8Pending[4] = {0, 0, 0, 0};
  uint8_t utf8Len = 0;
  uint8_t utf8Expect = 0;
  uint8_t contMin = 0x80;
  uint8_t contMax = 0xBF;
  auto utf8Reset = [&]() {
    utf8Len = 0;
    utf8Expect = 0;
    contMin = 0x80;
    contMax = 0xBF;
  };
  auto writeReplacement = [&]() {
    writeStr("\xEF\xBF\xBD");
    utf8Reset();
  };

  bool isStart = true;
  bool atLineStart = true;
  size_t pendingSpaces = 0;
  int bytesRead = 0;

  while (!stopRequested() && (bytesRead = readFn(readerCtx, inBuf.get(), IN_BUF_SIZE)) > 0) {
    int startIdx = 0;
    if (isStart) {
      isStart = false;
      if (bytesRead >= 3 && inBuf[0] == 0xEF && inBuf[1] == 0xBB && inBuf[2] == 0xBF) {
        startIdx = 3;
      }
    }

    for (int i = startIdx; i < bytesRead; i++) {
      uint8_t b = inBuf[i];

      if (utf8Expect > 0) {
        if (b >= contMin && b <= contMax) {
          utf8Pending[utf8Len++] = b;
          utf8Expect--;
          if (utf8Expect == 0) {
            for (uint8_t k = 0; k < utf8Len; k++) {
              writeByte(utf8Pending[k]);
            }
            utf8Reset();
          }
          continue;
        }
        writeReplacement();  // falls through: reprocess b as a fresh byte
      }

      if (b == '\r') continue;

      if (b == '\n') {
        pendingSpaces = 0;
        writeStr("<br />");
        atLineStart = true;
        continue;
      }

      if (b == ' ') {
        if (atLineStart) {
          writeStr("&#160;");
        } else {
          pendingSpaces++;
        }
        continue;
      }

      if (pendingSpaces > 0) {
        for (size_t s = 0; s < pendingSpaces - 1; s++) {
          writeStr("&#160;");
        }
        writeByte(' ');
        pendingSpaces = 0;
      }
      atLineStart = false;

      if (b < 0x80) {
        if (b == '&') {
          writeStr("&amp;");
        } else if (b == '<') {
          writeStr("&lt;");
        } else if (b == '>') {
          writeStr("&gt;");
        } else if (b < 0x20 && b != '\t') {
          writeByte(' ');
        } else {
          writeByte(b);
        }
        continue;
      }

      if (b <= 0xDF) {  // 2-byte lead
        if (b < 0xC2) {
          writeReplacement();  // C0/C1 are overlong
          continue;
        }
        utf8Pending[0] = b;
        utf8Len = 1;
        utf8Expect = 1;
        contMin = 0x80;
        contMax = 0xBF;
        continue;
      }
      if (b <= 0xEF) {  // 3-byte lead
        utf8Pending[0] = b;
        utf8Len = 1;
        utf8Expect = 2;
        // E0 rejects overlong (cont < A0), ED rejects surrogates (cont > 9F)
        contMin = (b == 0xE0) ? 0xA0 : 0x80;
        contMax = (b == 0xED) ? 0x9F : 0xBF;
        continue;
      }
      if (b <= 0xF4) {  // 4-byte lead
        utf8Pending[0] = b;
        utf8Len = 1;
        utf8Expect = 3;
        // F0 rejects overlong (cont < 90), F4 rejects > U+10FFFF (cont > 8F)
        contMin = (b == 0xF0) ? 0x90 : 0x80;
        contMax = (b == 0xF4) ? 0x8F : 0xBF;
        continue;
      }
      writeReplacement();  // F5-FF and stray continuations
    }
  }

  if (utf8Expect > 0) {
    writeReplacement();  // truncated sequence at end of input
  }

  if (bytesRead < 0 && !sinkSatisfied) {
    LOG_ERR("TXT", "Read error while streaming TXT/MD: %.*s", static_cast<int>(filename.size()), filename.data());
    return false;
  }

  writeStr("\n</body>\n</html>\n");
  flushOut();
  if (sinkSatisfied) {
    return true;
  }
  if (!outputOk) {
    LOG_ERR("TXT", "Failed to stream complete HTML (write error or disk full)");
    return false;
  }
  return true;
}

bool TxtToHtml::stream(std::string_view filename, std::string_view content, Print& out, bool allowEarlyStop) {
  struct ViewReader {
    std::string_view s;
    size_t pos = 0;
  } reader{content, 0};

  return stream(
      filename, &reader,
      [](void* ctx, uint8_t* buf, size_t size) -> int {
        auto* r = static_cast<ViewReader*>(ctx);
        if (r->pos >= r->s.size()) return 0;
        const size_t n = std::min(size, r->s.size() - r->pos);
        memcpy(buf, r->s.data() + r->pos, n);
        r->pos += n;
        return static_cast<int>(n);
      },
      out, allowEarlyStop);
}
