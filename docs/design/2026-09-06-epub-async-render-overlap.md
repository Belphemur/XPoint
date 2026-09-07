## Design: Async Display Overlap with Next-Page Pre-Render

### Context

Profiling on X4 Pro (see `docs/design/2026-09-05-epub-dual-core-profile-plan.md` §5)
shows the render pipeline is **display-bound**, not CPU-bound:

| Phase | Duration | Type |
|---|---|---|
| Prewarm (font/SD prep) | 5-33ms | CPU |
| BW render (text to framebuffer) | 22-46ms | CPU |
| e-ink base refresh | 569-1458ms | **Panel waveform** |
| Grayscale LSB pass | 49-72ms | CPU |
| Grayscale MSB pass | 61-88ms | CPU |
| e-ink grayscale display | 286-293ms | **Panel waveform** |
| BW restore | 42-44ms | CPU |

**Total per page: ~1100-1200ms, of which ~850-950ms is e-ink display time.**
The CPU is idle for ~75% of each page turn.

### Goal

Overlap the e-ink panel refresh (dead time) with **next-page layout pre-computation**
on core 0, targeting a 30-40% reduction in perceived page-turn latency.

### Constraints (verified from profiling)

1. **Single framebuffer** on X4 Pro — 48 KB DRAM. Can't render the next page's
   pixels while the current page is still being displayed.
2. **Single framebuffer** is the hard blocker for pixel-level pre-rendering.
   `GfxRenderer` uses one `frameBuffer` pointer; both pages can't occupy it.
3. **Async display is already supported** — `displayBufferAsync()` +
   `waitRefreshComplete()` exist in `HalDisplay` / `GfxRenderer`.
4. **`overlapRefresh` is already used** for the grayscale pass — but only when
   `tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages`.
5. **Background section build is idle-gated** — `loop()` calls
   `buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)` only when the reader is idle,
   not during an active page turn.

### Current page-turn flow (`EpubReaderActivity::renderBook()`)

1. Load page from SD/cached `.bin`
2. Render BW text to framebuffer (CPU: ~30ms)
3. `displayBuffer()` — **BLOCKING e-ink refresh (569-648ms)**
4. If grayscale: store BW buffer → render gray planes → `displayGrayBuffer()` → restore BW
5. Return; `loop()` picks up idle prewarm + background build next tick

**Step 3 is the idle period on core 0.** It lasts 569-1458ms.

### Proposed design: overlap display with next-section prebuild

#### Approach: async display for the base B/W refresh

Replace the blocking `render.displayBuffer()` calls in the page-turn path with
`displayBufferAsync()` when `renderer.supportsAsyncRefresh()` returns true.
This starts the e-ink waveform and returns immediately on core 1.

While the panel refreshes (569-648ms of dead time), core 0 can:
- Run the **background section build** for the *next** chapter if one exists
  and hasn't been built yet (cold-build overlap)
- **Pre-load the next chapter's EPUB metadata** (TOC, CSS, spine) so the tap
  to advance chapters is instantaneous

**Why not pre-render the next page's pixels?** Single framebuffer. The next
page needs the framebuffer to render, but the current page is still being
sent to the panel. We could use `storeBwBuffer()` to snapshot, render next
page, then restore — but that's the existing grayscale pattern and only
works because grayscale planes are written via `beginStripTarget` to external
buffers, not the framebuffer.

**Why not pre-render on a separate core to a PSRAM buffer?** PSRAM bandwidth
is limited (~14 Mbps effective). The framebuffer is 48KB; a PSRAM copy on
every page turn adds allocation/deallocation overhead. This was tried in the
Phase 2 PSRAM experiments and caused fragmentation — not worth repeating.

#### Concrete changes to `EpubReaderActivity::renderContents()`

1. **Gate the base display on `supportsAsyncRefresh()`**: when true, call
   `displayBufferAsync()` instead of `displayBuffer()` for the base B/W refresh
   (currently line 1766/1771/1773).

2. **After firing async display, immediately yield to background build**:
   call `runBackgroundBuild()` — a new method that runs
   `section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)` for the *next*
   chapter's section if one is pending.

3. **Insert `waitRefreshComplete()` before any framebuffer-touching work**:
   The grayscale pass already does this (line 1808). The non-grayscale path
   needs one too — but in the non-grayscale case the grayscale block is skipped,
   so `waitRefreshComplete()` should be at the end of `renderContents()` when
   async was used and no grayscale pass follows.

4. **Next-page prewarm during async display**: move the idle prewarm logic from
   `loop()` into a `runPrewarmDuringRefresh()` call, so font/SD pre-warming
   for the next page happens on core 1 while the panel refreshes on core 1
   (the display refresh is driven by the panel controller, not CPU-bound).

#### What actually overlaps

```
Core 1:  [BW render 30ms] → [displayBufferAsync → panel starts] → 
          [font prewarm for next page: 5-33ms] → [waitRefreshComplete] →
          [grayscale render if needed] → [restore BW] → ...

Core 0:  [background section build: 55-522us per chunk] → idle →
          [next-chapter metadata prefetch] → idle
```

