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
[DBG] [SDCF] Overflow: loaded U+XXXX on demand — font cache overflow ring buffer full, reading from SD
```

**Root cause**: The `FontDecompressor` and `SdCardFont` caches are DRAM-only, severely capped for 380KB C3 budgets, and the overflow ring buffer holds only 7 glyphs. CJK books and books with SD fallback fonts hit the overflow path frequently, causing per-glyph SD reads during render (~94ms per miss per glyph).

### Goal

Move the hot-path font caches from DRAM to PSRAM on X4 Pro (ESP32-S3, 8MB PSRAM), expanding their capacities to eliminate per-page-glyph SD reads during render. Target: eliminate the 106-162ms heavy-page stalls by pre-warming more glyphs in the async overlap window and keeping them resident.

### Current font cache architecture

```cpp
// FontDecompressor (lib/EpdFont/FontDecompressor.h)
PageSlot pageSlots[MAX_PAGE_SLOTS=4];  // 4 prewarm slots, DRAM, freed between pages
uint8_t* hotGroup;      // one decompressed group, DRAM
uint8_t* hotGlyphBuf;     // single glyph scratch, DRAM

// SdCardFont (lib/EpdFont/SdCardFont.h)
OverflowEntry overflow_[8];        // 7-entry ring, DRAM — on-demand glyph cache
AdvanceEntry* advanceTable_[4];    // 768 entries × 6 bytes = 4.5KB/style, DRAM
```

**Current DRAM footprint per SD font per style**: ~4.5KB (advance table) + 7 × ~32B (overflow) + hot group (~varies) = ~5KB+
**Current PSRAM usage**: 0 (all DRAM)

### Proposed design

#### Option A: Selective PSRAM expansion (minimal change)

Move only the caches that cause SD I/O bottlenecks to PSRAM, keeping hot render-path buffers in DRAM:

1. **Expand overflow ring buffer from 7 to 64 entries** (PSRAM)
   - Each entry: `EpdGlyph` (32B) + `uint8_t* bitmap` (~200B average) = ~232B × 64 = ~15KB per style
   - With 4 styles: ~60KB PSRAM (well within 8MB budget)
   - Eliminates most `overflow: loaded U+XXXX on demand` hits

2. **Expand advance table from 768 to 4096 entries** (PSRAM)
   - 4096 × 6 bytes = 24.6KB per style
   - With 4 styles: ~100KB PSRAM
   - Eliminates `buildAdvanceTable: +%u from SD` misses for large CJK text

3. **Expand prewarm page buffer slots from 4 to 8** (PSRAM)
   - More pages pre-warmed during async overlap
   - Each slot: ~5-20KB depending on page glyph count

**Total PSRAM impact**: ~165KB worst case (60KB overflow + 100KB advance + 5KB page slots)

#### Option B: Full PSRAM migration (aggressive)

Move ALL font caches to PSRAM:
- `hotGroup` buffer (currently DRAM for speed)
- `hotGlyphBuf` scratch
- Page buffer slots
- Overflow ring
- Advance tables

**Risk**: Hot-path render (`getBitmap`) reads glyph data from PSRAM instead of DRAM — PSRAM read latency is ~80ns vs ~10ns for DRAM. On the pixel-by-pixel render path, this could add 70ns × glyphPixels per glyph. The OpenCode review's concern about "PSRAM read-modify-write is far slower per pixel" applies here.

### Recommended approach: Option A

The overflow ring buffer expansion is the highest ROI. The profiling shows `Overflow: loaded U+XXXX` correlates directly with the 106-162ms heavy pages — each miss costs ~94ms (SD seek + read + decompress). Expanding the overflow cache to 64 entries turns most misses into cache hits, saving ~80-120ms on heavy pages.

The advance table expansion is secondary: `buildAdvanceTable` already sorts reads sequentially and batches them, so its SD cost is lower per-glyph than the overflow path.

### Implementation plan

1. **Gate all PSRAM allocations behind `BOARD_HAS_PSRAM`** (not build env) — same pattern as existing PSRAM code
2. **`FontDecompressor`**: Make page slot buffers PSRAM-backed via `makeUniqueNoThrowPsram<uint8_t[]>` when `BOARD_HAS_PSRAM`
3. **`SdCardFont`**: Make overflow ring and advance tables PSRAM-backed; expand capacities
4. **Allocation helper**: Add `makeUniqueNoThrowPsramArray(size_t count)` overload that takes an explicit size parameter (fixes the `sizeof(T)=1` issue noted in Phase 3)
5. **Fallback**: If PSRAM allocation fails, fall back to DRAM with current capacity limits — graceful degradation, same as existing pattern

### Sizing the opportunity

Current heavy page cost: 138ms rasterize (with SD font misses)
- Overflow expansion: eliminates ~80-120ms of SD I/O per heavy page
- Advance table expansion: eliminates ~15-25ms of SD I/O for CJK layout
- **Expected result**: 138ms → ~20ms on heavy pages (85% reduction)

Light pages (42ms): minimal impact (already mostly cache hits)
- **Expected result**: 42ms → 35ms (15% reduction from larger prewarm window)

### What doesn't change

- No GfxRenderer modifications needed (this is pure data-cache expansion)
- No display path changes (no `clearScreen` corruption risk)
- No invalidation complexity (font caches already handle font unload)
- Host unit tests need no changes (PSRAM paths gated behind `BOARD_HAS_PSRAM`, undefined in host)
- `BOOK_PROFILE` instrumentation already exists and can measure the delta

### Risks

| Risk | Level | Mitigation |
|------|-------|------------|
| PSRAM read latency on hot render path | Medium | Only expand DRAM caches; keep hot group in DRAM |
| PSRAM pressure (165KB / 8MB) | Low | <2% of PSRAM; allocation failure falls back to DRAM |
| Host test compilation | Low | All PSRAM code gated behind `#ifdef BOARD_HAS_PSRAM` |
| Cache coherency on font unload | Low | `freeAll()` / `clearCache()` already called on style eviction |

### Open questions

1. Should the overflow ring store glyph data only, or also advance metrics? (Storing both reduces SD reads further but increases PSRAM per entry.)
2. What's the optimal overflow capacity? 64 is a guess — could profile with 16/32/64/128 to find the knee.
3. Should we move `fullIntervals` (codepoint coverage tables) to PSRAM for large CJK fonts? Currently always in DRAM.
