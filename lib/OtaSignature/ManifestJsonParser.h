#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "StreamingJsonParser.h"

// One board's signed entry inside the release manifest. The manifest lists,
// per board, the exact download URL, byte size, and SHA-256 of the matching
// firmware asset so the device can verify integrity before flashing.
struct ManifestBoardEntry {
  char board[24];
  char url[512];
  size_t size;
  uint8_t sha256[32];
  bool hasSha;
};

// Streaming parser for the release manifest.json:
//   { "version": "1.2.3",
//     "boards": [ { "board":"x4pro", "asset":"firmware-x4pro.bin",
//                   "url":"https://.../firmware-x4pro.bin", "size":N,
//                   "sha256":"<64 hex>" }, ... ] }
// SAX-based: feed the bytes as they arrive, then findBoard() the running
// board's entry. No full-body buffering.
class ManifestJsonParser {
 public:
  ManifestJsonParser();

  ManifestJsonParser(const ManifestJsonParser&) = delete;
  ManifestJsonParser& operator=(const ManifestJsonParser&) = delete;

  void feed(const char* data, size_t len);

  void reset() {
    lastKey = Key::NONE;
    pos = Pos::TOP;
    expectArray = false;
    versionFound = false;
    version[0] = '\0';
    nEntries = 0;
    memset(&cur, 0, sizeof(cur));
  }

  bool foundVersion() const { return versionFound; }
  const char* getVersion() const { return version; }

  // Returns the entry whose `board` matches the running firmware's board tag,
  // or nullptr if the manifest carries no image for this device.
  const ManifestBoardEntry* findBoard(const char* name, size_t len) const;

 private:
  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void commitBoard();

  enum class Key : uint8_t { NONE, VERSION, BOARDS, BOARD, ASSET, URL, SIZE, SHA256 };
  enum class Pos : uint8_t { TOP, IN_BOARDS, IN_BOARD };

  StreamingJsonParser parser;

  Key lastKey;
  Pos pos;
  bool expectArray;

  char version[32];
  bool versionFound;

  ManifestBoardEntry cur;
  static constexpr int MAX_BOARDS = 8;
  ManifestBoardEntry entries[MAX_BOARDS];
  int nEntries;
};

inline const ManifestBoardEntry* ManifestJsonParser::findBoard(const char* name, size_t len) const {
  for (int i = 0; i < nEntries; ++i) {
    const ManifestBoardEntry& e = entries[i];
    if (len == strlen(e.board) && memcmp(e.board, name, len) == 0) return &e;
  }
  return nullptr;
}
