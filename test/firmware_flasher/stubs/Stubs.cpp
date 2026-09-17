// Host-test stub definitions for FirmwareFlasher's ESP-IDF, HAL, and mbedTLS
// dependencies. The SHA-256 implementation is a compact FIPS 180-4 port so the
// validator's SHA-256 trailer check runs for real against actual release
// images; firmware itself links mbedTLS.
#include <cstring>

#include "HalStorage.h"
#include "OtaBootSwitch.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"

namespace {
// Default matches the synthetic-image fixtures; tests validating real release
// images override it via testSetRunningChipId().
uint16_t g_hostChipId = 0x1234;
}  // namespace

void testSetRunningChipId(uint16_t chipId) { g_hostChipId = chipId; }

HalStorage HalStorage::instance;

const esp_partition_t* esp_ota_get_running_partition() {
  static const esp_partition_t running{"ota_0", 0x10000, 2 * 1024 * 1024};
  return &running;
}

const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* running) {
  (void)running;
  static const esp_partition_t next{"ota_1", 0x210000, 2 * 1024 * 1024};
  return &next;
}

esp_err_t esp_partition_read(const esp_partition_t* partition, size_t src_offset, void* dst, size_t size) {
  if (partition != esp_ota_get_running_partition() || src_offset != 12 || size != sizeof(uint16_t)) return -1;
  std::memcpy(dst, &g_hostChipId, sizeof(g_hostChipId));
  return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t*, uint32_t, uint32_t) { return ESP_OK; }
esp_err_t esp_partition_write(const esp_partition_t*, uint32_t, const void*, size_t) { return ESP_OK; }

namespace ota_boot {
bool switchTo(const esp_partition_t* dest) {
  (void)dest;
  return true;
}
}  // namespace ota_boot

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256Transform(mbedtls_sha256_context& ctx, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = (static_cast<uint32_t>(p[4 * i]) << 24) | (static_cast<uint32_t>(p[4 * i + 1]) << 16) |
           (static_cast<uint32_t>(p[4 * i + 2]) << 8) | static_cast<uint32_t>(p[4 * i + 3]);
  }
  for (int i = 16; i < 64; i++) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = ctx.h[0], b = ctx.h[1], c = ctx.h[2], d = ctx.h[3];
  uint32_t e = ctx.h[4], f = ctx.h[5], g = ctx.h[6], h = ctx.h[7];
  for (int i = 0; i < 64; i++) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + S1 + ch + kK[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  ctx.h[0] += a;
  ctx.h[1] += b;
  ctx.h[2] += c;
  ctx.h[3] += d;
  ctx.h[4] += e;
  ctx.h[5] += f;
  ctx.h[6] += g;
  ctx.h[7] += h;
}

}  // namespace

void mbedtls_sha256_init(mbedtls_sha256_context* ctx) {
  ctx->h[0] = 0x6a09e667;
  ctx->h[1] = 0xbb67ae85;
  ctx->h[2] = 0x3c6ef372;
  ctx->h[3] = 0xa54ff53a;
  ctx->h[4] = 0x510e527f;
  ctx->h[5] = 0x9b05688c;
  ctx->h[6] = 0x1f83d9ab;
  ctx->h[7] = 0x5be0cd19;
  ctx->bitLen = 0;
  ctx->blockLen = 0;
}

void mbedtls_sha256_free(mbedtls_sha256_context*) {}

int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int is224) {
  mbedtls_sha256_init(ctx);
  (void)is224;  // only SHA-256 is used by the flasher
  return 0;
}

int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const uint8_t* input, size_t ilen) {
  ctx->bitLen += static_cast<uint64_t>(ilen) * 8;
  while (ilen > 0) {
    const size_t take = (64 - ctx->blockLen < ilen) ? 64 - ctx->blockLen : ilen;
    std::memcpy(ctx->block + ctx->blockLen, input, take);
    ctx->blockLen += take;
    input += take;
    ilen -= take;
    if (ctx->blockLen == 64) {
      sha256Transform(*ctx, ctx->block);
      ctx->blockLen = 0;
    }
  }
  return 0;
}

int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, uint8_t output[32]) {
  const uint64_t bitLen = ctx->bitLen;
  const uint8_t pad0x80 = 0x80;
  mbedtls_sha256_update(ctx, &pad0x80, 1);
  const uint8_t zero = 0;
  while (ctx->blockLen != 56) mbedtls_sha256_update(ctx, &zero, 1);
  uint8_t lenBytes[8];
  for (int i = 0; i < 8; i++) lenBytes[i] = static_cast<uint8_t>(bitLen >> (56 - 8 * i));
  mbedtls_sha256_update(ctx, lenBytes, 8);

  for (int i = 0; i < 8; i++) {
    output[4 * i] = static_cast<uint8_t>(ctx->h[i] >> 24);
    output[4 * i + 1] = static_cast<uint8_t>(ctx->h[i] >> 16);
    output[4 * i + 2] = static_cast<uint8_t>(ctx->h[i] >> 8);
    output[4 * i + 3] = static_cast<uint8_t>(ctx->h[i]);
  }
  return 0;
}
