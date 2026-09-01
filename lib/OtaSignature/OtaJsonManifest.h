#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

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

// Upper bound on boards parsed from one manifest (matches the release
// workflow's per-fork board count with headroom).
constexpr int OTA_MANIFEST_MAX_BOARDS = 8;

// Returns the entry whose `board` matches name/len, or nullptr if the
// manifest carries no image for that board.
inline const ManifestBoardEntry* findBoardEntry(const ManifestBoardEntry* entries, int count, const char* name,
                                                size_t len) {
  if (!entries || !name) return nullptr;
  for (int i = 0; i < count; ++i) {
    const ManifestBoardEntry& e = entries[i];
    if (len == strlen(e.board) && memcmp(e.board, name, len) == 0) return &e;
  }
  return nullptr;
}
