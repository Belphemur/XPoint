#pragma once

#include <cstddef>
#include <cstdint>

struct mbedtls_sha256_context {
  // Real FIPS 180-4 state so the host validator verifies the SHA-256 trailer
  // of real release images (see Stubs.cpp for the implementation).
  uint32_t h[8];
  uint64_t bitLen;
  uint8_t block[64];
  size_t blockLen;
};

void mbedtls_sha256_init(mbedtls_sha256_context* ctx);
void mbedtls_sha256_free(mbedtls_sha256_context* ctx);
int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int is224);
int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const uint8_t* input, size_t ilen);
int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, uint8_t output[32]);
