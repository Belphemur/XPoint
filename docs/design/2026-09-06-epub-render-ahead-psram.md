## Design: PSRAM Render-Ahead Pipeline for X4 Pro

### Context

Profiling on X4 Pro (see `docs/design/2026-09-05-epub-dual-core-profile-plan.md` §5)
shows the render pipeline is **display-bound**, not CPU-bound:

| Phase | Duration | Type | Where CPU is free |
|---|---|---|---|
| Prewarm (font/SD prep) | 5-33ms | CPU | — |
| BW render (text to framebuffer) | 22-46ms | CPU | — |
| e-ink base refresh (DRF) | 649-1335ms | **Panel waveform** | ✅ entire duration |
| Grayscale LSB pass | 49-72ms | CPU | — |
| Grayscale MSB pass | 61-88ms | CPU | — |
| e-ink grayscale display | 286-293ms | **Panel waveform** | ✅ entire duration |
| BW restore | 42-44ms | CPU | — |

**Total per page: ~1100-1200ms, of which ~950-1400ms is e-ink display time.**
The CPU is idle for ~75% of each page turn, but Phase 4 (async display overlap) only
uses a fraction of that window for next-chapter prefetch.

### Goal

Eliminate the **649ms base-refresh wait** on X4 Pro by pre-rendering the next page
into a PSRAM framebuffer during the current page's panel refresh. Target: 30-40%
reduction in perceived page-turn latency, from ~1.1s to ~700ms.

### Key insight

The async display overlap (Phase 4) already proved that `displayBufferAsync()` +
`waitRefreshComplete()` gives us a 569-648ms window where Core 1 is free.
Currently we use that window only for prefetch of the **next chapter**.
The highest-yield optimization is to pre-render the **next page** of the **current
chapter** — but that requires a second framebuffer.

### Current memory layout on X4 Pro

The X4 Pro is ESP32-S3 with 8MB PSRAM. Current allocation:

```
DRAM (304KB total, ~208KB free for app):
├── Main framebuffer (48KB) — single-buffer mode (EINK_DISPLAY_SINGLE_BUFFER_MODE=1)
├── Stack + heap (~208KB)

PSRAM (8MB, ~8.1MB free):
├── _grayLsb (48KB) — grayscale LSB plane
├── _grayMsb (48KB) — grayscale MSB plane
├── _lastBw (48KB) — last BW frame for grayscale comparison
├── _pendingBw (48KB) — pending BW frame for grayscale
├── _asyncShadow (48KB) — lazily-allocated async shadow buffer
├── Section build cache (.bin on SD, 2×48KB planeBufFits during render)
└── EPUB metadata, CSS, TOC caches
```

The grayscale planes (`_grayLsb`, `_grayMsb`, `_lastBw`, `_pendingBw`) are allocated
in PSRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM)` in `PaperMonoDriver::allocateBuffers()`.
They are **only used during the grayscale pass** of the current page — after
the page is displayed, they sit idle until the next grayscale page.

### Proposed design

#### Option A: PSRAM second framebuffer (minimal change)

Allocate a **second 48KB framebuffer in PSRAM** and render the next page into it
while the current page's base refresh runs. Then when the user taps:

```
Current page:  displayBufferAsync() → [649ms panel refresh]
                                     ↳ Core 1 renders next page to PSRAM FB
Next tap:      renderContents() starts from the already-rendered PSRAM FB →
               displayBuffer(PSRAM_FB) → done, no 30ms re-render
