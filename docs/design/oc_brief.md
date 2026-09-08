# Task: Eliminate SD temp files for EPUB HTML/CSS content on PSRAM boards

## Overview
On PSRAM boards (X4 Pro, `BOARD_HAS_PSRAM`), decompressed EPUB content (HTML chapters, CSS files) is currently streamed to temp files on SD, then read back. This wastes SD I/O on content that easily fits in PSRAM (8 MB available). Eliminate the temp-file roundtrip by decompressing directly into PSRAM and parsing from memory.

## Branch/worktree
- Branch: `feat/psram-html-cache` (already created from origin/develop)
- Workdir: `/home/balor/workspace/eink/crosspoint-psram-html`
- Submodule `freeink-sdk` already initialized

## Design doc
`docs/design/2026-09-08-psram-epub-content.md` — read this for full context and sizing.

## Changes (KISS, DRY, single pattern)

### 1. ZipFile::readFileToMemory (lib/ZipFile/ZipFile.cpp:395)
- Change `malloc(dataSize)` → `heap_caps_malloc(dataSize, MALLOC_CAP_SPIRAM)` when `BOARD_HAS_PSRAM`
- Change `free(data)` → `heap_caps_free(data)` in the error/cleanup paths when `BOARD_HAS_PSRAM`
- The caller `Epub::readItemContentsToBytes` (Epub.cpp:902) returns `uint8_t*` — keep that interface. The caller already uses `free()` for cleanup; make that pool-aware too.

### 2. Epub::readItemContentsToBytes (lib/Epub/Epub.cpp:894-908)
- Change `free(content)` → pool-aware: `#ifdef BOARD_HAS_PSRAM heap_caps_free(content) #else free(content) #endif`
- Also update `Epub::readItemContentsToMemory` if it exists — grep will find it.

### 3. Section.cpp:beginBuild (lib/Epub/Epub/Section.cpp:354-394)
- On `BOARD_HAS_PSRAM`: decompress HTML to PSRAM buffer via `readItemContentsToBytes`, then:
  - Write the PSRAM buffer to the persistent `htmlPath` in ONE `write()` call (single SD write instead of N/8192 chunked writes)
  - Parse from the PSRAM buffer instead of reading back from file
  - The parser needs a memory-based parse method (see #4)
  - On PSRAM alloc failure: fall back to the existing stream-to-file path (no behavior change)
- Non-PSRAM boards: unchanged code path

### 4. ChapterHtmlSlimParser (lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h/.cpp)
- Add `parseFromMemory(const uint8_t* data, size_t len)` that uses `XML_Parse()` (expat memory-parse API) instead of `XML_GetBuffer` + `XML_ParseBuffer` + file reads
- The existing `parseStep()` reads from `HalFile`; the memory path is a one-shot `XML_Parse` call with `done=true`
- The `filepath` member is still used for CSS path resolution — keep it, add a memory span alongside

### 5. CssParser (lib/Epub/Epub/css/CssParser.h/.cpp)
- Add `loadFromMemory(const char* data, size_t len)` that processes the buffer instead of `HalFile`
- The existing `loadFromStream` reads char-by-char from `HalFile`; the memory version advances a pointer
- The `handleChar` lambda stays the same — just change the data source

### 6. Epub.cpp CSS path (lib/Epub/Epub.cpp:376-405)
- On `BOARD_HAS_PSRAM`: decompress CSS to PSRAM buffer, parse from memory, skip temp file entirely (CSS temp files are non-persistent — always deleted after parse)
- Non-PSRAM boards: unchanged
- Use `#ifdef BOARD_HAS_PSRAM` blocks that fall back to the existing path on failure

## Key constraints
- **DRY**: The pool-aware allocation/free is a single `makeUniqueNoThrowPsram`-style pattern (already in `lib/Memory/Memory.h`). Reuse it where possible. The PSRAM/DRAM fallback logic should be in ONE place per function, not scattered.
- **SOLID**: The parser's core character-handling logic (`handleChar` lambda in CssParser, expat handlers in ChapterHtmlSlimParser) must NOT be duplicated. Extract the data-source abstraction (file vs memory) so the parsing logic is identical.
- **KISS**: Each function gets one `#ifdef BOARD_HAS_PSRAM` block that falls back to the existing path. No new abstractions for non-PSRAM boards.
- **No behavior change on non-PSRAM boards** (C3): the `#else` path is byte-for-byte the existing code.
- **Falls back to SD path on PSRAM alloc failure**: if `heap_caps_malloc` returns nullptr, use the original stream-to-file path.

## Files to create/modify
- `lib/ZipFile/ZipFile.cpp` — PSRAM allocation in readFileToMemory
- `lib/Epub/Epub.cpp` — PSRAM free in readItemContentsToBytes; PSRAM CSS path
- `lib/Epub/Epub/Section.cpp` — PSRAM HTML path in beginBuild
- `lib/Epub/Epub/css/CssParser.h` — add loadFromMemory
- `lib/Epub/Epub/css/CssParser.cpp` — implement loadFromMemory (refactor loadFromStream to share logic)
- `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h` — add parseFromMemory, add memory buffer member
- `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` — implement parseFromMemory using XML_Parse

## Verify
- `pio run -e default` (no PSRAM — fallback path)
- `pio run -e x4pro` (PSRAM path)
- `./bin/clang-format-fix -g`
- `pio check`
- Host tests: `pio test -e native` (stubs unaffected)
- NO commit, NO push — just make the edits. Stop after verify.
