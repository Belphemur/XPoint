#pragma once

// ProgressRecord — the single byte-level encoder/decoder for progress.bin
// records (design §3.5, docs/file-formats.md). ProgressManager is the only
// production consumer; host tests exercise these pure functions directly.
//
// Record layouts (little-endian, byte-packed):
//   6  : {u16 spineIndex, u16 pageNumber, u16 pageCount}          (legacy)
//   10 : legacy + {u32 visibleTextOffset}                          (legacy)
//   16 : {u16 spineIndex, u16 pageNumber, u16 pageCount,
//         u32 charOffset, u32 generation}                          (TTF reader)
//
// The 16-byte layout shares its prefix with the legacy 10-byte one, so a
// legacy reader that encounters a TTF record still reads a sane
// spine/page/pageCount triple (and must NOT treat charOffset as a visible
// text offset — decode() reports the layout via hasGeneration instead).
// Load-side migration: any unknown size degrades to the 6-byte base fields;
// reads never fail.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct ProgressRecord {
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t pageCount = 0;
  uint32_t visibleTextOffset = 0;
  bool hasOffset = false;
  // TTF reader layout only; charOffset is a chapter character offset
  // (layout-generation independent position anchor), NOT a visible text
  // offset. `generation` is the FIBP layout generation hash it was saved
  // under — restore maps it through PageCacheReader::pageForChar only when
  // it matches the current generation.
  uint32_t charOffset = 0;
  uint32_t generation = 0;
  bool hasGeneration = false;
};

namespace progress_record {

static constexpr size_t kSizeBase = 6;
static constexpr size_t kSizeOffset = 10;
static constexpr size_t kSizeGeneration = 16;

inline void putU16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}

inline void putU32(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>(v >> 24);
}

inline uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1]) << 8; }

inline uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

// Encodes the record for the given field availability. Returns the byte
// count written (0 when `out` is too small).
inline size_t encode(const bool hasOffset, const bool hasGeneration, const uint16_t spineIndex,
                     const uint16_t pageNumber, const uint16_t pageCount, const uint32_t visibleTextOffset,
                     const uint32_t charOffset, const uint32_t generation, uint8_t* out, const size_t cap) {
  const size_t size = hasGeneration ? kSizeGeneration : (hasOffset ? kSizeOffset : kSizeBase);
  if (out == nullptr || cap < size) return 0;
  putU16(out, spineIndex);
  putU16(out + 2, pageNumber);
  putU16(out + 4, pageCount);
  if (hasOffset) {
    putU32(out + 6, visibleTextOffset);
  } else if (hasGeneration) {
    putU32(out + 6, charOffset);
    putU32(out + 10, generation);
  }
  return size;
}

// Decodes the longest prefix layout the byte count supports. Returns the
// recognized layout size (0 = garbage/too short; caller treats as absent).
inline size_t decode(const uint8_t* buf, const size_t len, ProgressRecord& out) {
  out = ProgressRecord{};
  if (buf == nullptr || len < kSizeBase) return 0;
  out.spineIndex = getU16(buf);
  out.pageNumber = getU16(buf + 2);
  out.pageCount = getU16(buf + 4);
  if (len >= kSizeGeneration) {
    // Generation layout: charOffset occupies the legacy offset slot, but the
    // semantics differ — a legacy consumer must not map it as a text offset.
    out.charOffset = getU32(buf + 6);
    out.generation = getU32(buf + 10);
    out.hasGeneration = true;
    return kSizeGeneration;
  }
  if (len >= kSizeOffset) {
    out.visibleTextOffset = getU32(buf + 6);
    out.hasOffset = true;
    return kSizeOffset;
  }
  return kSizeBase;
}

}  // namespace progress_record