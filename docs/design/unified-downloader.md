# Unified Downloader with PSRAM Support

## Status
**Draft design — not yet implemented.**

## Problem Statement

Currently, the firmware has three distinct download patterns that each manage memory, HTTP, and progress independently:

1. **`HttpDownloader::downloadToFile`** — used by FontDownloadActivity for fonts, downloading to SD then verifying with CRC32. Always streams to SD; never uses PSRAM as an intermediate buffer.
2. **`HttpDownloader::fetchUrl` (callback variant)** — used by OtaUpdater for firmware streaming. Writes directly to flash partitions via `esp_ota_write(); never buffers the full image in memory.
3. **`HttpDownloader::fetchUrl` (string variant)** — used for small JSON (OTA manifest). Always loads the entire response body into a `std::string` in DRAM.

The font manifest no longer follows pattern 1: `FontDownloadActivity::fetchAndParseManifest` fetches via the callback variant into an in-memory `PoolBytes` buffer (PSRAM on S3 boards, DRAM on C3), capped at 64 KB, and parses the JSON from that buffer — no SD round-trip.

None of the large-download paths leverage a **PSRAM buffer** even though the X4 Pro, X4 Classic, and Paper Mono all have 8MB PSRAM. On PSRAM-equipped boards, large downloads are currently forced through the SD-card intermediate path, which is slower (extra I/O pass) and wastes the 8MB of external RAM that's already available.

### Current memory situation on PSRAM boards
- `PlatformIO` `platformio.ini`: `BOARD_HAS_PSRAM` is defined for `x4pro`, `x4c`, `papermono` environments.
- `lib/Memory/Memory.h` provides `poolMalloc` / `poolFree` / `PoolBytes` that transparently allocate from PSRAM on PSRAM boards and DRAM on non-PSRAM boards.
- 8MB PSRAM, ~115KB used at idle — ample headroom for large download buffers.

## Existing Usage Map

| Caller | Method | Buffer strategy | Integrity check |
|--------|--------|-----------------|-----------------|
| `FontDownloadActivity::fetchAndParseManifest` | `fetchUrl(url, callback)` | In-memory `PoolBytes` buffer (PSRAM/DRAM), 64 KB cap | None (manifest fetched over TLS) |
| `FontDownloadActivity::downloadFamily` | `downloadToFile(url, dest, progress, cancel, downgrade=true)` | SD dest directly | CRC32 post-download |
| `OtaUpdater::installUpdate` | `fetchUrl(url, callback)` | Direct-to-flash | SHA-256 (signed manifest) |
| `OtaUpdater::checkForUpdate` | `fetchUrl(url, string, maxBytes=65536)` | DRAM `std::string` | None (pre-verification) |
| `OpdsBookBrowserActivity::fetchFeed` | `fetchUrl(url, Stream&)` | Callback to `OpdsParser` | None |
| `OpdsBookBrowserActivity::downloadBook` | `downloadToFile(url, sdPath, progress, cancel)` | SD dest directly | None |

## Design

### Goal
A single `BufferedDownloader` class that abstracts the download buffer strategy: on PSRAM boards, buffer chunks in PSRAM before writing them to the final destination; on non-PSRAM boards, stream directly to the destination (preserving the current behavior). This is SOLID (Single Responsibility: one class for buffered HTTP download), DRY (no duplicate buffer-management code across activities), and KISS (the caller says "download to file/stream/callback" and the class picks the right buffer size).

### Architecture

```
HttpDownloader (existing — transport layer: TLS, redirects, WiFi power-save)
    ↑
BufferedDownloader (NEW — buffer management: PSRAM vs DRAM, progress pump)
    ↑
DownloadSink (NEW — abstract interface: write + progress + cancel)
    ├─ FileSink          (HalFile wrapper for SD/dest writes)
    ├─ StreamSink        (write-through to a Stream, used by OpDS callback)
    └─ CallbackSink      (direct callback, used by OTA flash-write)
```

#### `BufferedDownloader`

```cpp
class BufferedDownloader {
 public:
  struct Options {
    size_t bufferSize = 2048;   // PSRAM-friendly chunk size; see sizing rationale below
    HttpDownloader::ProgressCallback onProgress = nullptr;
    bool* cancelFlag = nullptr;
    std::string username;
    std::string password;
    bool downgradeRedirectsToHttp = false;
  };

