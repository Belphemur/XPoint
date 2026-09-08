## Design: Eliminate SD Temp Files for EPUB Content on PSRAM Boards

### Context

Serial log shows the EPUB engine writing decompressed HTML to a temp file on SD, then reading it back:

```
[52170] [DBG] [ZIP] Decompressed 415 bytes into 770 bytes
[52174] [DBG] [SCT] Streamed temp HTML to /.crosspoint/epub_2298905019/html/.tmp_11.html (770 bytes)
```

On X4 Pro (8 MB PSRAM, `PSRAMFree=8074KB`), this SD roundtrip is avoidable. The decompressed HTML fits trivially in PSRAM. The same pattern exists for CSS files:

```cpp
// Section.cpp:354-394 — HTML: readItemContentsToStream → temp .tmp_N.html → rename/promote
// Epub.cpp:377-405  — CSS: readItemContentsToStream → temp .tmp.css → loadFromStream → remove
```

### Current flow (Section.cpp:beginBuild)

1. `epub->readItemContentsToStream(localPath, tmpHtml, 8192)` (`Epub.cpp:911`)
   → `ZipFile::readFileToStream` (`ZipFile.cpp:467`) — streams inflated data in **2 × 8KB `malloc`** chunks to the `HalFile` on SD
2. Temp file promoted via `Storage.rename(tmpHtmlPath → htmlPath)` for caching
3. `ChapterHtmlSlimParser` opens `htmlPath` via `Storage.openFileForRead` (`ChapterHtmlSlimParser.cpp:2007`)
4. Parser reads back in **1KB chunks** via expat `XML_GetBuffer(XML_...), XML_ParseBuffer(...)` (`ChapterHtmlSlimParser.cpp:2027-2042`)

**SD operations per chapter**: 2 writes (inflate stream) + 1 rename + 1 open + N reads (1KB each) + 1 close + (on cache miss) 1 remove.

### Current flow (CSS parsing, Epub.cpp:376-405)

1. `readItemContentsToStream(cssPath, tempCssFile, kStreamChunkSize)`
2. `tempCssFile.close()`
3. `Storage.openFileForRead(tmpCssPath, tempCssFile)` — reopen for read
4. `cssParser->loadFromStream(tempCssFile)` — reads in chunks via `HalFile::read`
5. `tempCssFile.close()` + `Storage.remove(tmpCssPath)`

**SD operations per CSS file**: 2 writes + 1 close + 1 open + 1 close + 1 remove.

### What already uses PSRAM

| Location | PSRAM allocation? |
|----------|-------------------|
| `ZipFile::readFileToStream` buffers (`fileReadBuffer`, `outputBuffer`) | ❌ `malloc(8KB)` — DRAM on all boards |
| `ZipFile::readFileToMemory` buffer | ❌ `malloc(inflatedDataSize)` — DRAM on all boards |
| `Epub::readItemContentsToStream` | No allocation (passes through to ZipFile) |
| `Epub::readItemContentsToBytes` → `readFileToMemory` | ❌ DRAM |
| `PixelCache` decode band | ✅ `heap_caps_malloc(MALLOC_CAP_SPIRAM)` |
| `Epub` object | ✅ PSRAM (Epub.cpp:235) |
| `ZipFileCache` | ✅ PSRAM (Epub.cpp:468) |
| `Epub::cache_` (render cache) | ✅ PSRAM (Epub.cpp:1125) |

### Proposed design

#### Step 1: PSRAM-backed `readFileToMemory` (`ZipFile.cpp:395`)

Change `readFileToMemory`'s `malloc(dataSize)` to use `heap_caps_malloc(MALLOC_CAP_SPIRAM)` when `BOARD_HAS_PSRAM` is defined. This makes `Epub::readItemContentsToBytes` (already used for covers at `Epub.cpp:902`) return PSRAM-backed buffers. The caller already wraps the result in a smart pointer with `free()` cleanup — needs to become `heap_caps_free` for the PSRAM path.

