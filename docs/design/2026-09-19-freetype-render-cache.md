# FreeType render performance: glyph cache, hinting re-enable, fast face loading, next-page prerender

Design of record for the `feature/freetype-render-cache` campaign (2026-09-19).
Authored by the orchestrator after code-level investigation; pi implements it.

## Problem statement

Three measured/observed performance problems in the native-TTF (FtFont/FreeType)
reader path:

1. **Page rendering repeats work.** `FtFont::rasterize()` runs
   `FT_Load_Glyph` + `FT_Render_Glyph` on every call — there is NO glyph
   cache (freeink-sdk `FtFont.cpp:563/651-673/853`; the header itself states
   "FreeType owns the glyph slot … no external glyph arena"). The paint path
   (`PagePaint::walkText`, `src/adapters/PagePaint.cpp:50`) calls `rasterize`
   per glyph per page, then walks every pixel with a per-pixel `sink`
   applying `grayTone()` quantization (`PagePaint.h:39`) and
   `drawPixel`/`drawGrayDualPixel` per pixel — on 4-level displays TWICE per
   page (base BW pass `paintText` + dual-plane pass `paintPlanes`). A page
   with ~1500 glyph instances re-rasterizes and re-quantizes all of them on
   every paint, including on FIBP cache hits (FIBP caches layout, not pixels).
2. **Family loading is slow for multi-face families.**
   `BookFontLoader::ensureLoaded` (`src/BookFontLoader.cpp:239`) does, per
   face (up to 4: regular/bold/italic/bold-italic): pool alloc + FULL SD read
   (up to 2 MB per face) + sfnt validation + `FtFont::init`, and then
   `computeFingerprint()` FNV-1a hashes EVERY byte of EVERY face. Multi-face
   families multiply all of this ×4.
3. **Hinting is shipped off** (`kRenderOptions.hinting = HintingMode::None`,
   `src/BookFontLoader.h:107`) because hinted CFF enters the Adobe
   interpreter whose stack footprint overflowed small task stacks. The render
   task now runs with a 48 KB stack (e49d21bd) and the worker with 32 KB —
   the blocker needs re-measuring, not assuming.

Reference implementation: **EPub-InkPlate** (turgu1), cloned at
`/tmp/epub-inkplate`. Its FreeType integration does exactly what we lack:

- `components/fonts/src/font.cpp` — `Font::getOrCreateGlyph()`: persistent
  per-(charcode, size) glyph cache; the rendered bitmap is COPIED out of the
  FT slot into pooled storage once, then served forever.
- `components/fonts/src/ttf2.cpp` — `FT_LOAD_DEFAULT` (native hinting ON)
  and a single `FT_Render_Glyph` (MONO or NORMAL) per glyph-ever.
- `components/fonts/src/fonts.cpp` — face-level cache with explicit
  `clearCache()`/`clearGlyphCaches()` lifecycle.

## Prior art in this repo

Commit `60079923` ("Prerender the next page", upstream branch
`fix-image-display`, never merged to develop) prerendered the next page's
content INTO THE SINGLE FRAMEBUFFER after the current page's flush — e-ink
retains the displayed image without the buffer, so the framebuffer itself is
a free 48 KB prerender target. On a forward page turn it only drew the status
bar over the prerendered content and flushed. 156-line diff,
`renderPageContentOnly()` helper, `PreRenderedPage{ready, spineIndex,
pageIndex}` validity struct. This design ports that pattern to the
TTF/PagePaint reader.

## Design

### P1 — Glyph bitmap cache in FtFont (SDK PR, highest impact)

Add a bounded glyph cache to `freeink::font::FtFont`:

- Key: `(glyphId, sizePx)` per face instance. Render options are fixed per
  face at `setRenderOptions()` — cache MUST be flushed in `setRenderOptions()`
  and `ensureSize()` when the size changes the raster.
- Value: the FT-rendered 8-bit coverage bitmap (copied out of the slot) +
  `GlyphMetrics`/bearing fields. Allocation through the existing FontAlloc
  custom-FT-memory path (PSRAM-first) or a small pooled allocator with a hard
  byte budget (default ~512 KB, overridable; hard ceiling + size guard per
  the CWE-400 house rule — no unbounded growth).
