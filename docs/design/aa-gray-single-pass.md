# Design: Single-Pass Dual-Plane Grayscale Rendering (Halve the Gray-Pass CPU)

**Status:** Design / architecture plan — pending review
**Date:** 2026-09-05
**Branch:** `design/aa-gray-pass`
**Target:** X4 Pro (UC8279X4, ESP32-C3, ~380 KB DRAM, no PSRAM). Must not regress Paper Mono /
Sticky / X3 / X4-UC8179.
**Scope:** DESIGN DOC ONLY — no source edits, no build, no commit.

> **Convention note.** Line numbers are **working-tree** snapshots of the in-progress branch
> (`git status` shows `lib/GfxRenderer/GfxRenderer.cpp` modified: 2,563 lines vs 2,551 in HEAD; the +12
> lines are the in-progress `grayplanes::` refactor — `planBlock`/`:417`, `setMsb`/`:529`, `setLsb`/`:534`).
> `EpubReaderActivity.cpp` is unmodified, so its numbers match HEAD. Submodule driver lines (UC8279X4… /
> Uc8253… / PaperMono…) are pinned to the checked-out `freeink-sdk` commit and are stable. The
> AGENTS.md/`:439-440` and brief ":447" citations were HEAD-era; they are stale here. Every citation is
> grep-verified as of this writing; re-derive any with `grep -n '<anchor>' <file>`. Also: `pixels2b` is a
> *local variable* (`fontconvert.py:364`), not a function — there is no `pixels2b` to grep for (see §2).

---

## 1. Goal and summary

Halve the gray-pass CPU by rendering **both** absolute grayscale bitplanes (LSB + MSB) in a single
page walk, instead of walking the whole page twice (once per plane).

The current reader render is `EpubReaderActivity.cpp:1658` `renderContents()`:

1. BW render + display (base layer).
2. If `SETTINGS.textAntiAliasing` or the page has images: the page is rendered **again** into an MSB
   bitplane, then **again** into an LSB bitplane — two full page walks — then `displayGrayBuffer()`
   commits both (UC8279X4Driver.cpp:532 `copyGrayscaleLsb` / :555 `copyGrayscaleMsb`).