```

**Changes required:**

1. **`lib/Memory/Memory.h`**: Add `makeUniqueNoThrowPsram<uint8_t[]>(BUFFER_SIZE)` helper
   (already exists — `makeUniqueNoThrowPsram<T>` with size param).

2. **`GfxRenderer.h` / `GfxRenderer.cpp`**: Add a `psramFrameBuffer` member
   (`uint8_t*` in PSRAM, owned by `std::unique_ptr` with custom deleter).
   Add `renderToPsramFrameBuffer()` that:
   - Renders the page content to `psramFrameBuffer` instead of `frameBuffer`
   - Uses `setFramebuffer()` semantics or a temporary framebuffer swap
   - Does NOT call any display functions

3. **`EpubReaderActivity.cpp`**: In the async display overlap window:
   ```cpp
   // After displayBufferAsync() for the base refresh:
   if (renderer.hasPsramFramebuffer()) {
     // Pre-render the NEXT page (not next chapter — same chapter, next spine page)
     renderNextPageToPsram();
   }
   renderer.waitRefreshComplete();
   ```

4. **Page cache hit on navigation**: When `goToPage(targetPage+1)` is called
   and a PSRAM framebuffer exists for it, skip `renderContents()` entirely:
   ```cpp
   if (cachedPsramPage == targetPage && renderer.hasPsramFramebuffer(targetPage)) {
     renderer.displayBuffer(HalDisplay::FAST_REFRESH);  // instant, no re-render
     return;
   }
   ```

#### Option B: PSRAM gray planes as render targets (more invasive)

The grayscale planes (`_grayLsb`, `_grayMsb`) are 48KB each and allocated in PSRAM.
On non-grayscale pages they are entirely unused during the base refresh. We could
reuse them as intermediate render buffers for the next page's BW framebuffer.

**Risk**: This would require deep changes to `PaperMonoDriver`'s internal state
management — the grayscale planes carry per-pixel dithering state, coverage
bitmaps, and generation counters. Mixing page N+1 render data into plane N's
storage could corrupt the grayscale pass for page N+1 itself.

**Verdict**: Not recommended — the risk of subtle ghosting/corruption bugs
outweighs the marginal memory savings (we'd reuse 2×48KB instead of allocating
1×48KB).

#### Option C: PSRAM section-build arena (already partially done)

The existing `lendBuildStorage()` and `borrowSecondaryBuffer()` paths already
lend PSRAM/DRAM buffers to section builds. We could extend this to hold a
full next-page framebuffer during the build phase — but section builds produce
`PageRender` structures, not raw framebuffers, so this doesn't directly map.

### Recommended approach: Option A (PSRAM second framebuffer)

**Sizing**: 48KB PSRAM for the PSRAM framebuffer (out of 8MB PSRAM, 0.6% overhead).
Current PSRAM free after app load: ~8081KB. 48KB is negligible.

**Allocation strategy**:

- Allocate once when the reader activity enters (`onEnter`) if `BOARD_HAS_PSRAM`
  is defined and `supportsAsyncRefresh()` returns true.
- Keep it warm for the lifetime of the reader activity — no per-page alloc/free
  (avoids fragmentation, per AGENTS.md §7.2).
- Gate the entire feature behind `#ifdef BOARD_HAS_PSRAM` (never behind a build
  env — C3/x4c lack PSRAM).
- If allocation fails (PSRAM full), silently fall back to current behavior (no
  pre-render, just async display as-is).

**Render path**:

The challenge: `GfxRenderer` has a single `frameBuffer` pointer that all drawing
primitives write to. To render the next page to a different buffer:

1. **Approach 1: `setFramebuffer()` swap** — `GfxRenderer` calls `display.setFramebuffer()`
   which does `memcpy(frameBuffer, newBwBuffer, bufferSize)`. But then we'd need to
   restore the original before display. This copies 48KB — ~5ms over SPI on X4.

2. **Approach 2: Dual frameBuffer pointers in GfxRenderer** — Add a `renderTarget`
   pointer that drawing primitives use instead of directly indexing `frameBuffer`.
   Default it to `frameBuffer`; when pre-rendering, set it to `psramFrameBuffer`.
   `getWriteTarget()` (line 407 of GfxRenderer.cpp) already returns `_stripActive
   ? _stripBuf : frameBuffer` — we can extend this to `renderTarget`.

3. **Approach 2 is cleaner** — single pointer indirection, zero copy. The drawing
   primitives already go through `getWriteTarget()` (line 592 of GfxRenderer.cpp:
   `uint8_t* target = frameBuffer;`). Change this to `renderTarget` and add a
   setter.

**Pre-render invocation**:

In `renderContents()`, after `displayBufferAsync()` returns:

```cpp
// Non-grayscale path only (grayscale uses store/restore, not async)
if (canAsyncDisplay) {
  renderer.displayBufferAsync(mode);
  renderer.setRenderTarget(renderer.getPsramFrameBuffer());  // switch to PSRAM FB
  renderPageToPsram(currentPage + 1);                         // pre-render next page
  renderer.setRenderTarget(renderer.getFrameBuffer());       // switch back
  renderer.waitRefreshComplete();
}
```