- Eviction: simple LRU (or generation-flush) — glyph bitmaps are
  deterministic (same FT version + same face + same options ⇒ same bytes),
  so eviction only costs a re-render, never correctness.
  **As shipped ([task2], SDK PR Belphemur/freeink-sdk#31):** LRU with the
  size IN the key; one deliberate deviation: NO flush on size change in
  `ensureSize26_6()` — the key already separates sizes, and the RasterFont
  contract requires the last `rasterize()` bitmap to stay valid across a
  size-changing `glyphBounds()`/`advance()` (the existing `test_ftfont`
  case pins this; a flush would free live coverage). Flush points:
  `setRenderOptions()`, `deinit()`/re-`init()` (glyph IDs are face-local),
  budget shrink.
- `rasterize()` contract change (documented, compatible): the returned
  bitmap stays valid until eviction or the next `rasterize()` on this face —
  strictly longer-lived than today's "until next rasterize()". All current
  consumers (PagePaint walks) consume within a single walk; the synthetic
  bold double-strike reads the same glyph twice, fine.
- Determinism gate (host test): rendering N glyphs with the cache enabled
  must be BYTE-IDENTICAL to the uncached path for the same face/options —
  same test style as the existing `FtFontRenderOptionsTest` /
  `FtFontDefaultParityHash` host tests. Plus: cache-hit after eviction
  re-render equality, budget-cap enforcement, flush on options change.
- The stb rollback path (`TtfFont`) is untouched and keeps its arena cache.
- Threading: one FtFont per task (reader paints on the render task; the
  prefetch worker builds its OWN faces) — no cross-task sharing, no new
  locks. State this invariant in the header comment.

### P2 — Re-enable hinting + single-work AA (app-side)

- Extend `kRenderOptions` usage: turn hinting ON (start with
  `HintingMode::Light`; native only if built with
  `FREEINK_FONT_ENABLE_NATIVE_HINTING`) now that P1 makes its cost
  one-time-per-glyph instead of per-page.
- The "double work" the owner calls out — 8-bit AA coverage rendered and
  then re-quantized per pixel into 2-bit tone planes on every pass — is
  addressed at two levels:
  1. P1 alone removes the repeated FT render (the dominant cost).
  2. Optional **Crisp mode**: `HintingMode::Light` + MONO rasterization →
     1-bit glyphs straight from FT, no coverage, no `grayTone`, no dual-plane
     pass. Shipped as a user setting (Smooth = AA planes, default;
     Crisp = hinted mono, fastest and hardest-edged). Both paths are render
     options and MUST fold into the FIBP cache identity — extend
     `renderOptionsFingerprintTag()` (BookFontLoader.h:115) to cover hinting
     mode AND raster mode, per its own documented rule.
- Stack-safety gate (the original blocker): before shipping hinted CFF,
  measure `uxTaskGetStackHighWaterMark` on the 48 KB render task and the
  32 KB worker task with hinted rendering of a CFF-heavy font. If HWM drops
  below a safe floor, auto-degrade THAT face to unhinted (per-face, logged
  LOG_ERR) rather than risking overflow. Review contract (PR #155 round 1):
  the degrade MUST go through `setRenderOptions()` — the P1 cache flush
  point — so no stale hinted/cached bitmap survives the mode change, and the
  effective (post-degrade) mode participates in the fingerprint identity so
  FIBP caches regenerate. Hinted TrueType (native TT
  interpreter) has a much smaller footprint — Safe default: Light for TT
  faces, measure for CFF.

### P3 — Faster face loading (app-side)

1. **SD-cached fingerprint** (`src/BookFontLoader.cpp`): persist the per-file
   FNV hash under `/.crosspoint/fonts/` keyed by (path hash), storing
   `{fileSize, mtime, hash}`; recompute only when size or mtime differ.
   Content-based semantics preserved (mtime is only the rehash trigger, never
   the hash value). `computeFingerprint()` stops hashing megabytes on every
   open. The FibpPrefetchWorker's parity hash can later reuse the same cache
   (post-#153 follow-up — this branch must NOT touch FibpPrefetchWorker,
   which belongs to the open PR #153).

   **As shipped ([task3] + review round 1, amended per the as-built rule):**
   the record is
   `{magic 'BFP2', version, inSeed, hash, headHash, fileSize, mtime}` —
   28 bytes, one
   file per face (`fp_<pathhash>.bin`). `headHash` is the FNV-1a over the
   first 4 KB of the (resident) face bytes and is verified on every hit
   (~2% of a full walk): a replaced font changes its header with
   near-certainty, closing the same-size/same-mtime staleness hole from
   review. Residual accepted window: a rewrite keeping size, mtime AND the
   entire first 4 KB identical. Swapped-card exposure is bounded the same
   way (a foreign card carries its own consistent records; a foreign font
   matching size+mtime+4 KB head is effectively the same content). Because the fingerprint CHAINS
   FNV-1a per slot (the incoming seed of face i+1 depends on face i), each
   record also stores the `inSeed` it was computed under and is only served
   when {fileSize, mtime, inSeed} all match the current load — a slot can
   never poison the chain after a predecessor's file changed. `scanFonts`
   now populates `FontFaceInfo::mtime` from `HalFile::modificationTime()`
   (it was declared but never set); a face with no SD timestamp (mtime 0)
   bypasses the cache entirely (fail closed: size alone cannot detect a
   same-size rewrite). Corrupt records (wrong length/magic/version) are
   recomputed and rewritten from a clean slate. Parity with the pure
   in-memory walk is host-tested (`BookFontLoaderFingerprintCache.*`),
   including the documented pre-fallback-tail contract of
   `fontFingerprint()`. The post-#153 note above stands: PR #153 is now
   MERGED to develop, the worker lives in `develop`, and it still belongs
   to a FOLLOW-UP campaign — this branch does not touch it.
2. **Keep the resident-bytes model on PSRAM boards** (8 MB pool affords it);
   do NOT switch to `initStream` in this campaign — measure first.
3. **Deferred non-regular style loading** — optional, OFF by default, gated
   on device measurement: if the fp-cache + telemetry show SD read dominates,
   load regular sync and bold/italic/bold-italic on the idle path within the
   first seconds. Fingerprint/styleCoverage must be computed from the
   manifest UP FRONT so the FIBP generation never changes mid-book; if this
   mode is ever enabled, installing a deferred face MUST recompute the
   effective fingerprint/style coverage and invalidate affected FIBP/
   prerender state (review contract, PR #155 round 1). Renders
   before a style lands use the existing synthetic-bold double-strike
   fallback.

### P4 — One-page-ahead prerender (app-side, port of 60079923)

- After rendering page N (post-flush), if inputs are idle and no build is in
  flight, render page N+1's CONTENT (no status bar, no flush) into the
  framebuffer. On a forward page turn: draw the status bar over the
  prerendered content + flush — skipping layout+paint entirely for the turn.
- Validity struct: `{ready, spineIndex, pageIndex, layoutGeneration,
  orientation}`; invalidate on any non-forward navigation, settings change,
  layout generation change, font fingerprint change.
- Review contract (PR #155 round 1): the prerender is ONE RenderLock-
  protected transaction that STARTS only after `waitRefreshComplete()` (the
  panel must have finished displaying page N; painting during an active
  refresh would expose a partial frame), publishes `ready` only after the
  paint completes, and is consumed under the same lock. The prerender is
  ALSO invalidated (or the current page re-rendered) on ANY full-framebuffer
  flush outside the forward turn's own commit: the HALF-refresh cadence
  (`ReaderUtils`), `forcedRefreshPending`, in-book popups (indexing, font
  sheet), and power-cycle/wake events — otherwise the panel shows page N+1
  bare (no status bar) while the reading position is still page N.
- Do NOT prerender across a chapter boundary whose FIBP cache does not exist
  (do not fight the 10%-remaining prefetch trigger from PR #153 for the same
  work).
- C3-safe: no extra RAM — the framebuffer IS the target. Cost is one extra
  paint per turn, which P1 makes cheap, and it overlaps the 1–2 s e-ink
  refresh (free wall-clock).
- Reference diff: `git show 60079923` (adapt to PagePaint/ttf_ position
  tracking — the upstream diff predates the TTF reader).

## Paradigm check (coding-philosophy, DRY > SOLID > KISS)

- DRY: the glyph cache stores FT's own output — no second copy of rendering
  knowledge; `grayTone()` remains the single quantizer (any future
  plane-caching must call it, not re-derive). The fp-cache stores the
  fingerprint computation, not a second fingerprint definition — loader and
  worker both read ONE cache.
- SOLID: budget caps + size guards on the cache (fail closed, log, degrade);
  per-face auto-degrade on stack pressure instead of a global hinting toggle
  that risks overflow; prerender validity is a structurally checked snapshot,
  not a scattered bool.
- KISS: no streamed faces this round; no plane-blit rewrite of PagePaint
  until P1 telemetry proves it necessary (that is a measured Phase-3
  candidate, not shipped speculation).

## Measurement plan (device soak, owner)

Debug-build telemetry (REND-style) for: page paint ms (cache miss vs hit),
per-face load ms (read / hash / FT-init breakdown), stack HWM with hinting.
Device-side numbers decide: P3.3 deferral, the Phase-3 plane-blit, and the
Crisp-mode default.
