## Design: PSRAM Font Cache Expansion for X4 Pro

### Context

Profiling on X4 Pro (`phase=preload_next`, PR #63) across 20 page turns revealed:

| Metric | Observed | Budget |
|--------|----------|--------|
| `preload_next_total` (loadPage + render N+1) | 42–162ms, mean ~89ms | 526-1375ms overlap |
| `loadPage(N+1)` alone | 11-34ms, mean ~16ms | — |
| `rasterize` alone | 29-138ms, mean ~71ms | — |
| `async_display_overlap` (idle window) | 526-1375ms, mean ~660ms | — |

The heavy pages (106-162ms) correlate with SD font glyph misses. Device logs show:
```
[DBG] [SDCF] Overflow: loaded U+%04X style %u on demand (slot %u/%u)
```

### Root cause (corrected per OpenCode review)

The profiling shows heavy pages (106-162ms) correlate with `Overflow: loaded U+XXXX on demand`
log lines. Two root causes, in order of impact:

1. **`MAX_PAGE_GLYPHS = 512` cap exceeded**: `prewarmStyle` accumulates resident glyph coverage
   in per-style mini arenas capped at 512 codepoints (SdCardFont.h). CJK books whose pages
   draw >512 unique glyphs in aggregate exceed this cap → the union is abandoned → every page
   turn re-reads up to 512 glyph records + bitmaps from SD (SdCardFont.cpp:1118-1214).

2. **Overflow ring of 8 entries too small**: glyphs not covered by prewarm fault through
   `onGlyphMiss` (SdCardFont.cpp:1593-1675), each costing ~5-20ms (HalFile open + 2 seeks + read).
   Per-miss cost is far lower than the original doc's 94ms claim — that was the aggregate
   per-page delta across multiple misses, not per-miss latency.

**The overflow ring is secondary.** The primary fix for CJK heavy pages is PSRAM-backed mini
arenas with a raised codepoint cap, which lands pages in the zero-SD covered-hit fast path
(SdCardFont.cpp:950-981). The overflow ring expansion helps for non-CJK SD-font cases
where the working set is <512 glyphs but spread across many pages.

### Goal

Move the hot-path font caches from DRAM to PSRAM on X4 Pro (ESP32-S3, 8MB PSRAM), expanding their capacities to eliminate per-page-glyph SD reads during render. Target: eliminate the 106-162ms heavy-page stalls by pre-warming more glyphs in the async overlap window and keeping them resident.

### Current font cache architecture

```cpp
// FontDecompressor (lib/EpdFont/FontDecompressor.h)
PageSlot pageSlots[MAX_PAGE_SLOTS=4];  // 4 prewarm slots, DRAM, freed between pages
uint8_t* hotGroup;      // one decompressed group, DRAM
uint8_t* hotGlyphBuf;     // single glyph scratch, DRAM

// SdCardFont (lib/EpdFont/SdCardFont.h)
OverflowEntry overflow_[OVERFLOW_CAPACITY = 8];  // 8-entry ring, DRAM — on-demand glyph cache
AdvanceEntry* advanceTable_[4];    // 768 entries × 6 bytes = 4.5KB/style, DRAM
```

**Current DRAM footprint per SD font per style**: ~4.5KB (advance table) + 8 × ~180B (overflow) = ~5KB+
**Current PSRAM usage**: Unknown — Arduino-ESP32 S3 may already spill allocations >~16KB to PSRAM via `CONFIG_SPIRAM_USE_MALLOC`. Must snapshot `heap_caps_get_info` before assuming baseline.

### Proposed design

#### Option A: PSRAM mini arenas + capacity expansion (recommended)

This is the re-ordered plan per the OpenCode review. The primary fix targets the
`MAX_PAGE_GLYPHS = 512` cap that causes CJK heavy-page stalls.

1. **Raise `MAX_PAGE_GLYPHS` to 2048 on PSRAM boards** (mini arenas → PSRAM)
   - miniIntervals/miniGlyphs/miniBitmap move to PSRAM
   - Cost: ~300-500KB worst case (2048 × 16B glyphs × 4 styles + bitmaps)
   - Well within 8MB budget (<6%)
   - Eliminates `PREWARM_ARENA_TOO_LARGE` failure path on PSRAM boards
   - Lands heavy CJK pages in the zero-SD covered-hit fast path

2. **Expand overflow ring from 8 to 64 entries** (PSRAM, shared across styles)
   - Corrected sizing: `OverflowEntry` ≈ 28B + bitmap (~50-150B) ≈ 75-180B/entry
   - Total: ~6-15KB (not the original doc's 60KB which assumed per-style)
   - Eliminates most `overflow: loaded U+XXXX on demand` hits for non-CJK books

3. **Expand advance table from 768 to 4096 entries** (PSRAM)
   - 4096 × 6 bytes = 24.6KB per style × 4 styles = ~100KB PSRAM
   - Eliminates `buildAdvanceTable: +%u from SD` misses

4. **Move `fullIntervals` to PSRAM** (codepoint coverage tables for large CJK fonts)

**Total PSRAM impact**: ~0.5MB worst case (300KB mini arenas + 100KB advance + 15KB overflow + 20KB intervals) — ~6% of 8MB

#### Option B: Full PSRAM migration (aggressive, NOT recommended)

Move ALL font caches to PSRAM including hot-path render buffers (`hotGroup`,
`hotGlyphBuf`). **Rejected** — the hot render path consumes glyph bitmaps sequentially,
which the S3 PSRAM cache serves at near-SRAM throughput. There is no measurable benefit
to moving DRAM hot-path buffers, and it adds complexity + risk.

### Recommended approach: Option A (re-ordered per review)

**Step 0 → Step 4 → Step 2 → Step 3.** The mini arena cap (step 4) is the primary
CJK fix; the overflow ring (step 2) is secondary but still valuable for non-CJK
SD-font books. The OpenCode review's re-ordering recommendation is adopted.

1. **Mini arenas + raised `MAX_PAGE_GLYPHS`** — primary: lands CJK pages in the
   zero-SD covered-hit path. Eliminates the `PREWARM_ARENA_TOO_LARGE` retry path
   on PSRAM boards.

2. **Overflow ring expansion** — secondary: eliminates per-miss SD reads for
   non-CJK books where the working set is <512 glyphs but spread across pages.

3. **Advance table expansion** — tertiary: eliminates batched SD reads for layout
   measurement on large-text pages.

The profiling measured 138ms rasterize on heavy pages. The 5-20ms per-miss cost
means a page with 5-10 overflow misses costs 25-100ms — the ring expansion saves
that. The CJK arena cap causes the larger stalls (re-reading 512 glyphs per page);
raising it to 2048 converges those pages to the covered-hit path, which the
profiling shows costs ~0-5ms (zero-SD reads).

### Implementation plan

**Re-ordered per OpenCode review**: instrument → mini arenas → overflow ring → advance tables.
The plan below references the OpenCode review's step numbering where applicable.

### Sizing the opportunity

Current heavy page cost: 138ms rasterize (with SD font misses)

**Mini arena expansion (primary fix for CJK):**
- Currently: pages with >512 unique glyphs per style trigger full re-read of
  up to 512 glyph records + bitmaps from SD per page turn
- With PSRAM-backed 2048-glyph arenas: CJK pages converge to the covered-hit
  fast path (`SdCardFont.cpp:950-981`) = ~0-5ms (zero SD reads)
- **Expected result**: 138ms → ~5ms on heavy CJK pages (96% reduction)

**Overflow ring expansion (secondary, for non-CJK SD-font books):**
- Current: 8-entry ring, each miss = ~5-20ms (HalFile open + 2 seeks + read)
- With 64-entry PSRAM ring: most misses become cache hits
- **Expected result**: 25-100ms of SD I/O eliminated per page with misses
- Light pages (42ms): minimal impact (already mostly cache hits)

### What doesn't change

- No GfxRenderer modifications needed (this is pure data-cache expansion)
- No display path changes (no `clearScreen` corruption risk)
- No invalidation complexity (font caches already handle font unload via `freeAll()`/`clearPersistentCache()`)
- `BOOK_PROFILE` instrumentation already exists and can measure the delta
- **Host unit tests cannot cover this** — `test/font_cache_manager/` uses stubs for `SdCardFont` and `FontDecompressor` (stubs/SdCardFont.h, stubs/FontDecompressor.h). The real classes are never built on host. All new behavior is device-verified only.

### Risks

| Risk | Level | Mitigation |
|------|-------|------------|
| Pool-mismatched free | High | One alloc/free convention for the feature; pool-aware `ensureArrayCapacity`; debug-time pool check |
| PSRAM-absent boot on x4pro | Medium-High | Runtime probe (`psramFound()` + canary); PSRAM absent → current DRAM capacities |
| PSRAM bandwidth contention with grayscale | Low | Font reads are small/sequential; S3 PSRAM cache absorbs them; hot group stays in DRAM |
| CJK arena fragmentation | Medium | `ensureArrayCapacity` grow-only pattern already proven; test with 4-orientation CJK book |
| Host/CI regressions | Low | All new code gated behind `BOARD_HAS_PSRAM`; host tests use stubs; verify `-e default` builds |
| Cache coherency on font unload | Low | `freeAll()`/`clearPersistentCache()` already handle eviction; must become pool-aware |

**Note on the `makeUniqueNoThrowPsramArray` helper**: the OpenCode review confirmed
the existing `makeUniqueNoThrowPsram<uint8_t[]>(count)` overload handles array sizing
correctly (Memory.h:77-84). No new helper needed — just use the existing one with an
explicit count parameter.

### Open questions

1. ~~Should the overflow ring store glyph data only, or also advance metrics?~~ — Answered: per-entry bitmaps are the payload, not advance metrics. The advance table is separate (step 3).

2. **What's the optimal overflow capacity?** 64 is a guess — the instrumentation step (step 0) should measure miss patterns to find the knee. Could try 16/32/64/128.

3. ~~Should we move `fullIntervals` to PSRAM?~~ — Answered: yes, on PSRAM boards (step 4). The `fullIntervals` table is a codepoint coverage map used by `findGlobalGlyphIndex`; for CJK fonts with thousands of intervals, this is significant DRAM.

4. **What's the optimal `MAX_PAGE_GLYPHS` cap?** 2048 is a proposal — could measure with `prewarmTotalGlyphs` stat. Could try 1024/2048/4096.

## OpenCode Review

*All citations below were verified against this tree at commit `affd52dc` ("docs: PSRAM font cache expansion design doc"), branch `feature/font-cache-psram`. Line references are to the working tree at that commit.*

### 1. Technical feasibility

The core idea is sound and the platform supports it: `[env:x4pro]` enables OPI PSRAM and `-DBOARD_HAS_PSRAM` (platformio.ini:276-288), and several other environments (x4pro_profile, x4c, papermono, sticky variants) carry the same flag, so the gate must be the macro, not the env name — the doc already gets this right in plan item 1.

Feasibility per cache:

- **Overflow ring bitmaps** (SdCardFont): per-miss reads are 16 B glyph metadata + ~50-150 B bitmap via a sequential seek/read pair (SdCardFont.cpp:1593-1675). Moving the resident copies to PSRAM costs nothing measurable — the read path is sequential and cache-served on the S3. Feasible and worthwhile; SD I/O (open + 2 seeks + 2 reads through the HalStorage mutex) dominates each miss by orders of magnitude over any memory-latency delta.
- **Advance tables**: 6 B `AdvanceEntry` (uint32 codepoint + uint16 advanceX), built by batched sequential reads (SdCardFont.cpp:1390-1490). Pure capacity problem; PSRAM placement is trivially fine.
- **Option B rejection**: directionally right (don't move the hot render path), but the stated math is wrong. "PSRAM read latency is ~80ns vs ~10ns... 70ns × glyphPixels per glyph" (line 72) conflates random pointer fetches with data streaming. `FontDecompressor::getBitmap` (FontDecompressor.cpp:143-221) and the SDCF blit path consume bitmaps sequentially (~64-150 B per glyph), which the S3 PSRAM cache serves at near-SRAM throughput; per-pixel rasterization reads from the DRAM framebuffer, not from glyph storage. The conclusion (keep `hotGroup`/`hotGlyphBuf` in DRAM) still stands — there is no measured benefit to moving them — but the justification should be "no benefit, added complexity", not a per-pixel latency claim that doesn't model how the data is read.

One feasibility caveat the doc underweights: **PSRAM availability is per-boot dynamic on this hardware.** `src/main.cpp:570-589` documents that PSRAM init can fail silently on some X4 Pro units during early boot, with a retry in `setup()`. `heap_caps_malloc(MALLOC_CAP_SPIRAM)` can therefore return `nullptr` on an x4pro build. The doc's fallback item (plan item 5) is not an edge case — it is a first-class path that will execute on real devices and must be tested as such (see §4).

### 2. Implementation gaps

**Sizing errors (correct before implementation):**

- **The overflow ring is one shared ring, not per-style.** `SdCardFont.h` declares `OVERFLOW_CAPACITY = 8` (not 7 as the doc states) as a single `overflow_[8]` array shared across all four styles, with a per-entry `styleIdx`. The doc's "60KB for 4 styles" (15 KB/style) therefore overstates by ~4×. Corrected math: `OverflowEntry` = `EpdGlyph` + bitmap pointer + codepoint + styleIdx ≈ 28 B, plus the bitmap allocation (~50-150 B for CJK) ≈ 75-180 B/entry → a 64-entry ring costs roughly **6-15 KB total**, not 60 KB. The DRAM relief is real but an order of magnitude smaller than claimed.
- **`EpdGlyph` is 16 B, not 32 B** (EpdFontData.h:131-139: width u8, height u8, advanceX u16, left i16, top i16, dataLength u16, dataOffset u32; the struct is not packed but is still 16 B on device and host). This propagates into the per-entry estimate.
- **"Each miss costs ~94ms (SD seek + read + decompress)" (line 76) is unsupported.** The actual per-miss work is one `HalFile` open + two seeks + a 16 B read + a `dataLength` read ≈ **5-20 ms** on SPI SD. "Decompress" does not occur in this path at all: SD `.cpfont` files (version 4) store bitmaps uncompressed; `onGlyphMiss` (SdCardFont.cpp:1593-1675) is a pure copy. The 94 ms figure is best explained as the *aggregate per-page delta across several misses* — which the doc's own log quote (multiple U+XXXX loads per page) supports. This matters for sizing: at 5-20 ms/miss, a page with 5 misses costs 25-100 ms, and the number of misses per page (not per-miss latency) is the lever.
- **The quoted device log is embellished.** The actual line is `"Overflow: loaded U+%04X style %u on demand (slot %u/%u)"` (SdCardFont.cpp:1671). There is no "ring buffer full, reading from SD" text. Cite the real format string so future greps find it.
- **Plan item 3 (page slots 4→8) is based on a semantic error.** `FontDecompressor` page slots are `MAX_PAGE_SLOTS = 4`, documented "One per font style (R/B/I/BI)" (FontDecompressor.h) — they are indexed by *style within a single `PrewarmScope`*, allocated in `prewarmCache` and freed only at scope end via `clearCache` (FontCacheManager.cpp:54-75). They are not a per-page-turn cache; a scope never touches more than 4 styles, so 4→8 can never help current call sites. Moreover, the SDCF path the profiling blames doesn't use page slots at all. This item should be dropped or re-scoped. The doc is also internally inconsistent here: "~5-20KB each" vs "5KB page slots" total (line 61) — 4→8 slots at 5-20 KB each is 20-160 KB, not 5 KB.
- **Plan item 4 (new `makeUniqueNoThrowPsramArray` helper) fixes a non-bug.** The existing unbounded-array overload (Memory.h:75-84) takes a count and multiplies by `sizeof(Elem)`; for `uint8_t[]` there is no `sizeof(T)=1` issue. For this plan the helper is unnecessary.
- **Plan item 1's "same pattern as existing PSRAM code" is ambiguous.** The existing pattern in this tree is raw `heap_caps_malloc`/`heap_caps_free` (Epub.cpp:468, PixelCache.h:96, HomeActivity.cpp:171); `makeUniqueNoThrowPsram` (Memory.h) exists but has **zero call sites**. Pick one convention explicitly (see §4, pool-mismatched free).

**Structural gaps the plan doesn't cover:**

- `overflow_` is a fixed member array; expansion requires pointer + runtime capacity, touching `onGlyphMiss` (:1593-1675), `isOverflowGlyph`/`getOverflowBitmap` (:1677-1691), `clearOverflow` (:212-216), and `releaseResidentCaches` (WiFi start drops the overflow ring). Per-entry **bitmaps** must move to PSRAM too — they are the bulk of the memory; moving only the entry structs saves almost nothing.
- **Fallback must select capacity by pool, not just placement.** A DRAM fallback at expanded capacities (64-entry ring + 4096-entry advance tables ≈ 24 KB×4 styles) would OOM on low-heap boards. Correct semantics: PSRAM present → expanded capacity in PSRAM; PSRAM absent → current capacity in DRAM (8/768).
- `FontDecompressor`: the plan moves only page-slot *buffers*. `PageGlyphEntry` arrays (12 B/glyph, 512 max) and the per-group decompression scratch `tempBuf = malloc(group.uncompressedSize)` (FontDecompressor.cpp:471, freed :482/:499) are unaddressed. The ~4.8 KB of `prewarmCache` stack arrays (`neededGlyphs[512]`, `neededGroups[128]`, `groupAlignedTracker[128]`, .cpp:251-506) should be explicitly declared out of scope.
- **No instrumentation exists to size the ring or validate ROI.** There is no counter for overflow misses per page, unique miss codepoints per page, or per-miss µs. Open question 2 admits 64 is a guess; without counters, it stays unfalsifiable. Add stats before expanding (see §3.1 and §5.0).
- **Host tests can't cover this.** "Host unit tests need no changes" (line 103) is technically true but only because `test/font_cache_manager/FontCacheManagerTest.cpp` compiles *stubs* (test/font_cache_manager/stubs/FontDecompressor.h, stubs/SdCardFont.h) — the real classes are never built on host. All new behavior is device-verified only; say so explicitly.
- `clearPersistentCache` and the `ensureArrayCapacity` grow path (delete[] + `new(std::nothrow)`, SdCardFont.cpp:85-91) must become pool-aware so a mixed DRAM/PSRAM cache is freed with the matching deallocator.

**The biggest gap: overflow expansion doesn't fix the CJK working-set problem the profile actually shows.** `prewarmStyle` accumulates resident coverage in per-style mini arenas whose union is capped at `MAX_PAGE_GLYPHS = 512` codepoints (SdCardFont.h). For a CJK book whose pages draw more than 512 unique glyphs in aggregate, the union is abandoned and every page turn re-runs the request-only rebuild path — up to 512 glyph records + bitmaps re-read from SD per turn (SdCardFont.cpp:1118-1214) — *plus* every uncovered glyph faults through `onGlyphMiss`. Against that workload, a 64-entry FIFO ring still thrashes: it converts N misses into N misses (just with a longer eviction horizon). The change that actually converges heavy CJK pages is more resident coverage: PSRAM-backed mini arenas with a raised codepoint cap, which lands every page in the covered-hit fast path (SdCardFont.cpp:950-981 — zero SD reads) and also deletes the `PREWARM_ARENA_TOO_LARGE` failure/retry path (:1191, triggered when the DRAM arena doesn't fit the largest free block). The doc mentions this territory only as open question 3 (`fullIntervals`), which undersells it: it is arguably the primary fix, and the overflow ring is secondary.

### 3. Alternative approaches

1. **Measure-first (do this regardless).** Add `BOOK_PROFILE` counters: overflow misses per page, unique miss codepoints per page, cumulative per-miss µs, and prewarm rebuild size (codepoints + bytes read). This sizes open question 2 with data, distinguishes "many distinct misses" (ring helps) from ">512-unique-glyph working set" (only residency helps), and validates the projected 138→20 ms. One profiling session re-orders the plan cheaply.
2. **PSRAM mini arenas + raised `MAX_PAGE_GLYPHS` (recommended primary).** Gate the cap: PSRAM boards → 2048+ (cost ≈ a few hundred KB worst case, still <5% of 8 MB), DRAM boards → unchanged 512. Moves miniIntervals/miniGlyphs/miniBitmap (+ optional `fullIntervals`) allocations to PSRAM. Effect: heavy CJK pages converge to the zero-SD covered-hit path; `PREWARM_ARENA_TOO_LARGE` becomes dead code on PSRAM boards. This addresses the profiled workload directly instead of damping its symptoms.
3. **LRU eviction instead of FIFO** (if the ring is kept): the linear scan in `isOverflowGlyph`/`onGlyphMiss` already visits the slot; bumping a timestamp is free. FIFO evicts glyphs that repeat across adjacent pages.
4. **Scoped Option B**: mini arenas + overflow + advance tables + `fullIntervals` → PSRAM; keep `hotGroup`/`hotGlyphBuf`/page slots in DRAM. Once the fallback plumbing (per-pool capacity, runtime probe) is being written anyway, the marginal cost of including the arenas is small and the win is strictly larger than Option A alone.
5. **SD-persisted advance/coverage cache** under `.crosspoint/` (per font + render settings): amortizes table builds across sessions. Heavier — it crosses into `docs/file-formats.md` versioning territory — so defer unless the measure-first data shows build cost dominating.
6. **Deferred miss-drain batching** (collect misses during rasterize, service between pages): reduces per-miss open/seek overhead but adds render-path coupling; note as future work only.

### 4. Risk assessment

- Pool-mismatched free — **High.** Freeing a PSRAM block with `delete[]`/`free()` (or vice versa) after refactors. Mitigation: one alloc/free pair per buffer, one convention for the whole feature (either the existing raw `heap_caps_malloc`/`heap_caps_free` pattern or `makeUniqueNoThrowPsram` everywhere, stated in the plan); pool-aware `ensureArrayCapacity`; debug-time pool check on free.
- PSRAM-absent boot on an x4pro build — **Medium-High.** `src/main.cpp:570-589` shows silent init failure with a `setup()` retry; `heap_caps_malloc(MALLOC_CAP_SPIRAM)` legitimately returns `nullptr`. Mitigation: runtime probe (`psramFound()` + one canary allocation) selects capacity *and* pool; test the failing-init path explicitly, not just the happy path.
- Unverified "Current PSRAM usage: 0 (all DRAM)" — **Medium.** Arduino-ESP32 S3 builds typically enable `CONFIG_SPIRAM_USE_MALLOC` with an internal-RAM preference threshold (~16 KB), so some large allocations may already spill to PSRAM; the doc's 165 KB budget may be partially consumed already. Mitigation: snapshot `heap_caps_get_info` per cap before/after the change instead of assuming 0.
- Eviction-horizon change vs pointer contract — **Low.** `glyphMissHandler` guarantees validity only "until the next glyphMissHandler call" (EpdFontData.h:228-231), and `GfxRenderer::getGlyphBitmap` consumers use the bitmap immediately (GfxRenderer.cpp:387, 493, 2305), so a larger ring cannot dangle live pointers. Add a comment that the ring may *not* be used to extend pointer lifetime.
- Linear scan cost at 64 entries — **Low.** `isOverflowGlyph`/`getOverflowBitmap` scans are ~64 × few-byte compares, negligible vs SD I/O. Keep the simple structure.
- PSRAM bandwidth contention with grayscale plane streaming in the same async-overlap window — **Low.** Font reads are small and sequential; the S3 PSRAM cache absorbs them. No mitigation needed beyond keeping the hot group in DRAM (already the plan).
- Host/CI regressions — **Low.** All new code sits behind `BOARD_HAS_PSRAM` (undefined on host; host tests use stubs) and the runtime probe keeps non-PSRAM envs on the current code path. Verify `-e default` still builds, not just `-e x4pro`.
- Cache invalidation — **None needed.** These are runtime (RAM) caches; no `.crosspoint` format or version change, no `Section.cpp`/`BookMetadataCache.cpp` version bump. The doc's "no invalidation complexity" claim holds.

### 5. Concrete implementation outline

0. **Instrument first** (SdCardFont.cpp): counters incremented in `onGlyphMiss` (:1593) — misses this page, unique miss codepoints this page, per-miss µs; surface via `BOOK_PROFILE` alongside the existing `preload_next` phase. Also snapshot `heap_caps_get_info(MALLOC_CAP_SPIRAM|MALLOC_CAP_INTERNAL)` before changes to replace the unverified "PSRAM usage: 0" claim.
1. **Allocation convention + pool probe** (Memory.h / SdCardFont.h / FontDecompressor.h): commit to one PSRAM alloc/free convention (recommend the existing raw `heap_caps_malloc`/`heap_caps_free` pattern for consistency with Epub.cpp:468, PixelCache.h:96, HomeActivity.cpp:171 — or fix up `makeUniqueNoThrowPsram` and adopt it; do not mix). Add a runtime probe: `BOARD_HAS_PSRAM` compile gate + `psramFound()` + canary allocation, since init can fail at boot (src/main.cpp:570-589). Derive capacities from the probe: e.g. `overflowCapacity = psram ? 64 : 8`, `advanceLimit = psram ? 4096 : 768`, `maxPageGlyphs = psram ? 2048 : 512`.
2. **Overflow ring → PSRAM** (SdCardFont.h/.cpp): `overflow_[8]` → pointer + capacity; per-entry bitmap allocations moved to PSRAM (the entries alone are ~28 B — the bitmaps are the payload); update `onGlyphMiss` (:1593-1675, keep temp-then-commit so a failed I/O leaves the slot valid), `isOverflowGlyph`/`getOverflowBitmap` (:1677-1691), `clearOverflow` (:212-216), `releaseResidentCaches` (WiFi start). Keep the DESTRUCTOR_CLOSES_FILE conventions (explicit `close()` only on the existing error paths; success path relies on the `HalFile` destructor). Add the miss stats from step 0 if not already present.
3. **Advance tables → PSRAM** (SdCardFont.cpp): `ensureArrayCapacity` (:85-91) and `mergeIntoAdvanceTable` (:1326-1356) become pool-aware (the `delete[]`-then-`new(std::nothrow)` swap must free with the matching deallocator); `clearPersistentCache` frees PSRAM blocks. Pool-selected capacity per step 1.
4. **Mini arenas + raised cap → PSRAM** (SdCardFont.cpp): pool-gate `MAX_PAGE_GLYPHS` and the miniIntervals/miniGlyphs/miniBitmap (+ `fullIntervals`, doc open question 3 — answer: yes, on PSRAM boards) allocations (:1090, :1109, :1186); on PSRAM boards `PREWARM_ARENA_TOO_LARGE` (:1191) becomes effectively unreachable — keep the DRAM path intact for non-PSRAM builds. This is the step that addresses the profiled CJK pages; without it, expect partial improvement only.
5. **`FontDecompressor` (conditional)**: move page-slot buffers *and* `PageGlyphEntry` arrays *and* the per-group `tempBuf` (:471) to PSRAM **only if** step-0 data shows flash-font decompression time mattering on the measured workload; keep `hotGroup`/`hotGlyphBuf` in DRAM (the Option B rejection stands, just with corrected reasoning — §1). Explicitly scope out the ~4.8 KB `prewarmCache` stack arrays.
6. **Verification**: `pio run -e x4pro` and `pio run -e default` (fallback path must compile on non-PSRAM builds); host tests unchanged (they stub these classes); `./bin/clang-format-fix -g`; `pio check`. Before/after `BOOK_PROFILE` on the same CJK book. **Human tester items**: heavy CJK book with >512 unique glyphs across prewarms (the residency case), a boot with PSRAM init failing (main.cpp:570-589 path), WiFi start after reading (exercises `releaseResidentCaches` on PSRAM blocks), and all 4 orientations for the render path (no changes expected).

**Bottom line**: Option A is feasible and low-risk, but as written it (a) mis-sizes the ring ~4× (shared, not per-style; `EpdGlyph` 16 B), (b) includes a no-op item (page slots 4→8), (c) rests on an unsupported 94 ms/miss figure, and (d) doesn't fix the >512-unique-glyph CJK working set that the profiled `Overflow` lines actually indicate. Re-order as: instrument (0) → residency/arenas (4) → overflow (2) → advance (3), and the projected heavy-page win is more defensible.
