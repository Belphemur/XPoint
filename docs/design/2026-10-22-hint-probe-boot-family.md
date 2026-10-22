# Hint probe boot-family selection: measure the faces the reader opens

Date: 2026-10-22
Owner: Antoine Aflalo (Belphemur)
Status: Draft → implemented by pi under supervision
Related: `2026-10-22-hint-probe-scheduling.md` (shallow-window settle + retry),
`2026-09-29-hint-stack-bench-results.md` (budget derivation)

## 1. Problem

Soak firmware `2.9.1-dev-feat/hint-probe-schedule-f8a2754c` (2026-10-22,
21:37 UTC-4): the shallow-window settle and the retry backstop from the
previous round both work as designed — and hinting is STILL off for the whole
reading session, because **both probe windows spawn into an already
fragmented heap**:

```
21:37:38 boot:  [DBG] [BFNT] Hint probe slot 0 consumed 16048 B      ← probe SUCCEEDS (Stage=boot, HeapFree=130KB)
21:37:49 open:  [CPRO] open: free=105356 max_block=63476             ← book engine open
                book arena used=6332; ZIP cache 93; CSS 72; spine caches 60/60
21:37:49        [ERR] [BFNT] Hint probe task spawn failed            ← 1st attempt (load tail)
21:37:49        [INF] [BFNT] Hint probe retry armed after unmeasurable attempt
                [ERR] [BFNT] Hint probe task spawn failed            ← retry, same second
                4x [INF] Hinting degraded to None for face slot N (stack probe: unmeasurable)
21:37:51 render:[MEM] Free: 87748 ... MaxAlloc: 45044
```

The probe task needs `kHintProbeTaskStackBytes` = 57,344 B contiguous DRAM
(`src/BookFontLoader.cpp:288`). After the book engine's arena + ZIP/CSS/spine
caches are resident, MaxAlloc is 45–63 KB — **both** the ensureLoaded-tail
window and the first-render retry backstop are downstream of those
allocations, so the retry cannot help. Fail-closed did its job; the net
effect is unhinted text on every book open.

## 2. Root cause (F1a): the boot verdict exists — for the wrong face set

The probe DID succeed at boot and DID cache a verdict. It just measured a
family the reader never opens:

1. Boot calls `ttfUiFallback.update(renderer)` (`src/main.cpp:831`), whose
   `ensureLoaded()` (`src/TtfUiFallback.cpp:64`) runs **before** the PSRAM
   registration gate — the boot load+probe is unconditional once the engine
   gate passes.
2. No `selectFamily()` has run yet, so the unselected loader keeps the legacy
   default and loads **`families_[0]`** (`src/BookFontLoader.cpp:855-859`) —
   the first discovered family (hidden root `/.fonts` scanned first). The
   boot log's single `Hint probe slot 0 consumed ...` line shows that set
   contributed exactly one loadable face.
3. `recordHintVerdict()` caches the verdict under the load key
   (`src/BookFontLoader.cpp:429`) — the fingerprint of *families_[0]*'s face
   set. The fingerprint is a chained per-face content identity (path hash,
   size, mtime, bytes, role map, render-options tag —
   `computeFingerprintCached()`, `src/BookFontLoader.cpp:1204-1250`), so it
   is specific to the exact faces probed.
4. Book open: `makeLayoutParams()` selects the settings family
   (`src/activities/reader/TtfBookRuntime.cpp:500-502`) → dirty →
   `ensureLoaded()` reloads with the user family (4 style faces) → the new
   fingerprint cannot match the cached key → `reuseHintVerdicts()` misses
   (`src/BookFontLoader.cpp:446`) → a fresh probe is owed → spawn fails
   twice → all 4 slots degraded.

**The cache miss is correct behaviour, not a cache bug.** Hinted render depth
is a property of the face bytes (a CFF face measures ~29 KB where a TTF face
measures ~14 KB — bench doc §results), so a verdict measured on families_[0]
must never clear the user family's faces. Loosening the key to family names
or load sessions would be unsound. The defect is upstream: **the boot load
probes the wrong family.**

## 3. Candidates evaluated

