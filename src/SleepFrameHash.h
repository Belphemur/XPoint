#pragma once

#include <cstddef>
#include <cstdint>

// Adler-32 (RFC 1950) over a byte range, with the modulo deferred every NMAX
// bytes so a 48KB framebuffer needs no per-byte division. Used as a change
// detector for the sleep-frame SD cache: natural screen content, not
// adversarial input, so a weak hash is acceptable.
//
// Collision trade-off: if a colliding frame is saved, the rewrite is skipped
// and the next quick-resume wake restores the previous frame — cosmetic and
// self-healing on the next sleep whose content differs.
constexpr uint32_t SLEEP_ADLER_MOD = 65521;
// 255*(NMAX+1)*(NMAX/2) must stay below 2^32; zlib's proven NMAX value.
constexpr size_t SLEEP_ADLER_NMAX = 5552;

constexpr uint32_t sleepFrameAdler32(const uint8_t* data, size_t len) {
  uint32_t a = 1;
  uint32_t b = 0;
  while (len > 0) {
    const size_t chunk = len < SLEEP_ADLER_NMAX ? len : SLEEP_ADLER_NMAX;
    len -= chunk;
    for (size_t i = 0; i < chunk; ++i) {
      a += data[i];
      b += a;
    }
    a %= SLEEP_ADLER_MOD;
    b %= SLEEP_ADLER_MOD;
    data += chunk;
  }
  return (b << 16) | a;
}