#### What we CANNOT overlap (constraints)

- **Page rendering** (BW text to framebuffer) — needs the framebuffer which
  is busy driving the panel
- **Grayscale plane rendering** — already overlaps via the existing tiled
  grayscale async path (uses external PSRAM buffers for planes)
- **e-ink waveform** — panel hardware, can't be shortened

#### Sizing the opportunity

If the base refresh is async and the grayscale pass runs as it does now
(also async, already overlapping), the only CPU work that extends the page
turn beyond the panel time is:
- Prewarm: ~5-33ms (can overlap with panel refresh)
- BW render: ~30ms (before display, not overlapping)
- Wait for refresh complete: this already happens at `tWait` in the grayscale path

For non-AA pages (the common case): current ~590ms total → could drop to
~569ms (just the panel time) if prewarm is fully hidden. That's ~3.5% improvement.

For AA+grayscale pages: current ~1100ms → the 569ms base + 286ms gray display
= 855ms of panel time, plus 150ms of CPU work. If we hide the prewarm (~20ms)
in the base refresh, we save ~20ms. That's ~1.8% improvement.

**The win is smaller than expected because the existing async grayscale
already handles the big gap.** The remaining opportunity is:

1. **Next-chapter cold build during display** — when navigating to a new
   chapter, the ~3s cold build can start on core 0 during the first page's
   display refresh, saving up to ~569ms of the ~3s build time.

2. **Full-refresh cadence tuning** — `pagesUntilFullRefresh` defaults to
   `SETTINGS.getRefreshFrequency()`. Reducing full-refresh frequency from
   every-N to every-2N would save ~400ms per full refresh.

### Implementation plan

#### Change 1: `ReaderUtils.h` — async base refresh wrapper

Add `displayBaseWithRefreshCycleAsync()` that calls `displayBufferAsync()`
instead of `displayBuffer()`, for the base refresh path only (before grayscale).

```cpp
// In ReaderUtils.h, alongside displayBaseWithRefreshCycle():
inline void displayBaseWithRefreshCycleAsync(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (!renderer.combinesGrayscaleBase()) {
    displayWithRefreshCycle(renderer, pagesUntilFullRefresh);  // non-async fallback
    return;
  }
  const auto mode = (pagesUntilFullRefresh <= 1) ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  renderer.displayGrayscaleBase(mode);  // already async on X3; blocking on X4 (but X4 doesn't grayscale-base)
  if (pagesUntilFullRefresh <= 1) {
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    pagesUntilFullRefresh--;
  }
}
```

#### Change 2: `EpubReaderActivity::renderContents` — overlap prewarm

In the non-tiled grayscale path (line 1773), after `displayWithRefreshCycle`,
insert a `prewarmNextPage()` call. In the tiled grayscale path, the async
overlap is already handled — just ensure `waitRefreshComplete()` is called
before the first grayscale strip write.

#### Change 3: `EpubReaderActivity::loop()` — background build during page turns

The idle prewarm at line 510 already runs `buildSomeMore` during idle. Extract
a `runBackgroundBuild()` that can be called during the async display gap:

```cpp
void EpubReaderActivity::runBackgroundBuild() {
  if (section && section->isBuilding() && !RenderLock::peek() && buildTickHeapGate()) {
    section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK);
  }
}
```

Call this during the `waitRefreshComplete()` window in `renderContents`.

### Risk: thread safety

Moving any work to core 0 requires auditing `Section`, `ChapterHtmlSlimParser`,
`CssParser`, and the `GfxRenderer` for shared-state access. The current
code is entirely single-threaded. The proposed changes **do not** move any
existing work to core 0 — they only add **new** background work (next-chapter
metadata prefetch, idle prewarm overlap) that doesn't touch the framebuffer
or section state.

A future Phase 4 (`x4pro_perf` env) could explore moving the *entire*
`createSectionFile()` blocking loop to core 0, but that requires a
thread-safety audit of `ChapterHtmlSlimParser` (shared expat state, LUT, CSS
cache) — out of scope for this change.

### Open question: next-chapter prefetch

`Epub::getSpineItem(nextSpineIndex)` + `Section::loadSectionFile()` for the
*next* chapter could run on core 0 during the current page's display refresh.
This is read-only on shared EPUB data (spine/TOC/manifest are immutable after
load) and touches only the next chapter's `.bin` (not the current one).
Needs verification against `BookMetadataCache::isMetadataCacheValid()`.

### References

- `docs/design/2026-09-03-epub-engine-pipeline-and-io.md` §10 — original
  Phase 3/4 roadmap
- `docs/design/2026-09-05-epub-dual-core-profile-plan.md` §5 — profiling data
- `lib/GfxRenderer/GfxRenderer.cpp:1704-1730` — displayBuffer / displayBufferAsync
- `lib/GfxRenderer/GfxRenderer.h:359-367` — storeBwBuffer / restoreBwBuffer
- `src/activities/reader/ReaderUtils.h:157-193` — displayWithRefreshCycle
- Profiling logs: `logs.profiling-1.txt` (attached)