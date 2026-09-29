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
| Loosen the budget / drop the fail-closed floor | The 17.5 KB "cost" is not the font's cost; raising the budget to admit it would admit the caller's depth too. Failure to measure still fails closed (task spawn/settle failure), and an unattributable delta can never be read as "safe". |
| Move the render pool / task stacks to PSRAM | The S3 build already sets `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`; the C3 floor target has no PSRAM at all, and the accesses are unavailable while the flash cache is off during OTA. PSRAM stacks are also ~10× slower, and the probe wants cheap contiguous headroom. |
| Fix the `ensureLoaded()` unload path | It is correct and untouched here. |

## 3. The fix

### 3.1 Measure on a dedicated task, triggered from a shallow site

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
`EpubReaderActivity::renderBookTtf()` (`EpubReaderActivity.cpp:2875`),
immediately after `ttf_->makeLayoutParams()` returns a usable chain:

```cpp
ttf_->makeLayoutParams(renderer, params, automaticPageTurnActive);
if (params.font == nullptr) { showBuildError(); return; }
freeink::book::fontLoader.ensureHintProbeSettled();
const uint32_t generation = layoutGenerationHash(params, fontLoader.fontFingerprint());
```

**The measurement itself runs on a dedicated one-shot task, not the calling
frame.** The first implementation kept the measurement on the loop task and
relocated it to the reader's shallow first render — and the owner's X4 Pro
log disproved that sufficiency: after popping the font preview (settings
path, ~34 KB deep), the loop task's `uxTaskGetStackHighWaterMark` stayed
below the fail-closed floor for the rest of the boot, so the reader's
shallow probe read `consumed == 0, HWM 14004 B < floor 27648 B` on slots 1-3 and
degraded every face again — lifetime high-water cannot be relocated away,
only measured around. The probe therefore no longer rasterizes on the
calling task at all: `probeHintStackSafety()` spawns a one-shot 32 KB task
(the exact `FibpPrefetchWorker` stack size being protected, DRAM, core 0,
priority 1 — worker-task `kCore` convention), whose only work is the probe
rasterizations, and whose entry high-water starts at the full stack size —
so `consumed = entry HWM - exit HWM` IS the face's whole render depth, with
no caller frames in it. The loop task blocks on the probe's completion
semaphore for its whole lifetime, so no second task calls into the FreeType
faces concurrently (the same main-blocked/worker-rasterizes discipline
`FibpPrefetchWorker` already runs on device). The probe task is reaped by
the waiter; on a settle timeout its readings are unattributable, so the
loader fails closed (degrades every loaded slot) and leaves the verdict
UNCACHED — and both the still-parked task and its semaphore are logged and
leaked, the same policy as the prefetch worker's wedged join (deleting a
task that may be mid-`FT_Render_Glyph` is not safe).

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
computed it, before any verdict was applied — combined with the **body point
size the probe measured at**. The fingerprint folds the face bytes, the path
hash, the collection face index, the file size and the mtime; the size joins
it because `probeHintStackSafety()` rasterizes at
`SETTINGS.ttfFontPointSize`-derived sizes and the Adobe charstring depth is a
function of the glyph size, so a size change must re-probe rather than inherit
an old-size verdict. The key therefore gives:

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
| `hintVerdictSizePt_` | body point size the cached mask was measured at |
| `hintProbePending_` | the current face set has no settled verdict |

A verdict whose reading was **unattributable** is never cached:
`probeHintStackSafety()` also reports whether NO measurement could be made
(probe-task spawn failure, semaphore allocation failure, or a settle
timeout). That outcome describes the loader's environment rather than the
font, so every loaded slot still degrades for the rest of the session (fail
closed), but the cache key is cleared and the next load re-measures instead
of freezing a failed probe as a font property. The 8 KB budget and the
unload path are unchanged.

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

That parity also fixes *which* coverage is folded. The fallback tail is a
constant both sites register, so hashing `chain_.styleCoverage()` directly
would make the identity depend on where the hash is evaluated: the loader now
computes the fingerprint after the tail joins the chain, while
`FibpPrefetchWorker::buildFaces()` folds coverage before appending it, and for
any family that does not already cover all four styles the two values diverge
and the worker's FIBP indexes are written under a generation the reader never
looks up. `ensureLoaded()` therefore captures the family coverage into
`fingerprintCoverage_` **before** the tail is appended, and both fingerprint
functions fold that stored value, so the identity is the pre-tail one wherever
it is evaluated — the value the worker's parity hash already produced, and the
value `computeFingerprint()` returns.

**Consequence, accepted:** a device that first settles a verdict for a book
regenerates that book's `.fibp` index once. A one-time re-index is the
correct price for an identity that always describes what renders.

### 3.4 Logging

A verdict is a designed fallback to unhinted rendering, not a fault: the
glyphs stay correct, only the grid-fit is dropped. The two degrade paths
therefore log at `LOG_INF` with the slot and the reason, and the per-slot
`consumed` measurement line stays at `LOG_DBG` — it is the diagnostic that
distinguishes a genuinely deep face from a shallow one, so it must be
available on a debug build without crying wolf on a release build.

## 4. Residual risk (documented, not fixed here)

A deep-path rasterization that happens **before** the reader's first TTF
render can still run a hinted face from a deep settings frame:
`TextSettingsPreview` rasterizes through the reader chain
(`TextSettingsPreview.cpp:252`, `:271`). The probe redesign covers the
VERDICT (it can no longer be wrong because of the caller), not the deep
rasterization itself: the Adobe footprint of a previewed hinted CFF face
still runs on the 48 KB loop task at the depth the settings activity leaves
it, with no measurement of whether it fits. Making that structurally safe
means either servicing the preview with a `QuickSink` that never touches a
hinted face or probing from the preview path too (on the dedicated task,
which the loading order would gate) — tracked as a follow-up; the exposure is
bounded to a settings preview of a hinted face on a device whose hinting
verdict is not yet settled.

## 5. Tests

`BookFontLoaderHinting.VerdictReusesOnUnchangedReloadAndMissesOnChangedBytes`
(`test/book_font_loader/BookFontLoaderTest.cpp`) pins the contract on host,
where the FreeRTOS measurement itself is compiled out, through the
`recordHintVerdictForTest` seam:

1. settling a verdict for a face set moves the tag and re-derives the
   fingerprint (identity follows the verdict), while `computeFingerprint()`
   and the loader agree (the identity does not depend on the fallback tail);
2. a second `ensureHintProbeSettled()` is a no-op (one-shot per load);
3. a reload of unchanged bytes re-applies the verdict and returns the
   identical fingerprint (reuse, no needless re-index);
4. changed bytes miss the cache, return the slot to the requested mode, and
   move the fingerprint (a verdict never outlives its face set);
5. a body-size change misses the cache and returns the slot to the requested
   mode (the verdict is a function of the size it was measured at).

The negative control is step 3: without `reuseHintVerdicts()` the reload
would reset the slot and step 3 would fail, and without the re-derivation in
step 1 `fpDegraded` would equal `fpRequested`.

## 6. Doc correction

`docs/design/2026-09-18-freetype-backend-as-built.md` §8 stated "Hinting
stays OFF" — true for the `43fed43` pin it describes, stale as a description
of the shipped reader. The bullet now states the current mechanism (Light
requested, gate-enforced per face, identity-folded) and points here.
