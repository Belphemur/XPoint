#include "ManifestJsonParser.h"

#include <cstdlib>
#include <cstring>

namespace {

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool hexDecode(const char* hex, size_t len, uint8_t* out) {
  if (len != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const int hi = hexVal(hex[2 * i]);
    const int lo = hexVal(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

void safeCopy(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  const size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace

ManifestJsonParser::ManifestJsonParser()
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd}),
      lastKey(Key::NONE),
      pos(Pos::TOP),
      expectArray(false),
      versionFound(false),
      nEntries(0) {
  version[0] = '\0';
  memset(&cur, 0, sizeof(cur));
}

void ManifestJsonParser::feed(const char* data, size_t len) { parser.feed(data, len); }

void ManifestJsonParser::commitBoard() {
  if (nEntries < MAX_BOARDS) {
    entries[nEntries++] = cur;
  }
  memset(&cur, 0, sizeof(cur));
}

void ManifestJsonParser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<ManifestJsonParser*>(ctx);
  if (self->pos == Pos::TOP) {
    if (len == 7 && memcmp(key, "version", 7) == 0)
      self->lastKey = Key::VERSION;
    else if (len == 6 && memcmp(key, "boards", 6) == 0)
      self->lastKey = Key::BOARDS;
    else
      self->lastKey = Key::NONE;
  } else if (self->pos == Pos::IN_BOARD) {
    if (len == 5 && memcmp(key, "board", 5) == 0)
      self->lastKey = Key::BOARD;
    else if (len == 5 && memcmp(key, "asset", 5) == 0)
      self->lastKey = Key::ASSET;
    else if (len == 3 && memcmp(key, "url", 3) == 0)
      self->lastKey = Key::URL;
    else if (len == 4 && memcmp(key, "size", 4) == 0)
      self->lastKey = Key::SIZE;
    else if (len == 6 && memcmp(key, "sha256", 6) == 0)
      self->lastKey = Key::SHA256;
    else
      self->lastKey = Key::NONE;
  } else {
    self->lastKey = Key::NONE;
  }
}

void ManifestJsonParser::sOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ManifestJsonParser*>(ctx);
  switch (self->lastKey) {
    case Key::VERSION:
      if (self->pos == Pos::TOP) {
        safeCopy(self->version, sizeof(self->version), value, len);
        self->versionFound = true;
      }
      break;
    case Key::BOARD:
      if (self->pos == Pos::IN_BOARD) safeCopy(self->cur.board, sizeof(self->cur.board), value, len);
      break;
    case Key::URL:
      if (self->pos == Pos::IN_BOARD) safeCopy(self->cur.url, sizeof(self->cur.url), value, len);
      break;
    case Key::SHA256:
      if (self->pos == Pos::IN_BOARD) self->cur.hasSha = hexDecode(value, len, self->cur.sha256);
      break;
    default:
      break;
  }
  self->lastKey = Key::NONE;
}

void ManifestJsonParser::sOnNumber(void* ctx, const char* value, size_t /*len*/) {
  auto* self = static_cast<ManifestJsonParser*>(ctx);
  if (self->lastKey == Key::SIZE && self->pos == Pos::IN_BOARD) {
    self->cur.size = static_cast<size_t>(strtoul(value, nullptr, 10));
  }
  self->lastKey = Key::NONE;
}

void ManifestJsonParser::sOnBool(void* ctx, bool /*value*/) {
  static_cast<ManifestJsonParser*>(ctx)->lastKey = Key::NONE;
}
void ManifestJsonParser::sOnNull(void* ctx) { static_cast<ManifestJsonParser*>(ctx)->lastKey = Key::NONE; }

void ManifestJsonParser::sOnObjectStart(void* ctx) {
  auto* self = static_cast<ManifestJsonParser*>(ctx);
  if (self->pos == Pos::TOP && self->lastKey == Key::BOARDS) {
    self->expectArray = true;
  } else if (self->pos == Pos::IN_BOARDS) {
    self->pos = Pos::IN_BOARD;
    memset(&self->cur, 0, sizeof(self->cur));
  }
  self->lastKey = Key::NONE;
}

void ManifestJsonParser::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<ManifestJsonParser*>(ctx);
  if (self->pos == Pos::IN_BOARD) {
    self->commitBoard();
    self->pos = Pos::IN_BOARDS;
  }
}

void ManifestJsonParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<ManifestJsonParser*>(ctx);
  if (self->expectArray) {
    self->pos = Pos::IN_BOARDS;
    self->expectArray = false;
  }
  self->lastKey = Key::NONE;
}

void ManifestJsonParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<ManifestJsonParser*>(ctx);
  if (self->pos == Pos::IN_BOARDS) self->pos = Pos::TOP;
}