`renderPageToPsram()` loads the next page from the Section and runs the same
`renderPage()` path that `renderContents()` uses, but writes to PSRAM instead of
DRAM.

### Why this is safe

- **PSRAM read-modify-write concern**: The PSRAM framebuffer is only **written**
  to during pre-render (Core 1 drawing text/pixels). It is then **read** during
  `displayBuffer()` when the user navigates — but that display path already
  involves a blocking 569ms wait where no other PSRAM contention exists. The
  concern from the Phase 4 doc about PSRAM bandwidth applies to per-pixel RMW
  in the hot render path — here we render to it once (write), then display it
  once (read), which is the optimal access pattern.

- **Thread safety**: Core 0 is blocked in `waitRefreshComplete()`. The pre-render
  on Core 1 touches only the PSRAM framebuffer and the immutable Section/PageRender
  data — no shared mutable state with the display path.

- **Display path compatibility**: `displayBufferAsync()` on X4 (dual-buffer
  PaperMono driver) swaps `frameBuffer` to the secondary buffer internally. Our
  PSRAM framebuffer is a separate buffer entirely — we'd use
  `setFramebuffer(psramFrameBuffer)` to make the panel scan it, or copy it to
  the real framebuffer if that's safer.

### What doesn't overlap

- **e-ink waveform** — panel hardware, can't be shortened (569-648ms base, 623ms gray)
- **Grayscale passes** — already async via async display overlap (Phase 4)
- **Section cold build** — already overlapping via `prefetchNextChapterDuringDisplay()`
- **Image decoding (JPEG/PNG)** — panel-SPI bound, PSRAM won't help

### Sizing the opportunity

Current non-grayscale page turn: ~590ms
- BW render: ~30ms
- e-ink base refresh: ~560ms
- Grayscale display: ~0ms (not a grayscale page)

With PSRAM framebuffer + async display:
- Page N: display with `displayBufferAsync()`, Core 1 renders page N+1 to PSRAM FB
- Tap to turn: page N+1 already rendered → `displayBuffer(PSRAM_FB)` = ~30ms + refresh

**Savings**: ~30ms (next page render) + any remaining async overlap work hidden
→ ~560ms total instead of ~590ms. But the bigger win is when the **next chapter**
is also being cold-built — that work continues during the refresh.

For grayscale pages (current ~1100ms):
- BW base + gray display is already async (gray passes overlap)
- Pre-rendering next page in PSRAM during the 649ms base refresh: ~30ms saved
- The gray display pass (286ms) can't overlap with the next base refresh because
  they're sequential waveform operations on the same panel

**Net effect**: ~5-7% latency reduction for grayscale pages, ~5% for BW-only.
The real value is in **eliminating visible jank** — the user never sees a "render
pause" because the next page was already rasterized.

### Implementation plan

#### Step 1: Add PSRAM framebuffer to GfxRenderer

```cpp
// GfxRenderer.h
#ifdef BOARD_HAS_PSRAM
  uint8_t* psramFrameBuffer = nullptr;
  void freePsramFrameBuffer();
  bool allocPsramFrameBuffer();
  bool hasPsramFrameBuffer() const { return psramFrameBuffer != nullptr; }
  uint8_t* getPsramFrameBuffer() const { return psramFrameBuffer; }
#endif
```

In `.cpp`:
```cpp
bool GfxRenderer::allocPsramFrameBuffer() {
#ifdef BOARD_HAS_PSRAM
  if (psramFrameBuffer) return true;  // already allocated
  psramFrameBuffer = static_cast<uint8_t*>(
    heap_caps_malloc(frameBufferSize, MALLOC_CAP_SPIRAM));
  if (!psramFrameBuffer) {
    LOG_ERR("GFX", "OOM: PSRAM framebuffer (%u bytes)", frameBufferSize);
    return false;
  }
  memset(psramFrameBuffer, 0xFF, frameBufferSize);  // white
  LOG_INF("GFX", "PSRAM framebuffer allocated (%u KB)", frameBufferSize / 1024);
  return true;
#endif
  return false;
}
```

#### Step 2: Add renderTarget indirection

Change `getWriteTarget()` and all drawing primitives to use `renderTarget`
instead of directly indexing `frameBuffer`:

