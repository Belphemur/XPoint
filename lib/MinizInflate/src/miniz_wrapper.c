// mz_* wrapper API over the fork's split miniz.
//
// The firmware consumes miniz header-only (full_miniz.h, ROM-bound cores) and
// builds no wrapper objects, but SDK libs declaring CONTENT_EXTERNAL_MINIZ —
// freeink-sdk's ContentProtection — expect the host image to provide
// mz_inflateInit2/mz_inflate/mz_inflateEnd. This unit compiles the wrapper
// layer of the same v1.15 fork vendored in freeink-sdk; on ESP targets
// miniz_cores.c is empty and its tinfl/tdefl calls bind to the mask ROM at
// link time, exactly like InflateStream's direct tinfl calls. FreeInkBook's
// own vendored copy compiles with freeink_* renames, so there is no symbol
// collision. Archive/stdio/time paths are compiled out: nothing in the
// firmware uses mz_zip and it keeps the flash cost to the inflate/compress
// wrappers alone. The tdefl_compress_mem_to_* helpers are renamed out of the
// way: miniz.c defines them unconditionally and FreeInkBook's vendored copy
// (MinizConfig.h does not gate them) already defines the same symbols; this
// copy's are dead here (nothing compresses through this layer).

#include "../../../freeink-sdk/libs/book/FreeInkBook/third_party/miniz/src/miniz.c"
#include "../../../freeink-sdk/libs/book/FreeInkBook/third_party/miniz/src/miniz_cores.c"