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

// Upper bound on boards parsed from one manifest: five boards x two asset
// families. The release workflow puts crosspoint entries first so v1.15.x
// parsers (which retain the old cap of 8 and return the first board match)
// still resolve their compatibility URL.
constexpr int OTA_MANIFEST_MAX_BOARDS = 16;

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

// Like findBoardEntry, but prefers the entry whose URL matches the asset the
// release JSON selected. This disambiguates duplicate board entries published
// during the crosspoint/xpoint asset-name transition. Falls back to the first
// board match for manifests that carry only one entry per board.
inline const ManifestBoardEntry* findBoardEntryForUrl(const ManifestBoardEntry* entries, int count, const char* name,
                                                      size_t len, const char* url) {
  const ManifestBoardEntry* fallback = findBoardEntry(entries, count, name, len);
  if (!fallback || !url || url[0] == '\0') return fallback;
  for (const ManifestBoardEntry* e = fallback; e < entries + count; ++e) {
    if (len == strlen(e->board) && memcmp(e->board, name, len) == 0 && strcmp(e->url, url) == 0) return e;
  }
  return fallback;
}
