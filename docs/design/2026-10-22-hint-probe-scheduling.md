# Hint probe scheduling: measure before the reader fragments the heap

Date: 2026-10-22
Owner: Antoine Aflalo (Belphemur)
Status: Draft → implemented by pi under supervision
Related: `2026-09-29-hint-probe-measurement.md`, `2026-09-29-hint-stack-bench-results.md`

## 1. Problem

On x4pro, hinting degrades to `None` for **all** slots on **every** reader
session. Device log (3 runs, 100% reproducible, 2026-10-22):

```
[CPRO] open: free=107044 max_block=65524      ← pre-book allocations
[MEM] Stage=book_open HeapFree=93KB
[ERR] [BFNT] Hint probe task spawn failed      ← instant, no timeout
[INF] [BFNT] Hinting degraded to None for face slot 0..3 (stack probe: unmeasurable)
```

Root cause — **spawn failure, not measurement**:

- The dedicated probe task needs `kHintProbeTaskStackBytes` = 48 KB (consumer)
  + 8 KB (margin) = **57,344 B contiguous internal DRAM**
  (`src/BookFontLoader.cpp:271`).
- At the probe's only trigger point — `ensureHintProbeSettled()` called from
  the reader's first TTF render (`EpubReaderActivity.cpp:3532`) — book-open
  allocations have already fragmented the heap. Lifetime telemetry in the
  same session: `MaxAlloc 47,092 B` (reader) / `51,188 B` (after reader exit)
  against 88–108 KB free. **93 KB free but the largest block is ~47 KB** —
  the 57 KB stack cannot be allocated. `xTaskCreatePinnedToCore` fails, and
  the fail-closed path (`degradeAllLoadedSlots("stack probe: unmeasurable")`,
  `BookFontLoader.cpp:337`) kills hinting for every loaded slot.
- The verdict from an unmeasurable probe is deliberately **not cached**
  (`ensureHintProbeSettled`, `BookFontLoader.cpp:424-434`), so every family
  load / body-size change re-arms the probe, fails again, and degrades again.
  Effectively hinting is permanently off.
- The window where it fits existed seconds earlier: `max_block=65524` at
  chapter-progress open, before the book-open allocations. 65,524 ≥ 57,344
  with 8 KB of slack.
- NOT BLE-related: reproduced with the page turner disabled (owner device).
- The bench doc's DRAM table (`hint-stack-bench-results.md` §"x4pro DRAM
  math": "book_open ≈ 128 KB") is stale versus today's resident set — the
  budget derivation stands (measured face depths 16–28 KB < 40 KB budget),
  but the *schedulability* of the probe task inside the reader is what broke.

## 2. Goal

Hinting works on x4pro in the normal reading flow, and the stack-safety
guarantee is preserved: no hinted glyph renders unless a settled probe
verdict cleared that face.

Non-goals:
- Changing the probe's measurement methodology (the dedicated virgin task
  stays; the caller-contamination bug must NOT come back).
- Changing the 40 KB budget derivation (`kHintProbeStackBudgetBytes`).
- Re-introducing the DRAM font-byte fallback tier (removed by directive).
- PSRAM task stacks (rejected: flash-cache-disabled windows, C3 has none).

## 3. Design

### D1 — Probe runs at the shallow window, immediately after face load

Move the probe trigger from the reader's first TTF render to the tail of
`BookFontLoader::ensureLoaded()` (right where `hintProbePending_` is currently
set, after `reuseHintVerdicts` decides a fresh probe is owed).

Rationale and properties:

- At `ensureLoaded()` time the heap is at its shallowest inside the reader:
  the log's `max_block=65524` happens in this neighborhood (chapter-progress
  open precedes the heavier page-build allocations). 65,524 ≥ 57,344 fits.
- The verdict cache already makes one successful measurement cover the whole
  session for an unchanged face set at the same body size
  (`reuseHintVerdicts`), so running early costs nothing later.
- The probe measures faces loaded into PSRAM and renders from resident
  bytes — it does not depend on the book's page-build state, so running it
  before page build is valid.
- Fingerprint sequencing is unchanged: the probe already runs before
  `computeFingerprintCached()` is consulted for the generation
  (`recordHintVerdict` re-derives on degrade), and the worker starts after
  (`updateFibpWorker` happens after `ensureHintProbeSettled` today). Moving
  the settle earlier keeps the same ordering invariant: **no hinted
  rasterization and no FIBP worker start before the verdict is settled.**

### D2 — Blocking discipline inside `ensureLoaded()`

`ensureLoaded()` runs on the loop task. The probe blocks the caller on the
probe's completion semaphore for up to 5 s per face (`kHintProbeSettleTimeoutTicks`).
This cost moves from the first page render (where it already sat) to the load
path — the user-visible effect is a possibly longer "Opening book" moment and
a shorter first-page delay; net perceived latency is unchanged or better.