**Size considerations**: Chapter HTML files are typically 50–500 KB uncompressed. CSS files are typically 5–50 KB. Both fit trivially in 8 MB PSRAM. The `Epub::readItemContentsToBytes` caller already checks file size via `getItemSize` and skips oversized items (CSS path: `MAX_CSS_FILE_SIZE` at `Epub.cpp:366`).

**Risk**: `readFileToMemory` is used for cover images (up to full 800×480 = 48 KB × 2 bits = 12 KB, or full-color covers). PSRAM placement is fine for these — they're read once and passed to `PixelCache`.

#### Step 2: PSRAM-backed HTML decompression in Section.cpp (beginBuild)

On `BOARD_HAS_PSRAM` boards, replace the temp-file streaming path with `readItemContentsToBytes` → PSRAM buffer:

```cpp
#ifdef BOARD_HAS_PSRAM
  size_t htmlSize;
  auto htmlBuf = epub->readItemContentsToBytes(localPath, &htmlSize);
  if (!htmlBuf) { /* fallback to stream-to-file path */ }
  // Pass htmlBuf to ChapterHtmlSlimParser via memory-based parse
#else
  // Original stream-to-temp-file path
#endif
```

The parser reads from PSRAM buffer instead of SD file. This eliminates all SD writes/reads for the HTML itself.

**Key question**: Can `ChapterHtmlSlimParser` parse from a memory buffer? Expat supports `XML_Parse(xmlParser, buf, len, done)` — a direct memory-parse call that doesn't need `XML_GetBuffer` + `XML_ParseBuffer`. The parser would need a new code path.

#### Step 3: ChapterHtmlSlimParser memory-based parse

Add a `parseFromMemory(const uint8_t* buf, size_t len)` overload alongside the current `parseStep()` (which reads from `HalFile`). The memory-parse path uses `XML_Parse` directly:

```cpp
// Instead of:
void* buf = XML_GetBuffer(parser_, PARSE_BUFFER_SIZE);
parseFile_.read(buf, PARSE_BUFFER_SIZE);
XML_ParseBuffer(parser_, len, done);

// Use:
XML_Parse(parser_, memBuf + offset, chunkSize, isFinal);
```

The parser already has `filepath` — it would also accept a `Span<const uint8_t>` for the in-memory path. The `parseStep()` loop becomes trivial (one `XML_Parse` call, done) when parsing from memory.

#### Step 4: PSRAM-backed CSS parsing (Epub.cpp:376-405)

On `BOARD_HAS_PSRAM` boards, replace the temp-file CSS path with `readItemContentsToBytes` + `CssParser::loadFromMemory`:

```cpp
#ifdef BOARD_HAS_PSRAM
  size_t cssSize;
  auto cssBuf = readItemContentsToBytes(cssPath, &cssSize);
  if (!cssBuf) { /* fallback */ }
  cssParser->loadFromMemory(cssBuf.get(), cssSize);  // new method
#else
  // Original stream-to-temp-file path
#endif
```

`CssParser` would need a `loadFromMemory(const char* data, size_t len)` overload. The parser's `handleChar` loop already processes one character at a time from `source.read()` — a memory-backed version just advances a pointer instead.

### Why this helps

From the serial log:
```
[52135] [DBG] [SCT] Failed to open file for reading: /.tmp_11.html
[52170] [DBG] [ZIP] Decompressed 415 bytes into 770 bytes
[52174] [DBG] [SCT] Streamed temp HTML to .tmp_11.html (770 bytes)
```

The 770-byte HTML file: 2 SD writes + 1 rename + 1 open + ~1 read + 1 close = **5 SD operations** for 770 bytes of data. On PSRAM boards, this becomes 0 SD operations (read into PSRAM buffer, parse from memory).

For a full novel (e.g., the 584 KB spine mentioned in the 8KB-chunk comment), the savings are larger. The current streaming uses 2 × 8KB DRAM buffers + writes to SD in 8KB chunks. With PSRAM, the entire decompressed HTML sits in a single PSRAM buffer, and the parser reads it sequentially with no SD I/O.

### Risks and mitigations

