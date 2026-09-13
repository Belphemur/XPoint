#pragma once

#include <cstddef>
#include <cstdint>

struct mbedtls_sha256_context {
  int unused = 0;
};

void mbedtls_sha256_init(mbedtls_sha256_context* ctx);
void mbedtls_sha256_free(mbedtls_sha256_context* ctx);
int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int is224);
int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const uint8_t* input, size_t ilen);
int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, uint8_t output[32]);