Constraint kept: the loop task must not touch the FreeType faces while the
probe task rasterizes them. The existing discipline (caller blocks on
`req.done`, strict suspend+delete join, degrades after the join) is
unchanged — only the call site moves.

### D3 — Fail-closed stays, with a retry backstop

- Spawn failure or settle timeout at the shallow point still degrades all
  loaded slots for the session (unchanged semantics, unchanged log reason
  string `stack probe: unmeasurable`).
- NEW backstop: if a probe attempt ended unmeasurable, the loader keeps a
  one-shot retry pending for the NEXT `ensureHintProbeSettled()` call
  (the reader's first-render site stays as the backstop call site — it is
  already there and already correct). Retry rules:
  - Retries are cheap only if they cannot thrash: at most **one** retry per
    `ensureLoaded()` cycle (a `retryArmed_` flag consumed on first attempt).
  - On retry success, the verdict is recorded normally and hinting upgrades
    (fingerprint re-derives; FIBP caches re-index once — accepted, one-time).
  - On retry failure, the session stays degraded; no further retries this
    session.
- This bounds the worst case: hinting is lost only when BOTH the shallow
  window AND the first-render point cannot spawn a 57 KB task.

### D4 — Host tests

The probe is `#if defined(ARDUINO)` and compiles out on host (unchanged).
Host-testable surface added:

- `ensureLoaded()` sets `hintProbePending_` (existing) AND `retryArmed_`
  semantics: a `HintingProbeRetryTest` pins that (a) a fresh load with no
  cached verdict leaves a pending probe + one retry armed, (b) consuming the
  retry disarms it, (c) a failed retry does not re-arm, (d) a successful
  measurement clears both.
- Existing `BookFontLoaderTest` verdict-cache tests must keep passing
  unchanged — the cache key/semantics are not modified.

### D5 — Telemetry

One `LOG_INF` when the retry path runs (`"BFNT: Hint probe retry armed after unmeasurable attempt"`),
so the soak distinguishes "shallow spawn failed" from "both windows failed".
The degrade logs stay `LOG_INF` (per the earlier decision — normal fallback
noise, not a fault).

### D6 — Docs updated in the same change

- `docs/design/2026-09-29-hint-stack-bench-results.md`: add a 2026-10-22
  addendum — measured x4pro heap at probe time (93 KB free / ~47 KB largest
  block; `max_block=65524` pre-allocations), spawn-failure failure mode, and
  the corrected DRAM math superseding the "book_open ≈ 128 KB" line.
- `CHANGELOG.md` entry under Fixed (owner will edit wording at merge; agent
  provides a factual draft).
- The `xpoint-bookfont-loader` skill's failure-mode section gains the fix
  note at merge time (owner's post-merge workflow).

## 4. Risks

| Risk | Mitigation |
|---|---|
| `ensureLoaded()` runs at boot for UI fallback before the reader exists — probing there would measure at boot when the heap is whole (fine) but adds SD+probe latency to boot | Only reader-relevant loads probe: the trigger is the existing `hintProbePending_` flag, which only reader renders ever consumed. If boot-path loads probe too, that is acceptable ONLY if boot latency stays under ~2 s; the soak will show it. If it does not, gate the early probe to FT-backend loads that follow a `selectFamily` (reader path), keeping UI-fallback loads on the deferred path. |
| The 65 KB block observed may itself be transient (e.g. CPRO open window) | The retry backstop (D3) covers it; worst case is today's behavior. |
| Blocking ensureLoaded for up to 4×5 s worst case (4 faces × timeout) | Only on pathological faces; typical measured depth completes in ms. Existing timeout bound retained. |
| Cache re-index churn when a retry upgrades hinting | One-time per session, by design of the fingerprint fold. |

## 5. Verification

1. Host: `test/book_font_loader` suite green in both backend variants
   (stb + BackendFT) with the new retry tests.
2. CI: full matrix green (clang-format on ALL files via bare
   `./bin/clang-format-fix`, cppcheck, 8 board builds).
3. Device soak (owner): x4pro flash of the branch → boot → open the same
   book → serial log must show either (a) `Hint probe slot N consumed ...`
   with `consumed ≤ 40960` and NO degrade lines, or (b) the retry line and a
   later settle. Confirm hinted rendering visually (crisper stems at body
   sizes in Crisp mode) and confirm FIBP caches regenerate once (gen change)
   without errors.
4. Regression guard: BLE page-turner enabled + bonded + linked → repeat (3):
   hinting must survive the radio being up (radio stops before the probe
   window or the shallow window still fits beside the radio's 46 KB; the
   radio's home-start policy keeps the heap whole at load time — verify with
   the `CPRO open` max_block line in the log).