Every plane render re-decodes every glyph bitmap, re-runs every `drawText`/`drawBitmap` call, and
re-reads the image `.pxc` cache (ImageBlock.cpp:94: "~100 ms for a full-page image … for every band
of both gray planes"). `LOG_DBG("ERS", …)` consistently shows the gray walk(s) as the largest
non-display segment after the panel refresh itself.

A single pass that emits both planes preserves the **bit-exact** per-plane buffers the driver already
expects (`writeGrayscalePlaneStrip` / `copyGrayscaleLsb/MsbBuffers`), so **no driver or cache schema
change is required** (§5). This is a pure `GfxRenderer` + reader-activity change.

---

## 2. How gray currently flows (verified, working tree)

**Gray level lives in the glyph.** Reader fonts are baked 2-bit (4-level) by the font converter:
`0=white, 1=light, 2=dark, 3=black` (fontconvert.py:362-363 legend; the inline 2-bit downsampling is the
block at fontconvert.py:361-385; the variable holding it is `pixels2b` at :364 — there is **no** function
named `pixels2b`, so the brief's "search `pixels2b`" resolves to that variable).

**Tone → plane flags, per renderMode, via `grayplanes::`.** The working tree routes the 2-bit tone
through a small helper rather than a hand-rolled `bmpVal` swap. Two renderers, two call sites:

- `renderCharImpl` (`GfxRenderer.cpp:457`; template definition) unpacks the 2-bit glyph and dispatches on
  `renderMode` (:524-:537):
  ```cpp
  if (renderMode == GfxRenderer::BW) {                                        // :524 black/dark/light -> B/W ink
    renderer.drawPixel(screenX, screenY, pixelState);                         // :527
  } else if (renderMode == GfxRenderer::GRAYSCALE_MSB && grayplanes::setMsb(rawTone)) { // :529
    renderer.drawPixel(screenX, screenY, false);                              // :533  MSB plane
  } else if (renderMode == GfxRenderer::GRAYSCALE_LSB && grayplanes::setLsb(rawTone)) { // :534
    renderer.drawPixel(screenX, screenY, false);                              // :536  LSB plane
  }                                                                           // :537 end 2-bit
  // `else {` at :540 begins the 1-bit branch (:540-:558, BW drawPixel :558).
  ```
- `renderCharScaled` (sup/sub/ruby 50%; `GfxRenderer.cpp:380`) asks `grayplanes` for the whole plan at
  once: `grayplanes::planBlock(renderMode == GfxRenderer::BW, maxRaw, coverage)` :417 returns a
  `BlockPlan{ .plot, .msb, .lsb }`; the MSB/LSB arms are :423/:425 (`plan.msb` / `plan.lsb`); the
  `else` at :431 begins its 1-bit branch (`// 1-bit packed format` :432).

The decomposition the dual pass must reproduce is already stated for the maintainer at :420:
**"dark-flagged blocks set both planes, light-only blocks MSB."** Equivalently `setMsb`/`setLsb` fire
both for a dark tone and only `setMsb` for a light tone. That is the whole truth table: dark→both,
light→MSB-only, black/white→neither (absorbed by the B/W base).

The driver folds the two 1-bit masks back into 4 tones (UC8279X4Driver.cpp:527-531: plane0/LSB→DTM1
`0x10`, plane1/MSB→DTM2 `0x13`; `copyGrayscaleMsb` :555-... does `plane1 = plane0 ^ maskMsb`).

**Other consumers of `renderMode` — the divergence set:**

- `renderCharScaled` 1-bit: `GfxRenderer.cpp:431` (`// 1-bit packed format` :432) → BW ink only.
- `renderCharImpl` 1-bit: `:540` (`else` after the 2-bit block), BW drawPixel `:558`.
- Images: `drawBitmap` 2-bit :1382, plane switch BW:1475 / MSB:1478 / LSB:1480; and the raw fast path
  `DirectPixelWriter::writePixel` (`DirectPixelWriter.h:147`; mode switch :151-166: BW:152-155,
  MSB:156-159, LSB:160-163). `DirectCacheWriter::writePixel` (:223) writes a *2-bit cache* buffer — a
  different staging store, not a gray plane target (no change).
- `drawPixel` itself: `GfxRenderer.cpp:568` (rotate + bounds :575-579; **strip redirect + clip**
  :585-591 selecting `_stripBuf` vs `frameBuffer`; bit calc :593-595; **one** bit write :597-601).
- `preserveImagePolarity`: `GfxRenderer.cpp:1570` (RMW on the active target) — see §7 risk #2.
- Strip target: `beginStripTarget`/`endStripTarget` :1677/:1688; band cull `glyphIntersectsStrip` :1695.

---

## 3. Target addressing — options, RAM cost, CPU cost, churn

The gray render today targets **one** 1-bit scratch at a time. A dual pass needs **two** 1-bit planes
per emitted pixel. Three options:

### (a) Packed 2-bit/pixel dual strip buffer
One scratch holding **2 bits/pixel**. `drawPixel` becomes a read-modify-write on a 2-bit field;
`writeGrayscalePlaneStrip` must unpack 2-bit→1-bit per plane.
- **RAM:** non-overlap tiled path `gwBytes*STRIP_ROWS` doubles: `100*80 = 8,000 B` → **16,000 B** per
  strip. Overlap path: 2 bits/pixel over the full frame = `800*480*2/8 = 96,000 B` = ~93.8 KB, **same**
  as two separate 48 KB planes. No RAM change in the overlap path, +8 KB in the non-overlap path.
- **CPU/pixel:** one byte read + mask + OR + write (RMW) per gray pixel — *more* work per pixel than a
  bare bit-set, but only **one** decode/walk.
- **Churn:** high — `writeGrayscalePlaneStrip` (`FreeInkDisplay.cpp:842`) and `copyGrayscaleLsb/MsbBuffers`
  (:830/:836) take 1-bit plane buffers; would need a new packed-unpack or a new driver entry. Crosses
  the HAL boundary.

### (b) `RenderMode::GRAYSCALE_DUAL` + second plane pointer on GfxRenderer   ◀ decision
Add a second target pointer alongside the existing strip/framebuffer target. In DUAL mode the existing
MSB arm writes the MSB target and the existing LSB arm writes the LSB target; because both arms now fire
in one walk (a single pixel iteration whose two `else if` tests each match), a **single** page walk
emits **both** plane buffers. The two resulting 1-bit buffers are handed to the **unchanged**
`writeGrayscalePlaneStrip(lsb, …)` / `writeGrayscalePlaneStrip(msb, …)` and `copyGrayscaleLsb/MsbBuffers`
calls.
- **RAM:** identical to today — non-overlap path needs two 8 KB strips = **16,000 B**; overlap path
  keeps two 48,000 B plane buffers (96 KB). No new full-framebuffer allocation.
- **CPU/pixel:** one decode/walk (halved), plus **one extra bit-write** per gray pixel vs. a single
  plane pass. The extra write is a couple of ALU ops, dwarfed by the saved re-decode of every glyph
  bitmap and the saved second text/image walk.
- **Churn:** contained to `GfxRenderer` + reader. **No driver / HAL / cache change.** Drivers already
  consume LSB and MSB as independent 1-bit planes.

### (c) Single target, dual-mode sets bits in two buffers selected per-plane-bit
Basically (b) as a mode flag instead of an enum + pointer pair. Same RAM/CPU profile as (b); marginally
less self-documenting. Equivalent churn.

### Decision
**Option (b).** It is the only variant that keeps the `writeGrayscalePlaneStrip`/`copyGrayscale*`
contract byte-for-byte, so per-board driver variance (§5) cannot regress from this change, and it needs
no cache version bump. The per-pixel cost addendum (a second bit-write) is strictly cheaper than the work
it eliminates (full glyph decode + layout walk + image cache re-read). Option (a)'s packed un-packing
would push plane-splitting logic into the driver, multiplying per-board divergence risk.

### RAM budget check (X4 / ESP32-C3, ~380 KB DRAM, no PSRAM)
- `STRIP_ROWS = 80`, `gwBytes = 800/8 = 100`. Today's single strip scratch = `100*80 = 8,000 B` (the
  brief's "6 KB" is the `48,000/8 = 6,144` rough figure; the real figure is 8,000 — verified at
  EpubReaderActivity.cpp:1782 `makeUniqueNoThrow<uint8_t[]>(gwBytes * STRIP_ROWS)`).

> ⚠ **Correction to the brief's nontiled math.** The overlap path uses `planeBytes = gwBytes*gh` with
> `gh = 480` (full panel height — each strip scratch is a full column-strip band spanning the page). So
> `planeBytes = 100*480 = 48,000 B`, **not** `gwBytes*STRIP_ROWS`. Each plane buffer in the overlap path
> is 48 KB → two = 96 KB (matches the two 48,000 B allocations at :1749/:1750). Dual mode **reuses**
> these same 96 KB (one buffer per plane, filled in a single walk). The non-overlap dual scratch is the
> only RAM delta: 8 KB → 16 KB (+8 KB), ~4% of the 380 KB ceiling, under `MAX_ALLOC` fragmentation headroom.
- The overlap allocations at :1749/:1750 are gated by `ESP.getFreeHeap() >= planeBytes + 60,000` and
  `ESP.getMaxAllocHeap() >= planeBytes + 16,384` (:1745-:1748 `planeBufFits`); dual mode reuses them.

---

## 4. Divergence points — checklist

Every code path that consumes `renderMode` and would need a `GRAYSCALE_DUAL` arm. Lines are working-tree,
grep-verified. "1-bit" rows are gray-inert (they paint B/W ink only) and need **no** change. In DUAL mode
the MSB arm → MSB target and the LSB arm → LSB target; both fire in one walk.

| # | Code path | File:line (working tree) | Dual-mode action |
|---|-----------|--------------------------|------------------|
| 1 | `renderCharImpl` 2-bit | GfxRenderer.cpp:457 (arms :524/:529/:534) | MSB→MSB tgt, LSB→LSB tgt (DUAL, 1 walk) |
| 2 | `renderCharImpl` 1-bit | :540-:558 (:558) | none — 1-bit is BW ink |
| 3 | `renderCharScaled` 2-bit | GfxRenderer.cpp:380 (planBlock:417; arms :423/:425) | mirror #1 via `plan.msb`/`.lsb` |
| 4 | `renderCharScaled` 1-bit | :431-:452 (:432) | none |
| 5 | `drawBitmap` 2-bit | GfxRenderer.cpp:1382 (arms :1475/:1478/:1480) | mirror #1 |
| 6 | `drawBitmap1Bit` | :1496 (:1554 ink `val<3`) | none |
| 7 | `DirectPixelWriter::writePixel` | DirectPixelWriter.h:147 (arms :152/:156/:160) | DUAL: write MSB+LSB targets |
| 8 | `DirectCacheWriter::writePixel` | DirectPixelWriter.h:223 | none — cache buf, not a plane tgt |
| 9 | `drawPixel` (core) | GfxRenderer.cpp:568 (strip :585-591; write :597-601) | DUAL: write both plane targets |
| 10 | `beginStripTarget`/`endStripTarget` | GfxRenderer.cpp:1677/:1688 | accept 2nd plane pointer |
| 11 | `glyphIntersectsStrip` | GfxRenderer.cpp:1695 | none — culls per glyph, orient-aware |
| 12 | `preserveImagePolarity` | GfxRenderer.cpp:1570 | RISK: RMW must hit both plane targets |
| 13 | `writeGrayscalePlaneStrip` | GfxRenderer.cpp:2436 | none — unchanged per-plane API |
| 14 | `copyGrayscaleLsb/MsbBuffers` | GfxRenderer.cpp:2430/:2432 | none — unchanged |
| 15 | `displayGrayBuffer` | GfxRenderer.cpp:2434 | none |
| 16 | `storeBwBuffer`/`restoreBwBuffer` | GfxRenderer.cpp:2461/:2494 (mk :2473) | none — nontiled only (no strips) |

**Out-of-scope paths (verified).** `drawTextDither`/`drawCharDither` (GfxRenderer.cpp:2239/2277) route
gray via a Bayer dither in **BW** mode, not the AA planes — they are used by disabled UI rows, not by
`page->render()` (which goes through `drawText`→`renderCharImpl`). No dual-mode branch needed. A grep
for `renderMode` switch arms returns only `renderCharImpl`, `renderCharScaled`, `drawBitmap`, and
`DirectPixelWriter::writePixel` (`DirectPixelWriter.h:19,42,151-166`) — no others.

---

## 5. Interaction with async overlap

The overlap path is `EpubReaderActivity.cpp:1749-1773` (`overlapRefresh = tiledGrayscale &&
supportsAsyncRefresh() && !pageHasImages`, defined :1691). It allocates `lsbPlaneBuf` and
`msbPlaneBuf` independently (:1749-1750), renders LSB while the B/W refresh is in flight, then renders
MSB.

**Single-pass dual simplifies, not breaks, the async path.** The gray render becomes **one** walk
emitting both plane buffers during the same overlap window (the framebuffer is still untouched — the
plane buffers are separate allocations, as today). The `lsbPlaneBuf`/`msbPlaneBuf` allocation stays:
one buffer per plane, filled in a single walk instead of two.

A real simplification: today's `else`-branch fallback at :1764-1765 re-renders the **MSB into the LSB
buffer** *after* `waitRefreshComplete()` (the second `makeUniqueNoThrow` failed) — that second walk
loses its overlap. Dual mode does one walk during overlap regardless of which allocation survived, so
even the OOM-fallback case keeps one overlapping walk. The post-walk
`writeGrayscalePlaneStrip(true,…)` (:1760) + `writeGrayscalePlaneStrip(false,…)` (:1762, and :1765 in the
reuse fallback) sequence is unchanged — it already takes one plane buffer each; `displayGrayBuffer()`
:1770 and `cleanupGrayscaleWithFrameBuffer()` :1773 are unchanged.

**Net:** the async contract (framebuffer preserved across the deferred refresh; `waitRefreshComplete`
before reuse) is unchanged. Dual-mode just collapses `renderPlaneToBuffer(true,…)` +
`renderPlaneToBuffer(false,…)` (:1732, :1753-1754, and the non-overlap per-strip loop :1803/:1814)
into one walk. `supportsAsyncDisplay` is `true` on UC8279X4 (`Uc8279Driver.h:45`),
UC8253X3 (`Uc8253X3Driver.h:62`) and Ssd1677/Sticky (`Ssd1677Driver.h:81`);
Paper Mono returns `false` (`PaperMonoDriver.h:53`), so the overlap branch is never taken on Paper
Mono — it uses the non-overlap tiled path, which dual mode still improves (one walk instead of two).

---

## 6. Expected savings (measured by existing LOG_DBG)

The page render time is already instrumented. The savings map directly onto existing fields:

- **Tiled async path:** `LOG_DBG("ERS", "Page render (tiled async)…")` at
  `EpubReaderActivity.cpp:1776`; field `gray_render` (:1779) currently lumps *both* plane walks. With
  dual mode it becomes a single `gray_both` — expect ~½.
- **Tiled (non-overlap) path:** `LOG_DBG("ERS", "Page render (tiled)…")` at :1826, fields
  `gray_lsb` + `gray_msb` (:1829). After dual mode these collapse to one `gray_both`; `gray_lsb +
  gray_msb` ~halves.
- **Non-tiled (`storeBwBuffer`) path:** `LOG_DBG("ERS", "Page render…")` at :1860, fields
  `gray_lsb` + `gray_msb` (:1862). *Not* in scope for phase 1 (no strip target; dual would need a second
  full framebuffer — see §7), so its LOG_DBG stays as-is — the regression baseline for X4-UC8179.
- `gray_display` / `gray_write` / `total` (all three LOG_DBG sites) should be **unchanged** in write
  volume (same bytes pushed to the panel per plane); only the render-side `gray_*` walk time shrinks.

**Image bonus:** pages with images also re-read the `.pxc` pixel cache once per plane per strip band
(ImageBlock.cpp:94-104: "~100 ms … for every band of both gray planes"). One walk instead of two halves
those re-reads too.

**A/B method:** log `ESP.getFreeHeap()` before/after the gray block (current `ERS` sites don't), and
diff the `gray_*` fields. The dual scratch (16 KB) must return to the baseline heap after scope exit —
assert no net leak.

---

## 7. Risks

1. **Hot-path `drawPixel` cost (`GfxRenderer.cpp:568`).** `drawPixel` is per-pixel, inlined (comment :572
   says so). Adding a second target write in DUAL mode adds an ALU op + store per gray pixel. *Mitigation:*
   the BW path stays branch-free — the strip-redirect block :585-591 is the only added gate and it is
   shared by both targets; the extra store is far cheaper than the eliminated re-decode. Still, the one
   place per-pixel overhead grows — quantify via the `gray_both` vs `gray_lsb+gray_msb` LOG_DBG delta.

2. **`preserveImagePolarity` (`GfxRenderer.cpp:1570`).** It does a read-modify-write on the active
   target (`getWriteTarget()`, `GfxRenderer.h:248` — returns `_stripBuf` under a strip) around an image
   rect. In dual mode there are *two* targets. If it only touches one, dark-mode polarity of light-gray
   image pixels could be wrong. **Must** apply the RMW to both plane buffers. Flagged as the
   highest-correctness risk; needs an explicit dual-mode test on an image page.

3. **Strip culling correctness (`glyphIntersectsStrip` :1695).** Culling is per-glyph and
   orientation-aware; in dual mode it culls **once** per band per glyph (instead of once per plane),
   so it is strictly better, not worse. No change needed — but a debug assert that a dual-stripped page
   matches a two-pass page bit-for-bit is cheap insurance.

4. **Image polarity / `DirectPixelWriter` band clipping (`DirectPixelWriter.h:111-142`).**
   `bandColRange` narrows columns to the in-band window assuming a single write target. Dual mode adds a
   second target pointer; `writePixel` (:147, switch :151-166) must clip the *same* physical row to both.
   Low risk (same coords), but the second buffer write must respect the identical band test.

5. **Per-board regression matrix** (verified from `PanelDriver.h` defaults + per-driver overrides):

   | Board | Driver | strip? | async? | combine base? | busy staging? | Dual-mode fit |
   |-------|--------|--------|--------|---------------|---------------|---------------|
   | X4 / X4C / X4 Pro | Uc8279X4Driver ^1 | true | true | false | false | yes (phase 1) |
   | X3 (UC8253) | Uc8253X3Driver ^2 | true | true | false | false | yes (phase 1) |
   | X3 (UC8279d) | Uc8279Driver | true | true | false | false | yes (phase 1) |
   | X4-UC8179 | Uc8179Driver ^3 | **false** | true | false | false | **no** — stays nontiled |
   | Sticky | Ssd1677Driver ^4 | true | true | false | false | yes (phase 1) |
   | Paper Mono | PaperMonoDriver ^5 | true | **false** | **true** | **true** | yes (phase 1) |

   ^1 `Uc8279Driver.h:54` strip, `:45` async. `Uc8279X4Driver.cpp` absolute planes
   (`copyGrayscaleLsb`:532, `copyGrayscaleMsb`:555).
   ^2 `Uc8253X3Driver.h:64` strip, `:62` async; differential grayscale
   (`copyGrayscaleMsb`:377 requires `lsbValid`; per-strip PTL stream :382-412).
   ^3 `Uc8179Driver.h:82` strip=false → uses the `storeBwBuffer` nontiled path
   (`EpubReaderActivity.cpp:1835`). Dual does **not** engage here; no regression.
   ^4 `Ssd1677Driver.h:87` strip, `:81` async (Sticky = Seeed Sticky, SSD1677 panel).
   ^5 `PaperMonoDriver.h:58` strip, `:59` combine, `:53` async=false, `:65` staging;
   `writeGrayscalePlaneStrip` uses `markGrayRows` coverage (PaperMonoDriver.cpp:939-962). Dual keeps
   writing 1-bit planes, so coverage tracking is unaffected — but verify the two single-plane writes
   still satisfy the coverage gate.

6. **Cache compatibility.** `section.bin` is `SECTION_FILE_VERSION = 45` (`Section.cpp:50`) and stores
   **layout** (line breaks, glyph positions), not rendered pixels; rendering is recomputed at draw time
   inside `page->render()` (`EpubReaderActivity.cpp:1670/1694`). A single walk emitting two planes
   instead of two walks emitting one changes **no cached geometry and no byte layout**, so **no cache
   version bump is required**. A grep for `plane`/`RenderMode` in `Section.cpp`/`Section.h` found no
   pass-count or renderMode field in the layout cache. *Caveat / UNVERIFIED beyond grep:* confirm no
   cache key/hash incorporates a render pass count before shipping phase 1.

7. **OOM in the overlap path.** The two `makeUniqueNoThrow` calls at :1749/:1750 already degrade
   gracefully (one buffer → re-render the missing plane into the survivor, :1764-1765). Dual mode must
   preserve this: engage only when *both* plane buffers are live, else transparently degrade to the
   current two-walk behavior. Keeps the 380 KB OOM story identical.

---

## 8. Verdict + phased plan

**Verdict: YES, worth it — scoped to the tiled path.** The gray walk is the largest non-display CPU
segment in the `ERS` LOG_DBG, and the change is a pure `GfxRenderer` + reader-activity refactor that
keeps every driver and cache contract intact. The per-pixel cost addendum (one extra bit-write) is
cheaper than the per-glyph re-decode it removes, and RAM grows by only 8 KB (16 KB scratch) in the
non-overlap path with zero growth in the overlap path. The only boards *unaffected* are X4-UC8179
(strips unsupported → nontiled path, unchanged) and Paper Mono's BW-dither UI (not reader gray).

### Phase 1 — minimal dual-mode, tiled path only
1. `lib/GfxRenderer/GfxRenderer.h:31` — add `RenderMode::GRAYSCALE_DUAL`; add a second (mutable) plane
   target pointer; extend `beginStripTarget` (:233) to take an optional second pointer (`nullptr` →
   legacy single target).
2. `lib/GfxRenderer/GfxRenderer.cpp`:
   - `drawPixel` :568 — in DUAL+strip mode, write the plane bit to **both** targets (BW/strip-clip
     :585-591 unchanged).
   - `renderCharImpl` :524-:537 — add a `GRAYSCALE_DUAL` arm that writes MSB→MSB-tgt and LSB→LSB-tgt
     (reusing `grayplanes::setMsb`/`:529`/`setLsb`/`:534` so the dark→both/light→MSB truth table is
     identical to today, just doubled).
   - `renderCharScaled` :417-:425 — same mirror via `plan.msb`/`:423`, `plan.lsb`/`:425`.
   - `drawBitmap` 2-bit `:1475-:1480` — same mirror.
   - `DirectPixelWriter::writePixel` (`DirectPixelWriter.h:147`) — DUAL arm writing both targets.
   - `beginStripTarget`/`endStripTarget` :1677/:1688 — accept the second pointer.
3. `src/activities/reader/EpubReaderActivity.cpp` — in `renderPlaneToBuffer` (:1732) replace the
   two-plane loop with one DUAL walk per band; keep the two `writeGrayscalePlaneStrip` calls
   (:1760/:1762, + reuse fallback :1765; non-overlap :1803/:1814) per plane, keep `displayGrayBuffer`
   (:1770/:1819/:1853) and `cleanupGrayscaleWithFrameBuffer` (:1773/:1822), keep the `planeBufFits`
   guard (:1745-:1748). Rename the `ERS` LOG_DBG field `gray_lsb`/`gray_msb`→`gray_both` *only on the
   dual path*; leave the nontiled LOG_DBG untouched as the X4-UC8179 baseline.
4. Gate: DUAL engages **only** when `tiledGrayscale` (:1685) — never on X4-UC8179 (`supportsStripGrayscale`
   false → :1685 is false).

### Phase 2 — extensions (separate review)
- Nontiled `storeBwBuffer` path (:1835-1856): would need a **second** 48 KB full framebuffer to hold
  the MSB plane during the LSB walk — violates "no new full-framebuffer allocation" on the 380 KB C3.
  Options: (i) attempt dual nontiled only when `ESP.getFreeHeap()` can spare 48 KB (rare mid-render),
  else degrade to 2-pass; (ii) defer. Likely defer — the tiled path covers X4/X3/Sticky/Paper Mono, the
  only boards that reach AA in practice.
- `preserveImagePolarity` (:1570) dual-target RMW — correctness gate for image pages (risk #2).
- Paper Mono coverage audit (PaperMonoDriver.cpp:939-962) — confirm two 1-bit plane writes still
  satisfy `markGrayRows`.
- Image pages (`pageHasImages`): gray passes skip AA text and only re-render *images*
  (renderGrayscalePass :1692-:1698 calls `renderImages` :1696 when `!needsTextGrayscale`). Dual mode for
  images exercises the `drawBitmap`/`DirectPixelWriter` branches (table #5/#7) — the highest-test
  surface; lean on the `.pxc` re-read halving as a secondary signal.

### A/B measurement (on-device, via existing + one new LOG_DBG)
- Compare `ERS` fields before/after on the same book:
  - tiled async: `gray_render` (:1779) pre → `gray_both` post (expect ~½).
  - tiled non-overlap: `gray_lsb + gray_msb` (:1829) pre → `gray_both` post (expect ~½).
  - nontiled (regression baseline): `gray_lsb + gray_msb` (:1862) unchanged.
- `total` (:1780/:1830/:1864) shrinks by the same delta (minus the new bit-write cost).
- `gray_display` / `gray_write` unchanged (same plane byte volume).
- Add one `LOG_DBG("ERS", "gray heap before/after: %u/%u", …)` around the gray block to prove the 16 KB
  scratch returns to baseline after scope exit (no leak, no fragmentation).

---

## 9. Open questions (do not block phase 1)

- **Q1.** The `renderCharImpl`/`scaled` decomposition assumes the glyph 2-bit tone is the *only* gray
  source. Are there other 2-bit tone producers bypassing `renderCharImpl`? `DirectPixelWriter` (images,
  #7) is the other — handled. `drawCharDither` (#8) is BW-only dither, not a plane source. Confirmed by
  grep (see §4): `renderMode` switch arms exist only in the four paths listed.
- **Q2.** Does `drawTextRotated90CW` (GfxRenderer.cpp:2163) reach gray planes? It delegates to
  `renderCharImpl<Rotated90CW>` with `renderMode` (the two instantiation calls at :2211/:2231), so it is
  covered by #1's template — no separate switch. Verified: no own `renderMode` branch in the rotated
  path (the "Mirrors renderCharImpl" comment is at :2236).
- **Q3.** `preserveImagePolarity` under a strip target: `getWriteTarget()` (GfxRenderer.h:248) returns
  `_stripBuf` when a strip is active. Dual mode must point the polarity RMW at *both* strip buffers.
  Exact handling deferred to phase 2's image-polarity fix.
