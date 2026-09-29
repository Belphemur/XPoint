# Hint-probe measurement: call depth vs. font cost

Date: 2026-09-29
Scope: `src/BookFontLoader.{h,cpp}`, `src/activities/reader/EpubReaderActivity.cpp`
Status: as-built

## 1. Symptom

On device the P2 hint-stack gate degraded **every** face slot to
`HintingMode::None`, so hinted reading was silently off everywhere:

```text
BFNT : Hint probe slot 0 consumed 17536 B
BFNT : Hint probe slot 1 unmeasurable (HWM 13980 B < floor, no delta)
BFNT : Hint probe slot 2 unmeasurable (HWM 13980 B < floor, no delta)
BFNT : Hint probe slot 3 unmeasurable (HWM 13980 B < floor, no delta)
```

Slot 0 tripped the budget (`kHintProbeStackBudgetBytes` = 8 KB) with a 17.5 KB
"cost"; slots 1–3 could not be measured at all, and the fail-closed
unmeasurable rule (`consumed == 0 && after < kHintProbeFloorBytes`) took them
down with it. Both symptoms have one cause: **the measurement was attributing
caller stack to the font.**

## 2. Root cause

`probeHintStackSafety()` ran inside `ensureLoaded()`, and `ensureLoaded()` is
reached from the deepest settings path in the firmware:

```text
main.cpp:973                    loop()  → ActivityManager (loopTask, 48 KB stack)
  TextSettingsActivity.cpp:167  ttfUiFallback.update(renderer)
    TtfUiFallback.cpp:59          fontLoader.ensureLoaded() → probe
```

By the time the settings font sheet reached the probe, the loop task's own
frames were ~34 KB deep — about 14 KB of headroom left under a 48 KB stack.
`uxTaskGetStackHighWaterMark(nullptr)` is a *lifetime* high-water of the
calling task, so a probe nested under those frames attributes the caller's
depth to whatever it rasterizes:

- slot 0: the already-deep high-water plus the Adobe CFF interpreter's
  multi-KB caller-stack charstring depth crossed 8 KB → false positive degrade;