| # | Candidate | Verdict |
| --- | --- | --- |
| 1 | Cache the verdict across loads keyed by face-set + size | **Rejected as stated** — the cache already keys on exactly that (face-set content fingerprint + body size, `reuseHintVerdicts()` :446). The boot verdict is for the wrong face set; no key change can bridge it. The *sound* form of this candidate is: make the boot load measure the reading family, and let the existing cache carry the verdict into every book open. **Chosen.** |
| 2 | Probe before the book arena/cache allocations (reorder the open path) | **Rejected.** The probe already runs at the load tail (prior design D1); the first session load happens at book open only because the boot load loaded the wrong family. Pre-loading inside `EpubReaderActivity::begin()` before the engine opens would run at `max_block` ≈ 63–65 KB — an ~8 KB margin the prior design's own risk table flags as possibly transient — and it duplicates work boot can already do for free. A new ordering constraint inside the reader for a problem boot already solves. |
| 3 | Allocate the probe task stack in PSRAM | **Rejected — recorded decision.** `2026-10-22-hint-probe-scheduling.md` §2 non-goals: "PSRAM task stacks (rejected: flash-cache-disabled windows, C3 has none)". Not renegotiated. |

## 4. Design: select the reader family at boot, before the first ensureLoaded

In `setupDisplayAndFonts()` (`src/main.cpp:824` block), after
`fontLoader.begin()` and **before** `ttfUiFallback.update()`, mirror the
reader's own selection:

```cpp
freeink::book::fontLoader.selectFamily(
    SETTINGS.readerFontEngine == CrossPointSettings::READER_ENGINE_TTF ? SETTINGS.ttfFontFamilyName : "");
```

Properties:

- The boot `ensureLoaded()` (via the UI-fallback sync) now loads the
  **settings-selected family** — the exact faces the reader opens — and the
  boot probe measures them at the whole boot heap (130 KB free at
  Stage=boot; the 57 KB spawn fits). The verdict is cached under the
  fingerprint the reader will compute.
- Book open: `selectFamily(same name)` takes the already-selected early
  return (`src/BookFontLoader.cpp:930`) → no dirty → `getReaderFont()`
  short-circuits → **no reload, no probe, no degrades**. The heap question
  disappears because the question is never asked.
- Settings changes mid-session keep today's behaviour: the settings screen's
  `begin()` + `selectFamily()` + preview `getReaderFont()` re-probes at the
  settings screen's heap (shallow, no book allocations), so the next reader
  entry reuses that verdict.
- Empty family name / bitmap engine: `selectFamily("")` is the explicit
  built-in-fallback selection — byte-for-byte the reader's own semantics at
  `TtfBookRuntime.cpp:501`. Boot stops using the legacy `families_[0]`
  debug default and agrees with what the reader will actually serve.
- Fixes a second latent wrongness in the same line: the UI fallback's script
  gate and `slotFaceBytes()` borrow (`src/TtfUiFallback.cpp:92-101`) now run
  against the family the reader renders. Before this change, an unselected
  boot load could register *families_[0]*'s faces under the settings family's
  font ids.
- FIBP identity improves: the layout generation hash derives from a
  fingerprint that no longer flips (families_[0] → user family) at the first
  book open — one less cache re-index per session.

Non-goals (unchanged from the prior design): measurement methodology, the
40 KB budget, DRAM font-byte fallback, PSRAM task stacks.

## 5. Risks

| Risk | Mitigation |
| --- | --- |
| Boot latency: 4 face loads + 4 probes instead of families_[0]'s 1 | The same SD reads happen at book open today; this trades book-open latency for boot latency. Prior design §4 accepted boot probing under a ~2 s budget; the soak verifies. Probes are ms-scale (bench doc). |
| Settings family missing/renamed on SD | `findFamily()` fails → built-in fallback (`ensureLoaded()` :867-870) — identical to the reader's own behaviour at open. |
| SD-error boot path (`main.cpp:987`) calls this block before `SETTINGS.loadFromFile()` | Defaults apply (engine TTF, empty family) → explicit fallback selection; nothing in that path reads the reader chain. No behaviour change. |
| Residual edge: builds/boards where the UI-fallback sync's engine gate exits early | No boot load happens (as today); the first reader open probes at the load tail with today's fail-closed outcome. Not worse than the soak baseline; revisit only if a soak shows it. |
| Host tests | No new host test: the change is a boot call-site ordering in `main.cpp`, which is not host-compiled. The loader semantics it relies on (unselected → families_[0], selected-family record without dirtying — issue #168, verdict reuse) are pinned by the existing `BookFontLoaderTestBackendFT` suites. Device soak is the verification. |

## 6. Verification

1. Gates: host ctest (stb + BackendFT incl. `HintingProbeRetryTest`),
   clang-format, cppcheck, `pio run -e x4pro` and `-e default`.
2. Owner soak (x4pro, same book): boot log shows one `Hint probe slot N
   consumed ...` line **per loaded face of the reading family**; book open
   shows **no** `Hint probe task spawn failed`, no
   `Hinting degraded to None` lines; hinted rendering confirmed visually.
3. Regression: settings → change font → back to reader still probes once at
   the settings screen and reuses the verdict on entry.
