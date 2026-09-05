## Phase 3: Dual-Core Parse Profiling Instrument Design

### Context

The X4 Pro uses an ESP32-S3 (dual-core Xtensa LX7 @ 240MHz). Currently, EPUB parsing and rendering both run on core 1 inside `EpubReaderActivity::renderBook()`. Phase 3 proposes moving the parse task to core 0 while keeping render on core 1.

Per the design doc v3 §10(a), this is **gated on profiling data**. Before any dual-core code lands, we must profile to determine:
1. Whether parse is CPU-bound (dual-core would help) or cache-bandwidth/PSRAM-bound (dual-core may not help)
2. What the contention pattern looks like

### Task

You are working in the `design/epub-engine-mem-sdcard` branch at `/home/balor/workspace/eink/crosspoint-epub-engine-mem`.

**Your deliverable:** A design document that specifies the exact profiling instrumentation needed to gate the Phase 3 dual-core parse decision, plus the implementation of that instrumentation in the codebase.

### Step 1: Research (5 mins)

Look online for how other C++ EPUB engines on embedded/dual-core systems profile rendering pipelines. Search for:
- "esp32 dual core epub rendering profiling"
- "FreeRTOS profiling task core migration"
- "ESP32-S3 PSRAM bandwidth contention measurement"

Capture 2-3 concrete examples of profiling patterns you find.

### Step 2: Design the profiling instrumentation

Write a design doc at `docs/design/2026-09-05-epub-dual-core-profile-plan.md` that specifies:

1. **What to measure:**
   - Parse time per chapter (start/end timestamps via `micros()` or `esp_timer_get_nanoseconds()`)
   - Render time per page
   - PSRAM allocation per phase (heap_caps_get_free_size for MALLOC_CAP_SPIRAM before/after)
   - Core affinity at each pipeline stage (`xPortGetCoreID()`)
   - SD read byte count and read duration
   - Free heap at each stage

2. **Where to instrument:**
   - `Section::createSectionFile()` — start of chapter build
   - `Section::buildSomeMore()` — each incremental build step
   - `EpubReaderActivity::renderBook()` — parse vs render boundary
   - `Section::loadSectionFile()` — cache hit/miss path
   - `Epub::load()` — book open (OPF + TOC + CSS parse)
   - Each `Page` completion in `Section::onPageComplete()`

3. **How to emit the data:**
   - Use existing `LOG_INF` / `LOG_DBG` macros
   - Add counters: `LOG_INF("PROF", "phase=X core=%d psram_free=%uB duration=%uus", ...)`
   - Use a lightweight counter struct (no heap allocation)
   - Gate profiling behind `#ifdef BOOK_PROFILE` build flag (not default)

4. **What the profile must reveal to proceed to Phase 3:**
   - If parse is >30% of total render time AND PSRAM contention is low → dual-core parse is beneficial
   - If parse is <30% OR PSRAM contention is high (>70% of PSRAM bandwidth) → dual-core parse may not help; defer
   - Threshold values must be specified

5. **How to collect the profile data:**
   - Build with `pio run -e x4pro` + the profile flag
   - Flash to X4 Pro device
   - Open "The Infinite and the Divine" epub (attached at the worktree root or referenced)
   - Navigate through 3-5 chapters from "Act Three" → "Chapter One" onwards
   - Capture serial output via `pio run -e x4pro -t monitor` or `python3 scripts/debugging_monitor.py`

### Step 3: Implement the instrumentation

Add the profiling counters and LOG_INF statements to the actual source files. The instrumentation must:
- Be gated behind `#ifdef BOOK_PROFILE`
- Add zero overhead when the flag is not set
- Use `uint32_t` counters (no 64-bit unless needed for `micros()` wrap)

### Step 4: Verify

- Build all 5 environments with the new flag OFF (default): `pio run -e x4pro -e default -e x4c -e sticky -e papermono`
- Verify `clang-format` and `cppcheck` pass
- Confirm the `#ifdef BOOK_PROFILE` gate means zero code added in default builds
- Run host tests: `cmake -S test -B /tmp/test-build && make -C /tmp/test-build && ctest`

### Constraints

- Do NOT commit anything — this is a design + implementation in a worktree for review
- Do NOT modify Section.cpp beyond adding instrumentation (no logic changes)
- Do NOT touch test fixtures or Epub.cpp beyond adding profiling counters
- The instrumentation must be zero-overhead when `BOOK_PROFILE` is not defined
- Follow the project's coding standards (AGENTS.md): no `std::string` in hot paths, `makeUniqueNoThrow` for heap allocs, `LOG_INF`/`LOG_DBG` for output

### Files to read first

- `lib/Epub/Epub/Section.cpp` — especially `createSectionFile()`, `buildSomeMore()`, `onPageComplete()`
- `src/activities/reader/EpubReaderActivity.cpp` — `renderBook()` entry point
- `lib/Epub/Epub.cpp` — `load()` method
- `lib/Memory/Memory.h` — existing memory helpers
- `platformio.ini` — build flags and environments
- `docs/design/2026-09-03-epub-engine-pipeline-and-io.md` — the full design doc (sections C.1, D.5, D.7, E.3, §10)

### VERIFY

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH" && for env in default x4c sticky papermono x4pro; do echo "--- $env ---"; timeout 300 pio run -e $env 2>&1 | tail -5; done
```
Then clang-format and cppcheck:
```bash
./bin/clang-format-fix -g && pio check -e x4pro
```
Then host tests:
```bash
cmake -S test -B /tmp/test-build && make -C /tmp/test-build && ctest --test-dir /tmp/test-build
```
