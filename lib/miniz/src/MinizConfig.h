/* CrossPoint only needs miniz's low-level streaming inflate (tinfl). The
 * archive, deflate, stdio, and zlib-compatibility layers are compiled out so
 * the vendored library stays small and never touches the filesystem or clock.
 * Include this header instead of <miniz.h> so every translation unit sees the
 * same configuration. */
#pragma once

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

// The ESP32 mask ROM exports tinfl_* at fixed addresses via DIRECT linker
// script assignments (e.g. "tinfl_decompress = 0x...;" in the ROM .ld),
// which override object-file definitions -- without these renames the
// firmware silently binds to the ROM's 2021 build (TINFL_LESS_MEMORY, a
// different tinfl_decompressor layout) and corrupts inflate state on real
// data. Rename so the linker can never capture them. The prefix is
// crosspoint_ (NOT freeink_) so a future branch that links FreeInkBook's
// identically-renamed copy does not collide. All exported mz_* and
// tinfl_* symbols are renamed — the core inflate API (mz_inflate,
// mz_inflateInit2, mz_version, etc.) is NOT covered by
// MINIZ_NO_ZLIB_COMPATIBLE_NAMES and must be renamed explicitly.
#define tinfl_decompress crosspoint_tinfl_decompress
#define tinfl_decompress_mem_to_heap crosspoint_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem crosspoint_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback crosspoint_tinfl_decompress_mem_to_callback
#define tinfl_decompressor_alloc crosspoint_tinfl_decompressor_alloc
#define tinfl_decompressor_free crosspoint_tinfl_decompressor_free
#define mz_crc32 crosspoint_mz_crc32
#define mz_adler32 crosspoint_mz_adler32
#define mz_free crosspoint_mz_free
#define mz_version crosspoint_mz_version
#define mz_inflateInit2 crosspoint_mz_inflateInit2
#define mz_inflateInit crosspoint_mz_inflateInit
#define mz_inflateReset crosspoint_mz_inflateReset
#define mz_inflateEnd crosspoint_mz_inflateEnd
#define mz_error crosspoint_mz_error
#define mz_inflate crosspoint_mz_inflate
#define mz_uncompress2 crosspoint_mz_uncompress2
#define mz_uncompress crosspoint_mz_uncompress
#define miniz_def_free_func crosspoint_miniz_def_free_func
#define miniz_def_alloc_func crosspoint_miniz_def_alloc_func
#define miniz_def_realloc_func crosspoint_miniz_def_realloc_func

// Include the vendored miniz by relative path: ESP-IDF ships a ROM miniz.h
// with the SAME include guard but a different (TINFL_LESS_MEMORY) struct
// layout -- resolving <miniz.h> through the platform include path would
// silently compile against the wrong structures.
#include "../third_party/miniz.h"