```cpp
// Private member (default = frameBuffer)
mutable uint8_t* renderTarget = nullptr;

void setRenderTarget(uint8_t* target) { renderTarget = target; }
uint8_t* getRenderTarget() const { return renderTarget ? renderTarget : frameBuffer; }
```

Then update `drawPixel`, `fillRectImpl`, `clearScreen`, etc. to use
`getRenderTarget()` instead of `frameBuffer`.

**Impact**: ~20 call sites in GfxRenderer.cpp change `frameBuffer` to
`getRenderTarget()`. The `invertScreen()` method also needs to use the target.

#### Step 3: Pre-render next page in renderContents

In `EpubReaderActivity::renderContents()`, within the `canAsyncDisplay` block:

```cpp
if (canAsyncDisplay) {
  renderer.displayBufferAsync(mode);
  
#ifdef BOARD_HAS_PSRAM
  if (renderer.hasPsramFrameBuffer() && currentPage + 1 < section->pageCount) {
    renderer.setRenderTarget(renderer.getPsramFrameBuffer());
    renderer.clearScreen(0xFF);
    
    // Render the next page to PSRAM
    if (section->loadPage(currentPage + 1)) {
      renderPageToRenderer(renderer, currentPage + 1, ...);
    }
    
    renderer.setRenderTarget(nullptr);  // back to frameBuffer
  }
#endif
  
  renderer.waitRefreshComplete();
}
```

#### Step 4: Use PSRAM framebuffer on navigation

In `EpubReaderActivity::pageTurn()` or `goToPage()`:

```cpp
#ifdef BOARD_HAS_PSRAM
if (renderer.hasPsramFrameBuffer() && targetPage == currentPage + 1) {
  // Copy PSRAM framebuffer content to DRAM framebuffer for async display
  renderer.setFramebuffer(renderer.getPsramFrameBuffer());
  renderer.displayBufferAsync(mode);
  renderer.waitRefreshComplete();
  // PSRAM FB now holds page N+2's pre-render
  return;  // skip normal renderContents()
}
#endif
```

Wait — `setFramebuffer()` copies into the real framebuffer (`memcpy`).
Actually for the async path we want to display directly from PSRAM. But
`displayBufferAsync()` operates on `frameBuffer` internally via the driver.
We'd need to either:

- **Copy PSRAM FB → DRAM FB** (48KB, ~5ms) then display normally
- **OR** have `displayBufferAsync()` accept a buffer parameter

The copy approach is simpler and safer: 5ms to copy 48KB via memcpy is
negligible compared to the 30ms re-render we're saving.

```cpp
// In renderContents, after the next-page was pre-rendered to PSRAM:
renderer.setRenderTarget(renderer.getFrameBuffer());  // back to DRAM FB
renderer.clearScreen(0xFF);
memcpy(renderer.getFrameBuffer(), renderer.getPsramFrameBuffer(), frameBufferSize);
renderer.displayBufferAsync(mode);
```

Actually, even simpler: `setFramebuffer()` in FreeInkDisplay already does
`memcpy(frameBuffer, bwBuffer, bufferSize)`. So we call
`renderer.display.setFramebuffer(psramFrameBuffer)` or add a method to
GfxRenderer that does this.

### Risk: thread safety

The pre-render writes to PSRAM while `waitRefreshComplete()` runs on the same
core — both are on Core 1. No cross-core contention. The Section/PageRender data
is read-only during pre-render (immutable after load). The PSRAM framebuffer is
exclusive to pre-render until it's copied to DRAM for display.

### Risk: PSRAM bandwidth

PSRAM on ESP32-S3: ~14 Mbps effective read bandwidth. 48KB framebuffer copy:
~27ms at 14 Mbps. But `memcpy` through the cache is faster in practice
(8KB cache lines, burst mode). Measured copy of similar buffers in the existing
grayscale path: ~1-2ms. Not a concern.

### Open questions

1. **`setRenderTarget` vs `setFramebuffer`**: The `setFramebuffer()` method in
   FreeInkDisplay copies data; `setRenderTarget()` in GfxRenderer redirects
   drawing. These are conceptually different. We need both:
   - `setRenderTarget()` to redirect drawing to PSRAM FB for pre-render
   - `setFramebuffer()` (or a new `displayFramebuffer(ptr)`) to display from
     PSRAM FB on navigation

