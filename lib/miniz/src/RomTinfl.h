#pragma once

#include <cstddef>
#include <cstdint>

// ESP32 firmware inflate backend: the SoC ROM's precompiled miniz tinfl.
//
// The ESP32 ROM ships a miniz tinfl (the 2021 TINFL_LESS_MEMORY build) whose
// tinfl_decompress the ROM linker script exports at a fixed address. Firmware
// calls that machine code directly instead of compiling the vendored miniz:
// the vendored C inflate produces wrong output under the Xtensa toolchain for
// specific DEFLATE streams (inflates correctly on x86, deterministically fails
// on-device), while the ROM image is fixed Espressif-validated machine code.
//
// This header is the ROM's public tinfl ABI (esp_rom/include/miniz.h), which
// is byte-identical across the ESP32-S3 and ESP32-C3 ROM images: the same
// TINFL_LESS_MEMORY struct layout with a 32-bit bit buffer (the header
// hardcodes MINIZ_X86_OR_X64_CPU 0 and TINFL_USE_64BIT_BITBUF 0). The struct
// layout below is pinned here so the
// code never depends on a specific <miniz.h> include-path resolution -- the
// same include-path shadowing class the vendored header avoids via
// MinizConfig.h's relative include. Only the low-level streaming entry point
// is used; tinfl_init is the header's trivial state zero-init, defined inline
// because the ROM exports no such symbol.

// Decompression flags for tinfl_decompress().
enum {
  TINFL_FLAG_PARSE_ZLIB_HEADER = 1,
  TINFL_FLAG_HAS_MORE_INPUT = 2,
  TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4,
  TINFL_FLAG_COMPUTE_ADLER32 = 8,
};

// Max size of the LZ dictionary (streaming ring window).
#define TINFL_LZ_DICT_SIZE 32768

// Return status of tinfl_decompress().
typedef enum {
  TINFL_STATUS_BAD_PARAM = -3,
  TINFL_STATUS_ADLER32_MISMATCH = -2,
  TINFL_STATUS_FAILED = -1,
  TINFL_STATUS_DONE = 0,
  TINFL_STATUS_NEEDS_MORE_INPUT = 1,
  TINFL_STATUS_HAS_MORE_OUTPUT = 2,
} tinfl_status;

// Internal bit-buffer/Huffman-table layout the ROM expects (esp_rom/miniz.h).
// TINFL_USE_64BIT_BITBUF is hardcoded 0 on the ROM build, so m_bit_buf is
// 32-bit and the decompressor state is 10992 bytes on both chips.
enum {
  TINFL_MAX_HUFF_TABLES = 3,
  TINFL_MAX_HUFF_SYMBOLS_0 = 288,
  TINFL_MAX_HUFF_SYMBOLS_1 = 32,
  TINFL_MAX_HUFF_SYMBOLS_2 = 19,
  TINFL_FAST_LOOKUP_BITS = 10,
  TINFL_FAST_LOOKUP_SIZE = 1 << TINFL_FAST_LOOKUP_BITS,
};

struct tinfl_huff_table {
  uint8_t m_code_size[TINFL_MAX_HUFF_SYMBOLS_0];
  int16_t m_look_up[TINFL_FAST_LOOKUP_SIZE];
  int16_t m_tree[TINFL_MAX_HUFF_SYMBOLS_0 * 2];
};

struct tinfl_decompressor_tag {
  uint32_t m_state, m_num_bits, m_zhdr0, m_zhdr1, m_z_adler32, m_final, m_type, m_check_adler32, m_dist, m_counter,
      m_num_extra;
  uint32_t m_table_sizes[TINFL_MAX_HUFF_TABLES];
  uint32_t m_bit_buf;
  size_t m_dist_from_out_buf_start;
  tinfl_huff_table m_tables[TINFL_MAX_HUFF_TABLES];
  uint8_t m_raw_header[4];
  uint8_t m_len_codes[TINFL_MAX_HUFF_SYMBOLS_0 + TINFL_MAX_HUFF_SYMBOLS_1 + 137];
};
typedef struct tinfl_decompressor_tag tinfl_decompressor;

// tinfl_init() equivalent: the ROM header resets only m_state.
static inline void tinfl_init(tinfl_decompressor* r) { r->m_state = 0; }

extern "C" tinfl_status tinfl_decompress(tinfl_decompressor* r, const uint8_t* pIn_buf_next, size_t* pIn_buf_size,
                                         uint8_t* pOut_buf_start, uint8_t* pOut_buf_next, size_t* pOut_buf_size,
                                         const uint32_t decomp_flags);