| Risk | Level | Mitigation |
|------|-------|------------|
| PSRAM allocation failure for large chapters | Medium | `readItemContentsToBytes` returns nullptr → fall back to the existing stream-to-file path. No behavior change on failure. |
| Parser doesn't support memory-backed parsing | Low | `XmlParse` API is a standard expat function — `XML_Parse(parser, buf, len, done)`. The character-by-character `handleChar` loop in `CssParser` maps trivially to a pointer advance. |
| PSRAM bandwidth contention during render | Low | HTML/CSS parsing happens in the background build path (`buildSomeMore`), not the foreground render path. No overlap. |
| Cache persistence (temp file is the cache) | Medium | The `.html` cache file serves as the PSRAM-bypass mechanism on next open. When using PSRAM for parsing, the temp file should **still be written** for caching (skip the re-inflate on next open) — use PSRAM only for the *read-back* path. See "Cache strategy" below. |

### Cache strategy

**Critical**: the temp HTML file is not just a temp — it's the persistent HTML cache (`htmlPath = htmlDir/spineIndex.html`). The Section.cpp code at line 343-348 explicitly states: "The unzipped HTML is keyed only on the book... survives the invalidation that wipes the layout (.bin) caches."

**Correct approach for PSRAM path**:
1. On PSRAM boards: decompress HTML into a PSRAM buffer (`readItemContentsToBytes`)
2. Parse from the PSRAM buffer (no temp file read-back)
3. **Still write the PSRAM buffer to the persistent `htmlPath`** for caching (so next open skips inflation)
4. Promote via `rename` as before

This eliminates the *read-back* (open + N reads + close) but keeps the write for cache persistence. The write is a single large write (vs the current multiple 8KB writes), which is also faster on SD.

Actually, even better: `readFileToStream` writes to the temp file in chunks. If we decompress to PSRAM first, then write the whole buffer to SD in one shot, that's **one write call** instead of N/8192 write calls. The SD write count drops from ~70 (for 584 KB) to 1.

For CSS: the temp file is NOT persistent (it's always removed after parsing). So for CSS, the entire temp-file lifecycle can be eliminated on PSRAM boards — decompress to PSRAM, parse from memory, done. No SD write, no remove.

### Sizing

| Item | Typical size | PSRAM cost (1 chapter) |
|------|-------------|----------------------|
| HTML buffer (PSRAM) | 50–500 KB | 50–500 KB (freed after build) |
| ZIP fileReadBuffer (8KB) | 8 KB | 8 KB (reused across items) |
| ZipOutputBuffer (8KB) | 8 KB | 8 KB (reused across items) |
| CSS buffer (PSRAM) | 5–50 KB | 5–50 KB (freed after parse) |

All freed after the build completes. With 8 MB PSRAM and 115 KB used at idle, even a 500 KB HTML chapter uses <6% of PSRAM transiently.

### Implementation plan

1. **`ZipFile::readFileToMemory`** → PSRAM allocation on `BOARD_HAS_PSRAM` (ZipFile.cpp:395)
2. **`Epub::readItemContentsToBytes`** → ensure PSRAM buffer is returned (already calls `readFileToMemory`, just needs the ZipFile change)
3. **`Section.cpp:beginBuild`** → on PSRAM, decompress to buffer, parse from memory, write to cache in one shot
4. **`ChapterHtmlSlimParser`** → add `parseFromMemory()` using `XML_Parse` instead of file-based `XML_ParseBuffer`
5. **`CssParser`** → add `loadFromMemory()` accepting a `string_view` or buffer pointer
6. **`Epub.cpp:CSS path`** → on PSRAM, decompress CSS to buffer, parse from memory, skip temp file entirely

### Verification

- `pio run -e default` — DRAM path unchanged (compiles with original code path)
- `pio run -e x4pro` — PSRAM path active (new code under `#ifdef BOARD_HAS_PSRAM`)
- Host tests: stubs for ZipFile/Section/Epub unchanged (no new test surface)
- `./bin/clang-format-fix -g`
- `pio check`
- Device: serial log should show "Decompressed N bytes into M bytes" with no subsequent "Streamed temp HTML" line for cache misses on X4 Pro
- Device: `PSRAMFree` should show a transient drop during chapter build (HTML buffer allocated) followed by recovery after the build frees the buffer