  // Download URL → sink. The sink's write() is called with whatever arrives;
  // the class manages the buffer allocation (PSRAM if available) and the
  // input-pump (mappedInput.update()) so progress callbacks can process input
  // during long downloads.
  HttpDownloader::DownloadError download(const std::string& url, DownloadSink& sink, const Options& opts);
};
```

#### `DownloadSink` interface (in `HttpDownloader.h` or a new `DownloadSink.h`)

```cpp
class DownloadSink {
 public:
  virtual ~DownloadSink() = default;
  virtual bool write(const uint8_t* data, size_t len) = 0;
  virtual void onProgress(size_t downloaded, size_t total) {}
};
```

The sink receives data in chunks. On PSRAM boards, the `BufferedDownloader` allocates a `PoolBytes` buffer of `opts.bufferSize` (default 2048), reads chunks into it, and forwards them to the sink. On non-PSRAM boards, it uses a small stack/DRAM buffer of the same size and streams through.

This design means:
- **FileSink** (SD file): on PSRAM boards, data is buffered in PSRAM then written to SD in fewer, larger writes (reducing SD I/O). On non-PSRAM, it streams directly (current `downloadToFile` behavior).
- **CallbackSink** (OTA flash): the callback receives data as it arrives; the buffer is only used for the HTTP read buffer itself, not for accumulating data (OTA must stream directly to flash — it cannot buffer the whole image).
- **StreamSink** (OpDS parser): same as CallbackSink — data flows through to the parser immediately.

### Buffer sizing: 2048 bytes

Rationale for the 2048-byte default:

| Board | DRAM free | PSRAM | Buffer fits |
|-------|-----------|-------|-------------|
| C3 (X4/X3, slim) | ~380KB | none | 2048B DRAM — negligible |
| S3 (X4 Pro, X4C, PaperMono) | ~380KB | 8MB | 2048B PSRAM — negligible |

The active transport already reads in 2048-byte chunks: `FREEINK_NET_WOLFSSL=1` is set in `[base]` of `platformio.ini`, so every environment routes `HttpDownloader` through `SecureHttpClient`, whose read loop uses `READ_CHUNK = 2048` (freeink-sdk `SecureHttpClient.h`). The `READ_CHUNK = 1024` constant in `HttpDownloader.cpp` belongs to the non-wolfSSL `esp_http_client` fallback, which is compiled out in all shipped builds. Matching the buffer size to the transport chunk therefore yields no syscall reduction by itself — the I/O savings come from coalescing: accumulating several 2048-byte transport chunks before writing (e.g. 8192 B = 4 chunks) produces fewer, larger SD writes. On non-PSRAM boards, 2048B of DRAM is still well within budget for a transient read buffer.

This number is **not** tied to any "download whole file into PSRAM" strategy — it's the chunk size for the HTTP read buffer. Full-file buffering into PSRAM is intentionally **not** done for large downloads (firmware = multi-MB, cannot fit). The buffer is for intermediate staging: on PSRAM boards, data arrives in PSRAM before being written to SD/flash/stream, which lets the write side coalesce and reduces I/O contention.

### PSRAM usage is automatic, not opt-in

The `poolMalloc`/`poolFree` pair in `Memory.h` already handles the `#ifdef BOARD_HAS_PSRAM` branching. `BufferedDownloader` uses `PoolBytes` exclusively for its read buffer — no `#ifdef` at call sites, zero overhead on non-PSRAM boards.

### Integration points (migration plan)

1. **`FontDownloadActivity`**: Replace `HttpDownloader::downloadToFile` with `BufferedDownloader::download(url, FileSink(downloadUrl), opts)`. CRC32 verification stays the same — the FileSink writes to the same path, so `computeFileCrc32` works unchanged.

2. **`OpdsBookBrowserActivity::downloadBook`**: Same swap — becomes `BufferedDownloader::download(url, FileSink(filename), opts)`.

3. **`OtaUpdater::installUpdate`**: This already uses the callback variant (`fetchUrl` with `DataCallback`). The `BufferedDownloader` adds no value here since OTA must stream directly to flash — keep the existing `fetchUrl` path. The class is designed so OTA is a `CallbackSink` if migration is ever desired, but there's no benefit (OTA can't buffer anyway).

4. **`OtaUpdater::checkForUpdate` and `fetchAndVerifyManifest`**: These use `fetchUrl(url, std::string&)` for small JSON. They can optionally migrate to the buffered callback path, but since the payload is <64KB, the DRAM string is fine. Migration is optional and low-priority.

5. **`OpdsBookBrowserActivity::fetchFeed`**: Uses `fetchUrl(url, Stream&)`. Can stay as-is — the streaming callback path already avoids buffering.

### What stays

- `HttpDownloader` remains the transport layer — no changes to its TLS/redirect/WiFi-power-save logic.
- `downloadToFile` and `fetchUrl` overloads remain for backward compatibility and for callers where the overhead of the new class isn't justified.
- `FirmwareFlasher` is untouched — it operates on SD-card files and flash partitions, not HTTP downloads.
- CRC32 verification in FontDownloadActivity stays as a post-download step (it reads from the final SD file, not from the download buffer).

### Host-testability

No host test compiles `HttpDownloader.cpp` — it is tied to the ESP32 transport and storage layers (`SecureHttpClient`/wolfSSL, `HalFile`). The host-testable unit is `download::ChunkCoalescer` (`lib/Download/ChunkCoalescer.h`), a pure C++ class that owns the write-coalescing logic and is covered by the `test/chunk_coalescer` gtest suite (passthrough mode, buffering boundaries, multi-chunk writes, flush-failure propagation). `HttpDownloader` delegates its SD write coalescing to `ChunkCoalescer`, so that behavior runs under `pio run -e x4pro -t unit-tests` (CMake/CTest) on every host test pass. `BufferedDownloader` itself delegates HTTP to `HttpDownloader` and buffer allocation to `poolMalloc`, and is verified by on-device integration; the `DownloadSink` interface is mockable if unit tests are added later.

## Open Questions

1. **Buffer size**: 2048 is proposed, matching the active transport's `SecureHttpClient::READ_CHUNK = 2048`. (The dormant `esp_http_client` fallback still uses 1024.) Should we unify the constant? (Low priority — the HttpDownloader read loop and the BufferedDownloader buffer are separate.)
2. **Should `downloadToFile` in `HttpDownloader` be replaced entirely, or kept alongside?** The design keeps it for now and migrates call sites incrementally.
3. **Error semantics**: `BufferedDownloader::download` returns `HttpDownloader::DownloadError`. Is this the right error type, or should there be a separate enum? (Current proposal: reuse `HttpDownloader::DownloadError` — it's the same error space.)