- slots 1–3: the high-water could not move at all (the caller was already
  deeper than the probe's transient depth), so the before/after delta was 0
  while the task history was below the floor → unmeasurable → fail closed.

The guard itself was right. The measurement context was wrong.

### Rejected alternatives

| Option | Why not |
| --- | --- |
| Disable the probe | A hinted CFF face on the 32 KB `FibpPrefetchWorker` blows the stack (the reason the probe exists). Trading a measurement bug for a stack overflow is not a fix. |
| Loosen the budget / drop the fail-closed floor | The 17.5 KB "cost" is not the font's cost; raising the budget to admit it would admit the caller's depth too. The floor exists precisely so an unattributable delta cannot be read as "safe". |
| Move the render pool / task stacks to PSRAM | The S3 build already sets `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`; the C3 floor target has no PSRAM at all, and the accesses are unavailable while the flash cache is off during OTA. PSRAM stacks are also ~10× slower, and the probe wants cheap contiguous headroom. |
| Fix the `ensureLoaded()` unload path | It is correct and untouched here. |

## 3. The fix

### 3.1 Measure from a shallow frame

`ensureLoaded()` no longer probes. The measurement moved to a public,
explicitly-called half of the gate:

```cpp
// BookFontLoader.h
void ensureHintProbeSettled();
```

`ensureHintProbeSettled()` is a no-op when the current face set already has a
settled verdict or when nothing is loaded, so the call site can be a plain
statement without a guard. On the stb backend it is defined empty (no render
options to gate), matching `probeHintStackSafety()`'s own `#else`.

The call site is the reader's first TTF render,
`EpubReaderActivity::renderBookTtf()` (`EpubReaderActivity.cpp:2873`),
immediately after `ttf_->makeLayoutParams()` returns a usable chain:

```cpp
ttf_->makeLayoutParams(renderer, params, automaticPageTurnActive);
if (params.font == nullptr) { showBuildError(); return; }
freeink::book::fontLoader.ensureHintProbeSettled();
const uint32_t generation = layoutGenerationHash(params, fontLoader.fontFingerprint());
```

That frame is shallow: `renderBookTtf()` is reached from `loop()` with the
activity stack and nothing else on it. The reader's own transient depth is
also the depth the probe's own guard is written against, so the "consumed"
delta is attributable to the face.

**Ordering is a hard constraint, not an optimization.** The verdict must
settle before *anything* rasterizes a hinted glyph under it, and before the
FIBP prefetch worker starts, because the worker's faces are created with
`BookFontLoader::effectiveRenderOptions(i)` and its parity hash xors
`renderOptionsFingerprintTag()`. The call site sits:

- after `makeLayoutParams()` → the family is loaded (`loaded_` is true, so
  the probe has faces to rasterize) but no glyph of this render pass has
  been painted yet;
- before `layoutGenerationHash(...)` → layout identity is read from the
  post-verdict fingerprint;
- before `updateFibpWorker(generation, params)` → the only place the worker
  is spawned (`ttfBackgroundBuildTick()` can only stop it);
- before `ttfRunPreRenderPass(params)` and `ttfFastDisplayPass(params)`, the
  first things in the pass that can rasterize.

`ensureTask()` at the later index-ahead call site is already downstream of
this line.

### 3.2 Cache the verdict per face-set identity

The measurement is a property of the face bytes and the interpreter build, not
of when the load happened or of the raster mode — the same argument
`applyRenderMode()` already makes for re-deriving the fingerprint. The verdict
is therefore cached, and the cache key is the **content fingerprint captured
under the requested render options** — `fingerprint_` as `ensureLoaded()`
computed it, before any verdict was applied. That value folds the face bytes,
the path hash, the collection face index, the file size and the mtime, so:

- a plain reload of unchanged faces hits the cache and re-applies the
  verdict instead of re-probing (and the reload's fingerprint comes back
  identical, so no FIBP re-index);
- replaced or re-pointed bytes miss it, return every slot to the requested
  mode, and re-arm the probe.

State (FT build only, `BookFontLoader.h`):

| Member | Meaning |
| --- | --- |
| `hintVerdictKey_` | load key the cached mask was measured under; `0` = none |
| `hintVerdictMask_` | bit *i* = slot *i* measured too deep |
| `hintProbePending_` | the current face set has no settled verdict |

`ensureLoaded()`'s clear loop already calls `resetHintState()` per slot
(`BookFontLoader.cpp:591`), so a reload starts from the requested mode and the
cached verdict is the only thing that can degrade a slot — there is no
double-application and no stale-mode compounding.

`releaseResidentCaches()` drops the key and the mask and re-arms the pending
flag: with nothing resident, a verdict measured against faces that are no
longer in memory must not be trusted.

**Polarity is deliberately unchanged.** `resetHintState()` restores the
requested mode and is *not* paired with a verdict: an unprobed load simply
serves the requested mode until the pending probe (or a cached verdict for the
same face set) settles it. Making the reset also erase the cache would force
a re-probe on every reload; keeping the cache and resetting the mode is what
makes the reuse path meaningful.

### 3.3 Layout identity follows the verdict

`renderOptionsFingerprintTag()` folds each slot's *effective* hinting mode
into the FIBP generation. The relocated probe therefore changes a value the
fingerprint is computed from, at a point where `computeFingerprintCached()`
has already run. `recordHintVerdict()` re-derives the fingerprint when the
verdict moved a slot, guarded by `loaded_` — the same guard and the same
pattern `applyRenderMode()` uses (`if (loaded_) fingerprint_ =
computeFingerprintCached();`):

```cpp
if (degradedMask != 0 && loaded_) fingerprint_ = computeFingerprintCached();
```

The reuse path in `ensureLoaded()` mirrors it: the verdict is re-applied
*before* the identity is read by the reader, and the fingerprint is
recomputed if the re-application moved a slot. Both fingerprint sites — the
loader's and the worker's parity hash — therefore fold the same effective
modes, and the worker never renders unhinted glyphs under a hinted identity.

**Consequence, accepted:** a device that first settles a verdict for a book
regenerates that book's `.fibp` index once. A one-time re-index is the
correct price for an identity that always describes what renders.

### 3.4 Logging

A verdict is a designed fallback to unhinted rendering, not a fault: the
glyphs stay correct, only the grid-fit is dropped. The two degrade paths
therefore log at `LOG_INF` with the slot and the reason, and the per-slot
`consumed` measurement line stays at `LOG_DBG` — it is the diagnostic that
distinguishes a real deep face from a contaminated measurement, so it must be
available on a debug build without crying wolf on a release build.

## 4. Residual risk (documented, not fixed here)

A deep-path rasterization that happens **before** the reader's first TTF
render can still run a hinted face: `TextSettingsPreview` rasterizes through
the reader chain (`TextSettingsPreview.cpp:252`, `:271`). That path is exactly
the one that produced the bad measurement, so it is now a known exposure: a
hinted CFF face may be rasterized once, from a deep frame, before any verdict
exists. It cannot overflow (that is a pool-task property, unchanged) and the
verdict settles on the reader's first render, so the exposure is bounded to
the settings preview. Making it structurally impossible means either probing
from the settings path too — which reintroduces the contamination — or
servicing the preview with a `QuickSink` that never touches a hinted face.
Tracked as a follow-up.

## 5. Tests

`BookFontLoaderHinting.VerdictReusesOnUnchangedReloadAndMissesOnChangedBytes`
(`test/book_font_loader/BookFontLoaderTest.cpp`) pins the contract on host,
where the FreeRTOS measurement itself is compiled out, through the
`recordHintVerdictForTest` seam:

1. settling a verdict for a face set moves the tag and re-derives the
   fingerprint (identity follows the verdict);
2. a second `ensureHintProbeSettled()` is a no-op (one-shot per load);
3. a reload of unchanged bytes re-applies the verdict and returns the
   identical fingerprint (reuse, no needless re-index);
4. changed bytes miss the cache, return the slot to the requested mode, and
   move the fingerprint (a verdict never outlives its face set).

The negative control is step 3: without `reuseHintVerdicts()` the reload
would reset the slot and step 3 would fail, and without the re-derivation in
step 1 `fpDegraded` would equal `fpRequested`.

## 6. Doc correction

`docs/design/2026-09-18-freetype-backend-as-built.md` §8 stated "Hinting
stays OFF" — true for the `43fed43` pin it describes, stale as a description
of the shipped reader. The bullet now states the current mechanism (Light
requested, gate-enforced per face, identity-folded) and points here.
