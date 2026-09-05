## Phase 3: Dual-Core Parse Profiling Instrument Design

### Context

The X4 Pro uses an ESP32-S3 (dual-core Xtensa LX7 @ 240MHz). Currently, EPUB parsing and rendering both run on core 1 inside `EpubReaderActivity::renderBook()`. Phase 3 proposes moving the parse task to core 0 while keeping render on core 1.

Per the design doc v3 §10(a), this is **gated on profiling data**. Before any dual-core code lands, we must profile to determine:
1. Whether parse is CPU-bound (dual-core would help) or cache-bandwidth/PSRAM-bound (dual-core may not help)
2. What the contention pattern looks like

### 1. What to Measure

| Metric | Description | Source | Frequency |
|---|---|---|---|
| `parse_duration_us` | Total time spent in chapter parse (from `createSectionFile()` entry to `buildComplete_`) | `millis()` start/end timestamps | Once per chapter build |
| `render_duration_us` | Total time spent rendering the current page | `millis()` before/after `renderContents()` | Once per page render |
| `psram_free_before` | PSRAM free bytes before a phase | `ESP.getFreePsram()` | At phase start |
| `psram_free_after` | PSRAM free bytes after a phase | `ESP.getFreePsram()` | At phase end |
| `heap_free_before` | Free heap before a phase | `ESP.getFreeHeap()` | At phase start |
| `heap_free_after` | Free heap after a phase | `ESP.getFreeHeap()` | At phase end |
| `core_id` | Core on which the code path runs | `xPortGetCoreID()` | At each instrumentation point |
| `sd_read_bytes` | Cumulative SD bytes read during a phase | SdFat file transaction sizes | Accumulated per phase |
| `sd_read_duration_us` | Cumulative SD read duration during a phase | `millis()` around SD reads | Accumulated per phase |
| `page_parse_duration_us` | Time to lay out individual page | `millis()` in `onPageComplete()` | Once per page |

**Thresholds (determined from profiling data):**
- **Parse-bound**: If `parse_duration_us` > 30% of `total_render_duration_us` (parse + render), parse is CPU-bound and dual-core helps.
- **PSRAM-contended**: If `(psram_free_before - psram_free_after) / psram_free_before` > 70%, PSRAM bandwidth is heavily contended; dual-core may not help due to PSRAM bus arbitration.
- **SD-bound**: If `sd_read_duration_us` > 500,000 us (500ms) per chapter, SD I/O is the bottleneck; dual-core parse may not help without SD caching improvements.

### 2. Where to Instrument

| Location | Function | Purpose | Notes |
|---|---|---|---|
| `Section::createSectionFile()` | `lib/Epub/Epub/Section.cpp:248` | Start/end of full chapter build (one-shot) | Measure parse duration, PSRAM/heap before/after |
| `Section::buildSomeMore()` | `lib/Epub/Epub/Section.cpp:456` | Each incremental build step (background builds) | Measure per-step contention |
| `EpubReaderActivity::renderBook()` | `src/activities/reader/EpubReaderActivity.cpp:1267` | Parse vs render boundary | Core ID, heap/PSRAM at entry/exit |
| `Section::loadSectionFile()` | `lib/Epub/Epub/Section.cpp:143` | Cache hit/miss path | SD read bytes/duration on cache miss |
| `Epub::load()` | `lib/Epub/Epub.cpp:447` | Book open (OPF + TOC + CSS parse) | Full pipeline timing |
| Each `Page` completion in `Section::onPageComplete()` | `lib/Epub/Epub/Section.cpp:88` | Individual page layout time | Per-page granularity |

### 3. How to Emit the Data

**Profiling gate:** `#ifdef BOOK_PROFILE` — not defined in any default build; must be explicitly added via `-DBOOK_PROFILE` in `platformio.ini`.

**Counter struct:** Lightweight, stack-allocated, zero-sized when `BOOK_PROFILE` is off:

```cpp
#ifdef BOOK_PROFILE
struct BookProfileData {
    uint32_t parse_start_ms = 0;
    uint32_t parse_end_ms = 0;
    uint32_t render_start_ms = 0;
    uint32_t render_end_ms = 0;
    uint32_t psram_free_before = 0;
    uint32_t psram_free_after = 0;
    uint32_t heap_free_before = 0;
    uint32_t heap_free_after = 0;
    uint8_t core_id = xPortGetCoreID();
    uint32_t sd_read_bytes = 0;
    uint32_t sd_read_duration_us = 0;
    uint32_t page_parse_ms = 0;
};
#endif
```

**Emission pattern:** Use existing `LOG_INF` / `LOG_DBG` macros:

