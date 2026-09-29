# Hint-stack depth bench results (2026-09-29)

Source of truth for `BookFontLoader`'s hint-probe budget
(`kHintProbeStackBudgetBytes`, `kHintConsumerStackBytes`,
`kHintProbeStackMarginBytes` in `src/BookFontLoader.cpp`). Regenerate with:

```sh
cmake -S test -B build/test -DCROSSPOINT_BUILD_BENCH=ON
cmake --build build/test --target HintMemBench
./build/test/hint_mem_bench/HintMemBench
```

## What the bench measures

The deepest byte a real firmware call reaches below a fresh stack top, in
bytes — the same measurement IDF's `uxTaskGetStackHighWaterMark` performs
(`prvTaskCheckFreeStackSpace`, a memory scan for the stack-fill byte). The
faces are loaded by the real `BookFontLoader` through its PSRAM tier with the
real fixture fonts; the unhinted leg is produced with the same
`FtFont::setRenderOptions(HintingMode::None)` call `degradeHint()` makes. The
glyph cache is disabled (`setGlyphCacheBudget(0)`): the uncached worst case is
what must fit, since the first render of a face is exactly what the device
probe measures.

Two consumer call sets are measured separately, because the hint verdict is
shared by both:

- **render** — `FontChain::fontFor()` + `RasterFont::rasterize()`: bitmap
  production, runs on the Arduino loop task.
- **layout** — `lineHeight/ascent/advance/kerning`: the only font calls
  `ChapterLayout` (and therefore `FibpPrefetchWorker`) makes; the worker never
  rasterizes.

`first` is the first call of the sequence; `steady` is the deepest call after
it. Point sizes are the firmware's real selection: `BUILTIN_READER_POINT_SIZES`
= {12, 14, 16, 18} pt plus the SD clamp range's 8 and 72 pt, converted with
px = round(pt · 150 / 72).

## Results (host, 64-bit; 8 MB measured stack)

| family | format | call set | glyph set | hinting | pt | px | first | steady |
|---|---|---|---|---|---|---|---|---|
| Amazon Ember | TTF (glyf) | render | probe stress | None | 12–72 | 17–150 | 4536 | 4592 |
| Amazon Ember | TTF (glyf) | render | page mix | None | 12–72 | 17–150 | 4536 | 4592 |
| Amazon Ember | TTF (glyf) | render | probe stress | Light | 12 | 25 | **29544** | 14104 |
| Amazon Ember | TTF (glyf) | render | page mix | Light | 12 | 25 | 14104 | 14104 |
| Amazon Ember | TTF (glyf) | render | both | Light | 14–72 | 29–150 | 14104 | 14104 |
| Amazon Ember | TTF (glyf) | layout | both | None/Light | 8–72 | 17–150 | 0–824 | 0 |
| Atkinson | OTF (CFF) | render | probe stress | None | 12–72 | 17–150 | 29360 | 29360 |
| Atkinson | OTF (CFF) | render | page mix | None | 12–72 | 17–150 | 28992 | 29016 |
| Atkinson | OTF (CFF) | render | both | Light | 8–72 | 17–150 | 28992 | 29016 |
| Atkinson | OTF (CFF) | layout | both | None/Light | 8–72 | 17–150 | 0 | 0 |

## Findings

1. **The depth is a per-face, per-format constant, not a size or glyph
   artifact.** Depth does not move across 8–72 pt (17–150 px) and does not
   move between the probe's own 6-codepoint stress set and a realistic page
   mix. The probe's stress set is representative; hypothesis (i) is rejected.
2. **Hinting is the TrueType stack cost.** Ember renders unhinted at ~4.5 KB
   and hinted at ~14.1 KB steady-state. The autohinter's one-time blue-zone
   setup puts a ~29.5 KB transient on the FIRST hinted glyph (measured only at
   the very first call of a session); every call after it is ~14.1 KB.
3. **CFF costs ~29 KB hinted AND unhinted — identical.** The Adobe CFF
   charstring interpreter, not hinting, is the depth. Degrading a CFF face to
   `HintingMode::None` buys **zero** stack headroom while removing hinting
   quality: as a safety mechanism the degrade is void for CFF.
4. **The layout path never pays the hinted depth.** `FtFont::advance()` reads
   `FT_LOAD_NO_HINTING | FT_ADVANCE_FLAG_FAST_ONLY` and the FIBP worker never
   rasterizes, so its measured depth is ~0 regardless of the hint verdict.
   The 32 KB worker this gate used to budget for is not a hinted-render
   consumer at all — keying a shared budget to it was wrong by construction.

## Device-vs-host correspondence

The device probe (`Hint probe slot N consumed X B`) reports 32-bit Xtensa
numbers; the bench reports 64-bit host numbers.

| face | device probe | host steady | host first |
|---|---|---|---|
| Amazon Ember (TTF) | 15988 B | 14104 B | 29544 B |
| Atkinson (CFF) | 28364 B | 29016 B | 28992 B |

CFF matches almost exactly (the CFF interpreter's frames are pointer-width
dominated). The TTF device number matches the host steady-state, not the host
first-glyph transient: on device the autohinter's blue zones are built during
face init at `kInitSizePx` (14 px, the probe's own first size leg), so the
probe never observes the transient the host sees on a cold face.

## Verdict: (iii) — the real cost exceeds the old budget; the budget is re-derived

The measured render depth (14–30 KB hinted TTF, ~29 KB CFF either way)
genuinely exceeds the previous flat 8 KB budget, so every hinted face was
degraded to None — the owner's regression. The budget is now **derived**:

```
kHintConsumerStackBytes    = 48 KB  // loop task — the only consumer that rasterizes
kHintProbeStackMarginBytes =  8 KB  // render caller frames + streamed-face SD tail
kHintProbeStackBudgetBytes = 40 KB
```

- The binding consumer is the **48 KB loop task** (the render path), not the
  32 KB FIBP worker (layout only, ~0 depth).
- The margin reserves what the probe cannot see: the reader's render caller
  frames and the SD tail a streamed face reads under the render
  (`streamReadThunk` → `HalFile::read` → HalStorage mutex → SdFat).
- Ember (device 15988 B) and Atkinson (device 28364 B) both fit: hinting is
  restored for both, and any future face measuring > 40 KB still fails closed.

### x4pro DRAM math (PSRAM enabled, boot free ≈ 164 KB, book_open ≈ 128 KB)

| path | stack | deepest measured need | headroom |
|---|---|---|---|
| Loop task, hinted CFF page | 48 KB | ~29 KB (face) + ~5 KB caller frames | ~14 KB |
| Loop task, hinted TTF page | 48 KB | ~16 KB device / 29.5 KB worst first-call | ~18–32 KB |
| FibpPrefetchWorker build | 32 KB | ~0 KB font depth (metrics only) | ~30 KB |
| Probe task (transient, this PR) | 56 KB = consumer + margin | measures up to 40 KB then degrades | n/a |

The probe task grew 32 → 48 KB so a render deeper than a consumer is reported
rather than crashing the measurer. Its +16 KB is transient at book-open
against ~128 KB free (one probe task live at a time, reaped before the next).
No consumer stack size is changed by this work; growing the 32 KB worker is an
owner decision that would buy nothing for hinting (it never hints) — the
loop task already has the headroom the honest budget requires.

## Coverage note

`probeHintStackSafety()` itself is `#if defined(ARDUINO)` and compiles to a
no-op on host, so no host test can execute the probe task body; the bench
measures the same render/layout calls through the real loader with the same
measurement methodology instead. Depth assertions are deliberately NOT ctest
cases (flaky across compilers/hosts).
