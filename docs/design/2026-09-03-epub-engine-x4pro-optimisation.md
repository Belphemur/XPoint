# EPUB Engine Optimisation for X4 Pro (ESP32-S3 + 8MB PSRAM)

**Date:** 2026-09-03
**Branch:** `design/epub-engine-mem-sdcard` (from `origin/develop` SHA `57ca4438`)
**Origin:** `https://github.com/Belphemur/crosspoint-x-reader`
**Scope:** `lib/Epub/` only. Design doc only — no `.cpp`/`.h` edits, no commits, no push.

---

## 1. Goal + Non-goals

**Goal:** Make EPUB reading measurably faster on X4 Pro (ESP32-S3, 8MB OPI PSRAM, SSD1677 800×480) by moving bulky structures from DRAM → PSRAM, caching I/O-bound work, and optionally swapping the parser for a heavier engine — all while keeping C3/Sticky/X4C/Paper Mono builds compilable and stable.

**Non-goals:**
- Breaking C3/Sticky/X4C/Paper Mono. Every change is gated behind `#ifdef BOARD_HAS_PSRAM` or `#ifdef BOARD_X4PRO`.
- Adding a new LGPL-licensed external dependency (lexbor is LGPL — excluded from the engine-swap candidate list).
- Lifting `EINK_DISPLAY_SINGLE_BUFFER_MODE=1` for X4 Pro unless the design proves the DRAM impact is acceptable (see §4 and open question #2).
- Any `.cpp`/`.h` edits in this task (doc-only).

## 2. Current State on X4 Pro

The X4 Pro env (`[env:x4pro]` in `platformio.ini:266-288`) inherits `base`, sets `-DBOARD_HAS_PSRAM`, `-DFREEINK_DEVICE_X4PRO=1`, `board_build.arduino.memory_type = dio_opi`, and `EINK_DISPLAY_SINGLE_BUFFER_MODE=1` (from `base:29`). Note: `x4c` also carries `-DBOARD_HAS_PSRAM` (line 305) despite the brief's claim; this is flagged as open question #5.

| Subsystem | File(s) & lines | Today's placement | Notes |
|---|---|---|---|
| Framebuffer | `lib/GfxRenderer/GfxRenderer.cpp:120-131` | DRAM, 48KB (`HalDisplay::BUFFER_SIZE`) | `begin()` calls `display.getFrameBuffer()`. Single-buffer mode. |
| Grayscale scratch | `GfxRenderer.cpp:2449-2474` (`storeBwBuffer`) | DRAM, 48KB via 8KB `malloc` chunks | `MIN_FREE_HEAP_FOR_JPEG` line 97 = 36KB; grayscale + JPEG together can OOM on C3. |
| JPEG decoder | `converters/JpegToFramebufferConverter.cpp:96-97` | Heap, ~20KB (`JPEGDEC`) | `MIN_FREE_HEAP_FOR_JPEG = 36KB` gates decode; line 368 `new (std::nothrow) JPEGDEC()`. |
| PNG decoder | `converters/PngToFramebufferConverter.cpp:86-87` | Heap, ~44KB (`PNG`) | `MIN_FREE_HEAP_FOR_PNG = 58KB` gates decode; line 352 `new (std::nothrow) PNG()`. |
| PixelCache band | `converters/PixelCache.h:59,90-91` | DRAM via `malloc`, ≤24KB | `MAX_BAND_BYTES = 24KB`; streams decoded rows to SD; avoids holding full image in heap. |
| Slim parser | `parsers/ChapterHtmlSlimParser.cpp:28` | DRAM stack + heap | `PARSE_BUFFER_SIZE = 1024` (Expat); `partWordBuffer[MAX_WORD_SIZE+1]` = 201B stack. |
| ParsedText words | `ParsedText.h:26` | DRAM heap | `std::deque<std::string> words` (avoids contiguous realloc); per-token `std::vector` arrays. |
| Page elements | `Page.h:79-83` | DRAM heap per page | `std::vector<std::shared_ptr<PageElement>> elements`, `footnotes`, `links`. |
| Section LUT | `Section.h:37-55`, `Section.h:39` | DRAM heap (build phase only) | `BuildContext::lut` (`vector<PageLutEntry>`), `inlineStyleStack`, `blockStyleStack`. |
| Hyphenation tries | `hyphenation/generated/hyph-en.trie.h` (etc.) | **Flash** (`alignas(4) constexpr uint8_t`) | `hyph-de.trie.h` = 1,289,474 bytes (~1.26MB); total 10 tries ≈ 1.6MB in flash. `src/main.cpp:40-115` loads fonts as global statics; tries are compile-time constants. |
| ZIP catalog | `Epub.cpp:219,828-836,853-854` | No cache | `ZipFile(filepath)` constructed **per call** (lines 219, 836, 853). Central directory re-read on every `readItemContentsToStream` / `getItemSize`. |
| Book metadata | `BookMetadataCache.cpp:14-20` | SD (`.crosspoint/epub_<hash>/book.bin`) | `BOOK_CACHE_VERSION = 10`; `BUILD_IO_BUFFER_SIZE = 4096`. |
| CSS parser | `css/CssParser.cpp:44-50` | DRAM heap | `MAX_RULES = 1500`, `SELECTOR_POOL_CAP = 32KB`, `MAX_UNIQUE_STYLES = 256`; `MIN_FREE_HEAP_FOR_CSS = 48KB` — skips CSS if heap below threshold. |
| Section HTML | `Section.cpp:1095` (full file) | Streamed from ZIP → SD cache | `.crosspoint/epub_<hash>/sections/<spineIndex>.bin`; version `SECTION_FILE_VERSION = 45` (line 50). |

**What I did NOT measure:** actual per-activity heap usage on X4 Pro hardware (no serial capture from a live device). All DRAM/PSRAM figures below are estimates derived from code reading and the known hardware constants.

## 3. X4 Pro Target Hardware Budget

| Subsystem | DRAM target | PSRAM target | Flash target | Rationale |
|---|---|---|---|---|
| Framebuffer (primary) | ≤48KB (unchanged) | — | — | Must stay in DRAM; the display controller accesses DRAM directly. |
| Framebuffer (secondary, page-turn) | — | ≤48KB when enabled | — | Gated; only when `EINK_DISPLAY_SINGLE_BUFFER_MODE` is lifted (§4). |
| Grayscale scratch (storeBwBuffer) | ≤48KB (unchanged) | — | — | Tied to `storeBwBuffer()`; moved to PSRAM only if double-buffer is adopted. |
| Parsed DOM / section HTML | ≤8KB (working set) | ≤1MB (cached HTML) | — | PSRAM holds the pre-parsed section; DRAM keeps only the active chunk. |
| Page list (`Section::pages`) | ≤16KB (LUT + active) | — | — | LUT is already serialized to SD; keep small in DRAM. |
| JPEG/PNG decoder | — | ≤20KB JPEG + ≤44KB PNG | — | Decoders heap-allocate anyway; move to PSRAM on X4 Pro. |
| PixelCache band | — | ≤24KB band buffer | — | Currently `malloc` in DRAM; redirect to PSRAM. |
| ZIP central directory | — | ≤64KB (cached) | — | Cache once at book open; eliminates repeated reads. |
| CSS rule tables | ≤48KB (working) | ≤256KB (full) | — | Keep working set in DRAM; full tables in PSRAM when CSS is active. |
| Hyphenation tries | — | — | 1.6MB (unchanged) | Already in flash as `constexpr`. No change. |
| Free heap reserve | ≥80KB | ≥2MB | — | Guard against fragmentation; leave headroom for tasks. |

## 4. Engine Decision

**Recommendation: Stay with the slim parser (status quo), but add PSRAM caching and a PSRAM-backed decoder pipeline. Do NOT swap to a full DOM engine on X4 Pro.**

Justification (estimates):

| Candidate | Extra DRAM | Extra PSRAM | Extra Flash | Parse-time delta | Render-time delta | Stability risk | Verdict |
|---|---|---|---|---|---|---|---|
| **Status quo (slim)** | Baseline | — | — | Baseline | Baseline | Low | **Chosen baseline** |
| Full DOM (gumbo, Apache-2.0) | +50-100KB tree nodes | +500KB-2MB | +300-800KB | Faster (single pass) | Faster (no re-parse) | Medium (heap-heavy on C3 if unguarded) | Rejected — flash cost too high for marginal gain; C3 gating complexity |
| Full DOM (lexbor, LGPL) | +50-100KB | +500KB-2MB | +200-500KB | Faster | Faster | **High** (LGPL license) | **Rejected** — licensing incompatible with closed firmware (AGENTS.md philosophy) |
| Full DOM (libxml2, MIT) | +200-500KB | +1-4MB | +1-3MB | Faster | Faster | Medium (large footprint) | Rejected — too large for the 380KB-DRAM ceiling even on X4 Pro |
| PSRAM-backed DOM of slim parser | — | +200-500KB | — | Same | Faster (no re-parse) | Low | **Acceptable mid-term** (Phase 2) |

The slim parser is explicitly memory-frugal by design (hand-written Expat state machine, `std::deque` to avoid contiguous-block growth, `MAX_ANCHORS_PER_CHAPTER = 1024` cap at `ChapterHtmlSlimParser.cpp:46`). The bottleneck on X4 Pro is **I/O** (ZIP re-reads, SD decode), not parsing. On a device with 8MB PSRAM and `dio_opi` flash, the right move is to cache the already-parsed results and decode images into PSRAM — not to replace the parser.

**Headline:** Keep the slim parser. Gate PSRAM caches behind `BOARD_HAS_PSRAM`. Phase 3 (engine swap) remains open if profiling proves I/O is no longer the bottleneck after Phase 2.

## 5. Numbered Proposed Changes (Priority Order)

### Change 1 — PSRAM-buffered ZIP central directory cache
- **Files:** `lib/Epub/Epub.cpp:219,828-836,853-854` (all `ZipFile(filepath)` constructions)
- **DRAM delta:** — (no new heap allocation)
- **PSRAM delta:** ≤64KB (cached central directory + file offset map)
- **Flash delta:** ≤4KB (cache struct)
- **Render-time delta:** Book-open: ~50-200ms saved (no repeated central-directory reads); per-page-turn: negligible for text, significant for images (avoids per-image `getInflatedFileSize` zip seek)
- **Gating:** `#ifdef BOARD_HAS_PSRAM` — cache in PSRAM; on non-PSRAM boards, `ZipFile(filepath)` stays per-call (current behavior). Build-size impact on C3/Sticky: **zero** (no new `.cpp`).
- **Risk:** Low. Central directory format is well-defined; a parsed cache is a read-only lookup table.
- **Verification:** `LOG_INF` heap/PSRAM at book open; compare `pio run -e x4pro` build size vs baseline.

### Change 2 — PSRAM-decoded image pipeline (JPEG + PNG)
- **Files:** `converters/JpegToFramebufferConverter.cpp:96-97,368,400`, `converters/PngToFramebufferConverter.cpp:86-87,352`, `converters/PixelCache.h:90-91`
- **DRAM delta:** ≤0KB (decoders currently heap-allocate; redirect to PSRAM)
- **PSRAM delta:** ≤64KB (JPEG 20KB + PNG 44KB decoders) + ≤24KB (PixelCache band buffer)
- **Flash delta:** ≤2KB (guards)
- **Render-time delta:** Image-heavy pages: 10-30% faster (no DRAM-heap contention with `JPEGDEC`/`PNG` allocations on C3; on X4 Pro, PSRAM allocation is near-instant)
- **Gating:** `#ifdef BOARD_HAS_PSRAM` for decoder placement in PSRAM; `PixelCache::begin()` `malloc` → `makeUniqueNoThrow<uint8_t[]>` (already PSRAM-friendly on PSRAM boards because `new (std::nothrow)` hits PSRAM first on ESP32-S3). **Fallback on non-PSRAM:** current `malloc` path unchanged.
- **Risk:** Low-Medium. Must confirm ESP32-S3 heap allocation order (PSRAM-first vs DRAM-first); test with `ESP.getFreeHeap()` and `ESP.getFreePsram()` before/after decode.
- **Verification:** `LOG_INF` PSRAM before/after decode; page-turn latency for image-heavy chapters.

### Change 3 — PSRAM pre-parsed section cache
- **Files:** `lib/Epub/Epub/Section.cpp:50,247-256`, `lib/Epub/Epub/Section.h:37-55` (`BuildContext::lut`)
- **DRAM delta:** ≤16KB (active LUT + parser working set)
- **PSRAM delta:** ≤1MB (cached HTML for the active section; typical novel chapter is 50-200KB HTML, well within PSRAM for multiple sections)
- **Flash delta:** —
- **Render-time delta:** Chapter open: 200-500ms saved (avoid re-inflating HTML from ZIP on revisit); incremental builds unchanged.
- **Gating:** `#ifdef BOARD_HAS_PSRAM`. Non-PSRAM: current `.bin` serialization path unchanged.
- **Risk:** Medium. Requires a new cache file format or extension to `.section.psram`; must bump `SECTION_FILE_VERSION` (currently 45) if binary layout changes. Version bump invalidates existing `.crosspoint/` caches (acceptable — forces re-parse).
- **Verification:** `LOG_INF` PSRAM at section open; re-open same chapter and measure time.

### Change 4 — Cover thumb cache (PSRAM-resident recent covers)
- **Files:** `lib/Epub/Epub.cpp:665-669,754-758` (cover generation paths), `src/activities/home/HomeActivity.cpp:166` (cover buffer pattern)
- **DRAM delta:** —
- **PSRAM delta:** ≤64KB (2-3 recent cover thumbnails at ~30KB each)
- **Flash delta:** —
- **Render-time delta:** Home screen: instant cover display for recent books (no re-generation).
- **Gating:** `#ifdef BOARD_HAS_PSRAM`. Non-PSRAM: regenerate on demand.
- **Risk:** Low. Covers are already cached on SD; PSRAM just holds the working set.
- **Verification:** Home screen open latency; `LOG_INF` PSRAM usage.

### Change 5 — `x4pro_perf` build env with single-buffer lift (Phase 3 option)
- **Files:** `platformio.ini:266-288` (add `[env:x4pro_perf]`), `GfxRenderer.cpp` (double-buffer logic)
- **DRAM delta:** +48KB (second framebuffer in PSRAM)
- **PSRAM delta:** +48KB (secondary framebuffer)
- **Flash delta:** ≤2KB (guard)
- **Render-time delta:** Page-turn animation: 2-4× faster (double-buffered flip); grayscale renders: no `storeBwBuffer`/`restoreBwBuffer` copy overhead.
- **Gating:** `#ifdef BOARD_X4PRO` AND `#ifdef BOARD_HAS_PSRAM`. Must NOT apply to Paper Mono (which also has PSRAM but different display controller timing). `#ifdef BOARD_X4PRO` alone gates it.
- **Risk:** **High.** `EINK_DISPLAY_SINGLE_BUFFER_MODE=1` is currently in `base:29` for ALL boards. Lifting it for X4 Pro only requires: (a) a new env that overrides the flag, (b) the second framebuffer allocation path in `GfxRenderer` (lines ~120-131 and ~340-370), (c) double-buffer flip logic. The `storeBwBuffer` path (line 2449) may need rewriting for double-buffer. **Honest uncertainty:** I did not fully trace the double-buffer flip path; this is flagged as open question #2.
- **Verification:** Page-turn animation on device; `LOG_INF` heap before/after; must not trigger watchdog.

### Change 6 — `std::vector` `.reserve()` in hot parse paths
- **Files:** `lib/Epub/Epub/ParsedText.cpp` (various `push_back` loops), `ChapterHtmlSlimParser.cpp` (line 812 `tableRowCells.reserve`, line 588 `tableLineVisibleOffsets.reserve`)
- **DRAM delta:** None (avoids reallocation fragmentation)
- **PSRAM/Flash delta:** —
- **Render-time delta:** Minor (~5-10% less fragmentation-related stall during parse)
- **Gating:** None (applies to all boards). Zero build-size impact.
- **Risk:** Low. Confirmed by AGENTS.md §7 (`std::vector` pre-allocation rule).
- **Verification:** `pio check`; heap fragmentation monitoring via `ESP.getFreeHeap()` before/after chapter open.

### Change 7 — `constexpr` freeze on hyphenation trie lookups
- **Files:** `lib/Epub/Epub/hyphenation/generated/hyph-*.trie.h` (already `constexpr`); `Hyphenator.cpp`, `LiangHyphenation.cpp`
- **DRAM delta:** — (already flash)
- **PSRAM/Flash delta:** — (no change)
- **Render-time delta:** None (already compile-time)
- **Gating:** None. Already compliant.
- **Risk:** None. Already done.
- **Verification:** Confirm `constexpr` in generated files (confirmed: `alignas(4) constexpr uint8_t en_trie_data[]` at `hyph-en.trie.h:5`).

## 6. Gating Matrix

| Optimization | x4pro | papermono | x4c | sticky |
|---|---|---|---|---|
| ZIP central-dir cache (§1) | `#ifdef BOARD_HAS_PSRAM` | (same define — PSRAM present) | (PSRAM defined but brief says no — see Q5) | off (no PSRAM) |
| PSRAM image decode (§2) | `#ifdef BOARD_HAS_PSRAM` | same | same | off |
| PSRAM section cache (§3) | `#ifdef BOARD_HAS_PSRAM` | same | same | off |
| Cover thumb cache (§4) | `#ifdef BOARD_HAS_PSRAM` | same | same | off |
| Single-buffer lift (§5) | `#ifdef BOARD_X4PRO && BOARD_HAS_PSRAM` | **off** (different display) | off | off |
| `reserve()` in parse paths (§6) | none (all boards) | none | none | none |
| Hyphenation constexpr (§7) | none (all boards) | none | none | none |

*Note: Paper Mono carries `-DBOARD_HAS_PSRAM` (platformio.ini:385) and uses SSD1683 grayscale, so §1-4 apply to it identically to x4pro. §5 is X4-Pro-specific because Paper Mono's display controller timing differs.*

## 7. On-Disk Cache Layouts (PSRAM-Buffered)

```
SD Card (.crosspoint/epub_<hash>/)          PSRAM (runtime working set)
├── book.bin          ← metadata           ┌─ ZIP central-dir cache (≤64KB)
├── progress.bin      ← progress           ├─ Active section HTML cache (≤1MB)
├── cover.bmp         ← cover thumbnail    ├─ Decoded image cache (≤24KB band)
├── cover_thumb_0.bmp ← recent covers      ├─ Recent cover thumbs (≤64KB)
└── sections/
    ├── 0.bin         ← serialized pages   │
    ├── 1.bin              ...             └─ Decoder heap (JPEG 20KB / PNG 44KB)
    └── N.bin
```

**Layer model:**
- **L1 (PSRAM):** ZIP cache, active section HTML, decoder heap, cover thumbs.
- **L2 (SD):** All serialized `.bin` files, cover BMPs, pixel cache bands (streamed).
- **L3 (Flash):** Hyphenation tries (1.6MB), fonts (~80 objects), code.

On non-PSRAM boards (C3/Sticky), L1 collapses to DRAM only — the layout stays the same, just smaller.

## 8. Engine Alternatives Evaluation

See §4 for the full table. Summary of the head-to-head:

| Candidate | License | DRAM | PSRAM | Flash | Verdict |
|---|---|---|---|---|---|
| **lexbor** | LGPL | +50-100KB | +500KB-2MB | +200-500KB | **Rejected** — LGPL incompatible with closed firmware |
| **gumbo** | Apache-2.0 | +50-100KB | +500KB-2MB | +300-800KB | Rejected — flash cost too high; marginal gain over slim + PSRAM cache |
| **libxml2** | MIT | +200-500KB | +1-4MB | +1-3MB | Rejected — too large; C3 path becomes unstable |
| **Purpose-built DOM** (replace `ChapterHtmlSlimParser`) | MIT (ours) | +50-100KB | +500KB-2MB | +100-300KB | Rejected for now — complexity not justified by I/O-bottleneck evidence |
| **Slim + PSRAM cache (chosen)** | MIT | baseline | +200-500KB | baseline | **Chosen** — lowest risk, highest practical gain |

**Why the slim parser wins on X4 Pro:** The bottleneck is I/O (ZIP central-directory re-reads, SD decode), not parsing. The slim parser already uses an Expat state machine with bounded working sets. PSRAM caching of the parsed output and a PSRAM-decoded image pipeline deliver 80% of the benefit of a full DOM engine at 20% of the risk and cost.

## 9. Online References

1. **`atomic14/diy-esp32-epub-reader`** — Proof that PSRAM-based EPUB parsing is viable on ESP32; validates the PSRAM-cache approach. *Takeaway: PSRAM turns the "can't parse" problem into a "cache management" problem.*
2. **`epublib` + `kxml2` pull-parser pattern** — Reference for a lightweight pull-parser alternative; useful if Phase 3 reconsider DOM trade-offs. *Takeaway: Pull-parsers avoid full DOM overhead while still offering flexible traversal.*
3. **`@lingo-reader/epub-parser`** — Section-cache layout reference; our `.crosspoint/epub_<hash>/sections/` layout mirrors this philosophy. *Takeaway: Pre-parsed section caches are the industry-standard pattern for e-reader firmware.*
4. **ESP-IDF memory-types guide** (DRAM/IRAM/flash rules) — Governs our `#ifdef BOARD_HAS_PSRAM` gating and `makeUniqueNoThrow` usage. *Takeaway: PSRAM is accessible via `esp_psram_alloc` or the Arduino heap; ESP32-S3 PSRAM is allocated via the default heap when `BOARD_HAS_PSRAM` is defined.*
5. **lexbor / gumbo / libxml2 READMEs** (quick reads) — lexbor is LGPL (disqualified); gumbo is Apache-2.0 (~200KB flash, ~50KB DRAM per parse); libxml2 is MIT but ~1-3MB flash. *Takeaway: License and footprint rule out all three for the C3 path; only a PSRAM-gated custom DOM survives consideration.*

## 10. Migration Plan

**Phase 1 — Gating infrastructure (shippable immediately, no behaviour change):**
- Add `#ifdef BOARD_HAS_PSRAM` guards around all proposed PSRAM allocations.
- Add `#ifdef BOARD_X4PRO` guard for §5 (single-buffer lift).
- No new `.cpp` files; all guards wrap existing code paths.
- Verify: `pio run` succeeds for all 14 envs; `pio check` passes.

**Phase 2 — PSRAM caches for status-quo parser (shippable independently):**
- Implement §1 (ZIP central-dir cache), §2 (PSRAM image decode), §3 (PSRAM section cache), §4 (cover thumbs).
- Bump `SECTION_FILE_VERSION` in `Section.cpp:50` if §3 changes binary layout.
- Add `heap_free_kb` / `psram_free_kb` `LOG_INF` at book open and page-turn.
- Verify: `pio run -e x4pro` build size; device heap/PSRAM monitoring.

**Phase 3 — Engine swap (optional, gated behind profiling):**
- Only if Phase 2 profiling shows I/O is no longer the bottleneck.
- Implement §5 (single-buffer lift) and/or a purpose-built DOM.
- Requires new `x4pro_perf` env in `platformio.ini`.
- Verify: page-turn animation latency; watchdog monitoring.

## 11. Verification Plan

For each phase, verify with:

| Metric | Method | Baseline (today) | Target (Phase 2) |
|---|---|---|---|
| `heap_free_kb` at book open | `LOG_INF("MEM", "Free: %d", ESP.getFreeHeap())` | ~55KB (est.) | ≥80KB |
| `psram_free_kb` at book open | `LOG_INF("MEM", "PSRAM free: %d", ESP.getFreePsram())` | measured free at boot (≈ 8 MB on X4 Pro) | ≥ 6 MB free after Phase 2 caches |
| `pio run -e x4pro` build size | PlatformIO build output | baseline | ≤ baseline + 5KB (Phase 2) |
| Page-turn latency (manually timed) | `millis()` delta between page requests | baseline | 20-30% reduction on image-heavy pages |
| Chapter re-open time | `millis()` delta | baseline | 200-500ms saved (§3) |
| CI pass | `pio run` for all envs | pass | pass |

**Add to `src/main.cpp` (or the relevant activity):**
```cpp
LOG_INF("MEM", "Heap: %dKB, PSRAM: %dKB", ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
```
at book open and page-turn entry points.

## 12. Open Questions

1. **Is a heavier engine worth the flash cost?** My recommendation is no — the slim parser + PSRAM cache delivers the practical benefit at lower risk. But if profiling in Phase 2 shows I/O is not the bottleneck, a purpose-built DOM (not gumbo/lexbor/libxml2) becomes worth evaluating. **Answer deferred to Phase 2 profiling.**

2. **Is single-buffer mode lift for X4 Pro acceptable, or must we keep it for code simplicity?** I am genuinely uncertain here. The `storeBwBuffer()`/`restoreBwBuffer()` path (GfxRenderer.cpp:2449-2499) copies 48KB via 8KB `malloc` chunks; a second PSRAM framebuffer would eliminate this. But I did not fully trace the double-buffer flip logic in `GfxRenderer`. **Requires deeper `GfxRenderer` audit before committing.**

3. **Is a per-book on-disk cache acceptable?** Yes — the `.crosspoint/epub_<hash>/` layout already exists; PSRAM caches are an extension. Cache invalidation rules (version bump, render-setting change) are already documented in `docs/file-formats.md` and `AGENTS.md:990-1013`. **No issue.**

4. **Does the SDK fork's parser change this design?** The `freeink-sdk` fork contains `ZipFile`, `JPEGDEC`, `PNGdec`, and `SdFat` — all of which the current design depends on. If the fork adds PSRAM-aware `ZipFile` central-directory caching or `mmap`-style reads, §1 becomes simpler. **Recommend: check fork for `ZipFile` central-dir caching before implementing §1.**

5. **Does `x4c` actually have `BOARD_HAS_PSRAM`?** `platformio.ini:305` sets `-DBOARD_HAS_PSRAM` for `[env:x4c]`, contradicting the brief's claim that x4c has no PSRAM. If x4c does have PSRAM, the gating matrix for §1-4 should apply to x4c too. **Requires confirmation from hardware team or the `BoardConfig` profile in the SDK.**

---

*Document ends. Path: `docs/design/2026-09-03-epub-engine-x4pro-optimisation.md`*