```cpp
#ifdef BOOK_PROFILE
  BookProfileData prof;
  prof.parse_start_ms = millis();
  // ... do work ...
  prof.parse_end_ms = millis();
  prof.psram_free_before = ESP.getFreePsram();
  // ... more work ...
  prof.psram_free_after = ESP.getFreePsram();
  LOG_INF("PROF", "phase=parse core=%d parse_dur=%uus psram_free=%uB->%uB heap=%uB->%uB",
          prof.core_id,
          prof.parse_end_ms - prof.parse_start_ms,
          prof.psram_free_before, prof.psram_free_after,
          prof.heap_free_before, prof.heap_free_after);
#else
  // Do nothing - zero compiler-generated code
#endif
```

**Key constraints:**
- All counter fields are `uint32_t` — no `size_t`, no `uint64_t`, no `double`
- `millis()` wraps after ~50 days; acceptable for single-session profiling
- `xPortGetCoreID()` returns `BaseType_t` (0 or 1 on S3); fits in `uint8_t`
- No heap allocation — all counters are compile-time initializers
- When `BOOK_PROFILE` is not defined, the struct and all LOG_INF calls compile to **zero** bytes

### 4. What the Profile Must Reveal to Proceed to Phase 3

The profile data must satisfy **one** of the following conditions to green-light the dual-core parse offload:

| Condition | Action |
|---|---|
| `parse_duration_us` > 30% of `(parse_duration_us + render_duration_us)` **AND** `(psram_free_before - psram_free_after) / psram_free_before` < 70% | **Proceed** — parse is CPU-bound and PSRAM bandwidth is available; dual-core offload will help |
| `parse_duration_us` < 30% of `(parse_duration_us + render_duration_us)` **OR** `(psram_free_before - psram_free_after) / psram_free_before` >= 70% | **Defer** — parse is not the bottleneck, or PSRAM is heavily contended; stick with single-core |
| `sd_read_duration_us` > 500,000 us (500ms) per chapter | **Investigate SD-first** — SD I/O is the real bottleneck; optimize ZIP caching or HTML cache before considering dual-core |

**Rationale:** If parse time is a small fraction of total render time, adding another core won't improve the user-visible FPS. If PSRAM bandwidth is already saturated at 70%+ contention, a second core trying to allocate from PSRAM will likely cause more contention than gain.

### 5. How to Collect the Profile Data

1. **Build**: `pio run -e x4pro -DBOOK_PROFILE` (or any S3 environment: `sticky`, `papermono`, `x4c`)
2. **Flash**: Upload to X4 Pro device
3. **Open book**: "The Infinite and the Divine" EPUB (or any chapter-rich novel)
4. **Navigate**: 3-5 chapters from "Act Three" → "Chapter One" onwards
5. **Capture**: `pio run -e x4pro -t monitor` or `python3 scripts/debugging_monitor.py`
6. **Analyze**: Look for `PROF:` prefixed log lines in the serial output

**Expected log format (example):**

```
LOG_INF("PROF", "phase=parse core=1 parse_dur=425000us psram_free=128000B->96000B heap=182000B->165000B")
LOG_INF("PROF", "phase=render core=1 render_dur=285000us psram_free=96000B->85000B heap=165000B->158000B")
LOG_INF("PROF", "phase=page core=1 page_dur=45000us psram_free=85000B->83000B")
```

### Implementation Notes

- **Zero-overhead gate**: When `BOOK_PROFILE` is not defined, the `BookProfileData` struct and all `LOG_INF("PROF", ...)` calls are completely eliminated by the preprocessor. No `.text` or `.rodata` entries are emitted.
- **`micros()` vs `millis()`**: The codebase already uses `millis()` extensively. `micros()` would wrap too frequently for session-long profiling (wraps every ~71 minutes of continuous use). `millis()` wraps after ~50 days — acceptable for a reading session.
- **PSRAM tracking**: `ESP.getFreePsram()` is available on ESP32-S3 (X4 Pro, X4C, Paper Mono) but returns 0 on C3 (X4). The profiling data from C3 builds will show 0 PSRAM, which is expected and actionable (C3 cannot dual-core parse anyway due to no PSRAM).
- **Core ID**: `xPortGetCoreID()` is available from ESP-IDF and works in the Arduino framework. It returns 0 or 1 on dual-core devices.

### File Mapping for Instrumentation

| File | Key Functions to Instrument |
|---|---|
| `lib/Epub/Epub/Section.cpp` | `createSectionFile()`, `buildSomeMore()`, `onPageComplete()`, `startBuild()`, `finalizeBuild()` |
| `src/activities/reader/EpubReaderActivity.cpp` | `renderBook()`, `loadBook()`, `renderContents()` |
| `lib/Epub/Epub.cpp` | `load()`, `parseContentOpf()`, `parseCssFiles()` |
| `lib/Logging/Logging.h` | Already includes `xPortGetCoreID()` usage in `logMemAt()` — reuse pattern |