2. **Cache invalidation**: If the user changes settings (font size, etc.)
   during a page turn, the PSRAM pre-rendered page is stale. Need to clear
   `psramFrameBuffer` on settings change.

3. **Section boundary**: If `currentPage + 1` is in the next chapter (page 0
   of next spine), the pre-render needs to load that section first. This
   overlaps with the existing `prefetchNextChapterDuringDisplay()` — we'd
   need to coordinate so prefetches don't fight for CPU time.

### References

- `docs/design/2026-09-06-epub-async-render-overlap.md` — Phase 4 async display overlap (current code)
- `docs/design/2026-09-05-epub-dual-core-profile-plan.md` §5 — profiling data
- `lib/GfxRenderer/GfxRenderer.h:407` — `setFramebuffer` for BW restoration
- `lib/GfxRenderer/GfxRenderer.cpp:592` — `getWriteTarget()` returns `frameBuffer` (single framebuffer today)
- `lib/GfxRenderer/GfxRenderer.cpp:1766-1781` — `displayBuffer` vs `displayBufferAsync`
- `freeink-sdk/libs/display/FreeInkDisplay/src/driver/PaperMonoDriver.cpp:167-175` — grayscale planes allocated in PSRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
- `freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp:380-384` — `allocFrameBufferStorage()` with `FREEINK_FB_PSRAM` fallback to `malloc`
- `lib/Memory/Memory.h` — `makeUniqueNoThrowPsram<T>()` helper already exists
- `platformio.ini:29` — `EINK_DISPLAY_SINGLE_BUFFER_MODE=1` (single framebuffer)

## OpenCode Review

*All citations below were verified against this tree (`design/epub-render-ahead-psram` branch) and the upstream `Free-Ink/freeink-sdk` @ `main` (the submodule is not checked out locally, so SDK claims were verified from GitHub).*

### 1. Technical feasibility

The core idea — a second 48KB buffer plus a `renderTarget` indirection so the next page can be rasterized while the panel waveform runs — is sound in principle and fits the X4 Pro's measured budget (569-648ms window vs ~50-90ms of render work, single core, single task). However, the document's model of the display path is wrong for the device it targets: X4 Pro links the SSD1677/UC8179/UC8279 drivers (`FREEINK_DEVICE_X4PRO` → `BoardConfig.h`), which are **single-buffer** (`EINK_DISPLAY_SINGLE_BUFFER_MODE=1` in `[base]`, platformio.ini:29, applies to every env) and use the deferred `displayStart()`/`displayFinish()` mechanism — there is **no** "dual-buffer PaperMono driver swap" as claimed in §"Why this is safe"; `PaperMonoDriver` is a different device (M5Stack Paper Mono) and in fact has `supportsAsyncDisplay() = false`. The §"Current memory layout" diagram is also wrong for X4 Pro: `_grayLsb/_grayMsb/_lastBw/_pendingBw` are not persistent driver PSRAM planes there — on X4 Pro the grayscale planes are app-side **transient DRAM heap buffers** allocated per page (`makeUniqueNoThrow<uint8_t[]>(planeBytes)`, EpubReaderActivity.cpp:1909-1910), and `FREEINK_FB_PSRAM` is 0 for X4PRO (only M5PAPER/PAPERMONO default it on), so the main 48KB framebuffer is DRAM as drawn, but via plain `malloc`, not the dual-buffer scheme described. Finally, the headline goal ("30-40% reduction, ~1.1s → ~700ms", line 26) contradicts the document's own sizing ("Net effect: ~5-7%", line 230); the realistic win is loadPage + rasterization time (~30-70ms) hidden per same-chapter forward turn.

### 2. Implementation gaps

- **The proposed `clearScreen(0xFF)` in the pre-render snippet is a live-corruption bug.** `GfxRenderer::clearScreen` does not go through `getWriteTarget()` — it calls `display.clearScreen()` → `FreeInkDisplay::clearScreen` → `memset(frameBuffer, ...)` (GfxRenderer.cpp:1680-1691) unless a *strip* target is active. With only `renderTarget` set, Step 3's snippet would wipe the **real framebuffer mid-refresh**, violating the noShadow contract (`displayBufferAsyncNoShadow`: "the framebuffer must stay untouched until waitRefreshComplete()", HalDisplay.h:42-46) and destroying the differential baseline. The pre-render clear must be a memset of the target itself (the existing `beginStripTarget` pattern shows the correct shape).
- **The renderTarget conversion surface is larger and different than "~20 call sites".** Direct `frameBuffer` users in GfxRenderer.cpp: `drawPixel` (line 592 — has its own inline strip logic duplicating `getWriteTarget`), `fillRectImpl` via `getWriteTarget` (1030), `invertScreen` (1756), `preserveImagePolarity` (1585-1600), `readFramebufferRegion`/`writeFramebufferRegion` (1799, 1816, 2020-2040), and the region-copy cache (2020-2040). Critically, the doc never mentions **`DirectPixelWriter`**, the hot per-pixel path the text renderer uses, which is seeded from `getWriteTarget()/getWriteOriginY()/getWriteRows()` (GfxRenderer.h:249-259) — if it isn't taught about `renderTarget`, the pre-render silently rasterizes into the live framebuffer.
- **"Skip `renderContents()` entirely" (Step 4) is not achievable without breaking reader features.** Per-turn bookkeeping happens in `renderBook`, not `renderContents`: `section->loadPage()` (EpubReaderActivity.cpp:1624, SD open + LUT seeks + `Page::deserialize`, Section.cpp:838-887), `currentPageFootnotes = std::move(p->footnotes)` (1648), `currentPageVisibleOffset` (1647), link capture, and reading-stats word counts (1650). A pre-rendered framebuffer cannot carry footnote/link/progress state; the consumer path must still run loadPage (or stash that state at pre-render time) — which also means the saved time is smaller than modeled.
- **Status bar and font prewarm are missing from the pre-render.** `renderContents` draws the status bar into the framebuffer (EpubReaderActivity.cpp:1839) with the page number derived from `section->currentPage` (renderStatusBar, line 2127) — pre-rendering page N+1 while `currentPage == N` bakes a **wrong page number** into the PSRAM buffer unless the method is parameterized. Likewise the pre-render must run inside a `FontCacheManager::createPrewarmScope()` (1794-1801) or CJK/SD-font glyph misses will fault one SD read per glyph during the window. Note the existing *idle prewarm* (loop(), lines 533-556) is scan-mode only (`drawText` records and returns, GfxRenderer.cpp:678-681) — it does not rasterize, so it is not a precedent for "already done".
- **Invalidation is underspecified.** The pre-rendered buffer goes stale on far more than "settings change" (open question 2): orientation changes mid-activity (`applyOrientation` via loop() lines 527-531), `applyReaderTextSettings` (2732), any navigation that is not exactly +1 in the same section (`pageTurn` backward, 1274-1291; `skipPages` 1301; `pendingPageJump`/anchor/percent jumps; end-of-book), partial-build reflow (`applyDeferredReposition` can remap `currentPage`), and `FrameBufferLoan` periods where `frameBuffer == nullptr` (renderBook:1449, 1493). A single `invalidatePreRender()` called from `renderBook()` entry plus `applyOrientation`/`applyReaderTextSettings` covers this, but the doc must say so.
- **The window is shared, and the doc's placement snippet doesn't integrate with the dominant path.** The async overlap block (1858-1874) already schedules `prefetchNextChapterDuringDisplay()` and `buildSomeMore`; worse, the claim "grayscale uses store/restore, not async" (§Pre-render invocation, line 168) is false: `canAsyncDisplay = supportsAsyncRefresh() && !pageHasImages` (1856) is independent of grayscale, so every AA text page runs the tiled-async path where the same window is consumed by two plane walks (49-72ms + 61-88ms) plus plane streaming. Pre-render must be ordered/budgeted *within* that block (and skip pre-render when `nextPage->hasImages()` — the next page's image decode would grab the `ImageBlock` PXC slot and blow the window). Also §"Why this is safe" says "Core 0 is blocked in `waitRefreshComplete()`" — no: everything runs on the same loop task/core; the real (simpler) safety argument is single-threadedness, at the price of pre-render time subtracting from prefetch budget.

### 3. Alternative approaches

- **Profile the split before building the machinery.** The doc never prices `Section::loadPage(N+1)` (SD open + 3 seeks + deserialize + per-element vector allocs, Section.cpp:838-887) vs rasterization. If loadPage dominates or is comparable, overlap **only** loadPage + footnote/link stash during the window (no PSRAM framebuffer, no GfxRenderer changes at all) and render normally at turn time — most of the win for a fraction of the risk. A `BOOK_PROFILE` phase tag (`phase=preload_next`) would answer this in an afternoon on hardware.
- **Consider the SDK's native dual-buffer route for the x4pro env instead of inventing a parallel buffer.** Dropping `EINK_DISPLAY_SINGLE_BUFFER_MODE` (plus `-DFREEINK_FB_PSRAM=1`) for x4pro gives `frameBuffer0/1` + `swapBuffers()` + `consumePrevFrameFor()` — a second PSRAM framebuffer with tested async/differential semantics already in `FreeInkDisplay`. Rejected here for good reason (blast radius across every single-buffer assumption in GfxRenderer, HAL, and grayscale cleanup), but it belongs in the doc as the evaluated alternative to Option A rather than being absent.
- **Refinements to Option A if pursued:** (a) an RAII `RenderTargetScope` mirroring `beginStripTarget/endStripTarget` (explicit strip-vs-renderTarget precedence) instead of bare setter calls; (b) own the PSRAM buffer in `EpubReaderActivity` (it's a reader feature) rather than in the shared `GfxRenderer`; (c) allocate via the existing `makeUniqueNoThrowPsram<uint8_t[]>` (Memory.h:75-84, `heap_caps_free` deleter) — the doc's Step 1 sample contradicts its own reference by using raw `heap_caps_malloc`, against AGENTS.md's RAII rule; (d) at consume time, keep `renderContents()`'s normal display/grayscale flow and just substitute the already-rasterized page, so the AA paths are untouched.

### 4. Risk assessment

- **Mid-refresh framebuffer corruption via missed write paths** — **High.** `clearScreen`, `invertScreen`, `preserveImagePolarity`, region snapshot/restore, and `DirectPixelWriter` must all honor `renderTarget`; any miss renders into the live framebuffer during the noShadow refresh → differential baseline corruption → ghosting artifacts that are intermittent and hard to reproduce. Mitigate with the RAII scope + a target-aware clear + an explicit review checklist of every `frameBuffer` reference (grep-able).
- **Stale/wrong page displayed after un-anticipated navigation** — **Medium.** Backward turns, TOC/bookmark/percent jumps, orientation or font changes, partial-build reflow, or last-page-of-chapter edge cases displaying a stale PSRAM frame. Mitigate with central invalidation (renderBook entry) + a stash key of {spineIndex, pageNumber, renderSpec, orientation}.
- **Window overrun / latency regression** — **Medium.** Next page has images (PXC decode), SD font faults during pre-render (no prewarm scope), or prefetch + pre-render + buildSomeMore together exceeding the 569-648ms window push page-turn latency *above* baseline. Mitigate: skip on `hasImages()`, prewarm scope, and a `millis()` budget check before starting the pre-render.
- **PSRAM pressure/fragmentation** — **Low.** 48KB once per reader session out of ~8MB, alongside existing PSRAM users (PixelCache band buffers, PixelCache.h:91; Epub caches, Epub.cpp:235/466/919/965/1125/1163); allocation failure must fall back to today's behavior (design already specifies this — keep it). Note the doc's "PSRAM ~14 Mbps effective" bandwidth figure (line 363) is internally inconsistent with its own "~1-2ms" measurement and off by orders of magnitude; real S3 octal PSRAM burst copies of 48KB are ~1-3ms, so this risk is genuinely negligible.

### 5. Concrete implementation outline

1. **Spike/profile first**: add `BOOK_PROFILE` phase tags around `section->loadPage(currentPage + 1)` and a full scan+draw of page N+1 inside the async window (temporary code), build `-e x4pro_profile`, confirm loadPage vs rasterization split and that both fit the window. Abort the design here if the combined budget exceeds ~150ms.
2. **GfxRenderer.h**: add `mutable uint8_t* renderTarget_ = nullptr;` plus RAII `RenderTargetScope` (ctor sets/restores, nesting-guarded, inert while `_stripActive`) mirroring `FrameBufferLoan` (GfxRenderer.h:413-424); document precedence `strip > renderTarget > frameBuffer`.
3. **GfxRenderer.cpp**: introduce one helper `uint8_t* activeWriteTarget() const { return _stripActive ? _stripBuf : (renderTarget_ ? renderTarget_ : frameBuffer); }` and convert: `drawPixel` (592), `fillRectImpl` (1030), `invertScreen` (1756), `preserveImagePolarity` (1585-1600), `readFramebufferRegion`/`writeFramebufferRegion` (1799/1816), `copyRegionToBuffer`/`copyBufferToRegion` (2020-2040); verify `DirectPixelWriter` seeding through `getWriteTarget()/getWriteOriginY()/getWriteRows()` picks up the new target (GfxRenderer.h:249-259).
4. **GfxRenderer**: add target-aware clear — `clearRenderTarget(uint8_t color)` that memsets `activeWriteTarget()` (never `display.clearScreen()`); add `getFrameBufferSize()` accessor if not already exposed.
5. **HalDisplay**: wrap the SDK method — `void setFramebuffer(const uint8_t* bwBuffer)` → `einkDisplay.setFramebuffer(bwBuffer)` (exists in FreeInkDisplay.cpp; `memcpy(frameBuffer, bwBuffer, bufferSize)`), so Step 4 consumption needs no SDK change.
6. **EpubReaderActivity.h**: own the buffer — `std::unique_ptr<uint8_t[], void(*)(uint8_t*)> psramFb` allocated in `onEnter()` via `makeUniqueNoThrowPsram<uint8_t[]>(renderer.getBufferSize())` (Memory.h:75-84); free in `onExit()`; add `preRenderNextPage()` and a small stash struct `{spineIndex, pageNumber, footnotes, links, visibleTextOffset}`.
7. **EpubReaderActivity.cpp — pre-render**: implement `preRenderNextPage()`: gates = `BOARD_HAS_PSRAM` + `renderer.supportsAsyncRefresh()` + `renderer.hasFrameBuffer()` + next page in same section bounds + `!nextPage->hasImages()` + `millis()` budget check; then `RenderTargetScope` → `clearRenderTarget(0xFF)` → `PrewarmScope` → `loadPage(N+1)` → extract footnote/link/offset stash → `page->render(...)` → parameterized `renderStatusBar(pageNumberOverride)`.
8. **EpubReaderActivity.cpp — wire into the window**: inside the `canAsyncDisplay` block (1858-1874), call `preRenderNextPage()` **before** `prefetchNextChapterDuringDisplay()` (pre-render is the higher-value consumer; prefetch continues on later ticks), then `waitRefreshComplete()` unchanged. Do not touch the framebuffer between `displayBufferAsync` and the wait.
9. **EpubReaderActivity.cpp — consume**: in `renderBook()`'s page-load block (1623-1651): if the stash matches `{currentSpineIndex, section->currentPage}` and the render spec/orientation are unchanged → skip `loadPage`, memcpy PSRAM FB → DRAM FB via the new `HalDisplay::setFramebuffer`, restore footnotes/links/offset from stash, and skip the rasterization portion while keeping the normal display/grayscale flow (store/restore, tiled, combined-base all untouched).
10. **Invalidation**: add `discardPreRender()` called at `renderBook()` entry, in `applyOrientation` (1186), in `applyReaderTextSettings` (2732), and on every `pageTurn`/`skipPages` branch that is not a same-section +1 (1233-1335); it must also be discarded around any `FrameBufferLoan`.
11. **Host-test compatibility**: keep every new allocation behind the existing `ESP_PLATFORM`/`BOARD_HAS_PSRAM` gates so the host unit-test build (Memory.h:1-9) compiles; `renderTarget` indirection itself is host-testable — add a GfxRenderer unit test asserting drawPixel/fillRect/clear land in the redirected buffer and none in `frameBuffer`.
12. **Device verification** (human tester): all 4 orientations, AA on/off, image pages, chapter crossing, backward turns, TOC/bookmark jumps, font-size change mid-read, partial-build long chapters, night mode toggle mid-window; monitor `ESP.getFreeHeap()`/`ESP.getFreePsram()` via the existing `logMemAt("page_turn")` hook; run `./bin/clang-format-fix -g` and `pio check` before review.