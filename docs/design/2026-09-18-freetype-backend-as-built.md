# FreeType font backend — as-built design

**Status:** Authoritative. **Supersedes** [2026-09-17-freetype-font-backend.md](2026-09-17-freetype-font-backend.md) — that document's §3 decisions D1–D10 remain valid except where amended below, and its §6 task plan is fully executed. This document records the shipped architecture, including every course correction made during the on-device crash campaign.

- Firmware: XPoint PR #146 (`feat/freetype-font-backend` → `develop`)
- SDK: freeink-sdk PRs #26 (merged `6a8addc`), #27 (merged `5b52bfc1`), #28 (merged `7fb79b0`), #29 (merged `7987dfd`), **#30 reverted #29 (merged `9dcd6814`)**

## 1. Executive summary

The native-TTF reader path uses FreeType (`FtFont`) for outline faces behind `CROSSPOINT_FONT_BACKEND_FT`, replacing stb with a compatible API surface and SD-side `FIBP` section caches keyed by a backend-tagged fingerprint. The crash campaign that followed initial bring-up produced three durable findings that reshaped the design:

1. **The Adobe CFF engine is a stack hazard with no `FT_LOAD_NO_HINTING` bypass** — it interprets charstrings with unbounded, multi-KB stack-resident structures regardless of the hinting flag (on-device: `cf2_doStems → cf2_arrstack_push` blew a 16 KB render task with a Cache/MMU fault and garbage-pointer memcpy).
2. **The old "freetype" CFF engine is bounded but 3–10× slower per glyph** (quick-sheet overlays went from ~0.5 s to 1.7–5.8 s on-device) — tried and rejected.
3. **The correct containment is a single big-stack renderer**: all rendering runs on the Arduino loop task with a **48 KB stack**; the dedicated render task was deleted.

## 2. As-built architecture

### 2.1 Task model — one renderer

- `ActivityManagerRender` (16 KB pinned task) is **deleted**. `ActivityManager::performRender()` renders the current activity inline on the Arduino loop task.
- `SET_LOOP_TASK_STACK_SIZE(49152)` — 48 KB. Sizing rationale: EPub-InkPlate (turgu1) runs the Adobe engine *plus the TT bytecode interpreter* on a single 40 KB `mainTask` across arbitrary books — the empirical proof point; 48 KB adds ChapterLayout + paint margin. Net task-stack DRAM is unchanged vs. the previous two-task layout (32 KB loopTask + 16 KB render task).
- Rebuilds are UX-modal anyway (`ttfShowIndexingPopup()` / `GUI.drawPopup(tr(STR_INDEXING))`), so blocking the loop during multi-second renders is accepted. What the render task bought (input routing during rebuilds) was worth less than its stack cost and its cross-task FreeType questions.

### 2.2 Render request semantics

- `requestUpdate(false)` — deferred: flag drained at the end of the current `loop()` iteration, rendered inline.
- `requestUpdate(true)` — renders **synchronously when called from the main thread** (progress callbacks inside tight OTA/SD-flash loops block the loop task, so a deferred flag would never drain — the progress bar depends on it). Re-entrant calls (caller already inside a render or any `RenderLock` scope) defer: the mutex is not recursive. Calls from other tasks defer to the next `loop()` iteration.
- `requestUpdateAndWait()` — renders synchronously on the main thread; other tasks keep the waiter-notification mechanism. Caller must not hold a `RenderLock` (asserted).
- Flash-safety of synchronous progress-callback renders: the flash driver critical-sections its writes (cache suspension windows are descheduled), so display work between writes is safe — the same reasoning that made the old render-task progress bars safe.

### 2.3 FreeType configuration (freeink-sdk, as of `9dcd6814`)

| Setting | Value | Why |
|---|---|---|
| `FT_RENDER_POOL_SIZE` | 4096 (PR #27) | ftgrays worker lives on the caller's stack; 16 KB default burned ~18 KB per rasterize, host-measured 18 → 5.7 KB |
| Load flags | `FT_LOAD_NO_HINTING` on every load (PR #28) | Hinting is pointless for AA e-ink (stb is unhinted too); does NOT bypass the Adobe engine, but reduces its work |
| `CFF_CONFIG_OPTION_OLD_ENGINE` | **disabled** (PR #30 revert of #29) | The old engine is bounded but 3–10× slower per glyph; Adobe is chosen with a 48 KB single-task containment instead |
| `FtFont::glyphBounds` | override via `FT_Outline_Get_CBox`, 1 px outward pad + faux-bold embolden allowance (PR #26) | Guarantees `glyphBounds ⊇ rasterize()` bitmap; metrics miss the `FT_Set_Transform` shear; `preserveGlyphBitmap()` keeps the rasterize bitmap valid across intervening loads |
| CFF + psaux modules | compiled in (wrappers `ft_ftcff.c`, `ft_ftpsaux.c`) | CFF (.otf) faces must render — stb parity requirement |

### 2.4 Firmware surface (unchanged from the superseded doc, confirmed)

- `BookFontLoader`: `NativeFace` = `FtFont` under the flag; no caller glyph arena (D6); `kInitSizePx = 14`; fingerprint XOR tag `0x46545531` (D4); faux bold/italic via transform + embolden.
- Device-class split: the FT flag is enabled on every PSRAM-class device profile — `x4pro`, `x4pro_profile`, the four `x4c` variants, and the three `papermono` variants; C3-class builds (`default`, `sticky`) dead-strip the amalgam entirely (0 `FtFont` symbols verified in the `default` env).
- Host test suites build in stb + `BackendFT` variants; `FontBackendInvariantsTest` covers positive advances, `glyphBounds ⊇ rasterize` containment, malformed rejection, and variable-font selection per backend.

## 3. Decision log — amendments to D1–D10

| # | Decision | Replaces |
|---|---|---|
| A1 | **Adobe CFF engine everywhere**, contained by the single 48 KB main-thread renderer. Old "freetype" CFF engine rejected on measured per-glyph speed. | D-none (new; PRs #29→#30) |
| A2 | **All rendering on the loop task; render task deleted.** Replaces the two-task render architecture assumed by the superseded doc's §7. Rebuild-blocking is accepted (UX already modal). | (new; supersedes the old §7 mitigation row) |
| A3 | **48 KB loopTask** (`SET_LOOP_TASK_STACK_SIZE(49152)`). 40 KB = EPub-InkPlate proof point, +20% margin. If a pathological face ever exceeds it, per-render high-water telemetry names it and 56 KB is a one-line change. | (new) |
| A4 | `requestUpdate(true)`/`requestUpdateAndWait()` render synchronously from the main thread. | (new) |

All other D1–D10 decisions stand as written in the superseded document.

## 4. Verification gates (as run)

- `pio run -e x4pro` and `-e default`: PASS (0 `FtFont` symbols in `default`; DRAM delta +24 bytes vs. stb baseline — D8 threshold <4 KB).
- Host ctest: 581/581 including both backend variants of every converted suite; SDK host suite 679 checks / 0 failures at PR #28, re-verified at #29/#30.
- `./bin/clang-format-fix` idempotent; `bin/cppcheck-check` PASS.
- On-device soak (x4pro): all former crash sites re-tested clean on the final build — book open with FIBP cache miss, quick-sheet font size ±, TextSettings font preview (CFF face), page turns, idle.

## 5. Monitoring / open follow-ups

- Per-render stack high-water + loop-task census (`SENT` logs, `CROSSPOINT_MEM_SENTINEL` builds only) name any path that outgrows the 48 KB budget.
- IDLE0 high-water dipped to 244 bytes once mid-session after the first quick-font render (persistent for the session, no canary, no crash). Not reproduced as a failure; watch it in the device soak. If it recurs, `CONFIG_FREERTOS_IDLE_TASK_STACKSIZE` is the knob.
- Render wall time vs. the stb build is a device-soak item (superseded doc §7 row on the missing glyph raster cache still applies; the per-face LRU cache remains the designated follow-up if a material regression shows up).

## 6. References

- Superseded design: [2026-09-17-freetype-font-backend.md](2026-09-17-freetype-font-backend.md) (D1–D10 decision rationale)
- freeink-sdk PRs: [#26](https://github.com/Belphemur/freeink-sdk/pull/26) glyphBounds, [#27](https://github.com/Belphemur/freeink-sdk/pull/27) render pool 4 KB, [#28](https://github.com/Belphemur/freeink-sdk/pull/28) unhinted loads, [#29](https://github.com/Belphemur/freeink-sdk/pull/29) old CFF engine (reverted), [#30](https://github.com/Belphemur/freeink-sdk/pull/30) revert
- XPoint PR #146 — firmware switch + main-thread renderer
- External precedent: [EPub-InkPlate](https://github.com/turgu1/EPub-InkPlate) — Adobe engine + bytecode interpreter on a single 40 KB `mainTask` (ESP32, Xtensa). Note on PSRAM stacks: IDF ≥5.x CAN place task stacks in external RAM on the S3 (`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` / `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` + `xTaskCreateStatic()` with an externally allocated buffer), but Espressif advises against it and it is unsafe here: external RAM is inaccessible whenever the flash cache is disabled (every OTA/flash write), which this firmware performs with live rendering. CrossPoint therefore keeps all task stacks in internal DRAM — a deliberate choice, not a platform limitation.

## 7. Addendum (index-ahead prefetch worker, 2026-09-18)

The §2 architecture (one renderer on the loop task, synchronous `requestUpdate`) is unchanged. `FibpPrefetchWorker` adds a **background chapter-index (FIBP) prefetch task** for S3-class builds (PSRAM + FT): 32 KB DRAM stack, priority 1, pinned to core 0, transient per book (created on the first TTF render with a known generation, cancelled in the reader's `onExit()`; with the capped one-spine prefetch window it self-exits once that window drains and is respawned on the next chapter entry or settings change). Inert stub on C3 / stb rollback.

- **One indexing implementation**: the build driver lives in `ChapterIndexEngine` (`ensureChapterSession` / `pumpChapterChunk` / `runChapterBuild` with a yield hook) over a narrow `ChapterIndexTarget` interface that `TtfBookRuntime` implements; the synchronous rebuild, background build tick, next-chapter prefetch, and the worker all pump through it.
- **FT threading**: face create/destroy stays on the main thread (worker `begin`/`cancel`, loader `ensureLoaded`) — FreeType only serializes face lifetime, which one thread satisfies without a lock; the worker thread uses only the per-face APIs documented thread-safe. The worker's faces are its own `FtFont` instances over its own PSRAM byte copies of the same font files; its fingerprint replicates `computeFingerprint()` byte-for-byte (same slot order, coverage folded **before** the fallback tail — matching `ensureLoaded()`'s ordering — plus the `0x46545531` backend tag), so worker-built FIBP files carry exactly the generation names the reader resolves.
- **Prefetch trigger (owner directive, 2026-09-19)**: the plan is capped to the NEXT chapter only — `fibp::kPrefetchLookaheadSpines = 1`, cut relative to the notified spine via `buildQueue`'s existing cap parameter (the pure function stays testable; no call-site special case) — and is enqueued only by the PROGRESS trigger `fibp::shouldPrefetchNext(page, pageCount)`: the next chapter is queued once the reader is inside the last `ceil(pageCount · 10 / 100)` pages of the CURRENT chapter (`kPrefetchRemainingPercent = 10`; `pageCount == 0` never triggers; the ceil keeps the 1-page-remaining edge firing on tiny chapters). The reader reports position via `notifyChapterProgress(spine, page, pageCount)` on chapter entry AND every page turn — the worker applies the policy and dedups through the run loop's change detection (page turns inside the same spine never rebuild the queue). Replacing the old entry-time trigger avoids building chapters the user may never open, and a chapter entered already past the threshold fires immediately (resume/short chapters). Whole-book prefetching had caused the device-soak symptoms (SD-write pressure, the task-WDT abort at spine 65 of a 177-spine run). The window replans via `ensureTask()` (the task self-exits after each window drains; the loop head treats a fully-drained inherited queue as empty so a respawned worker replans) — with the window exhausted the worker exits instead of spinning: the run loop's stop conditions (`break` when every planned spine has a cache or was attempted) cover the empty window.
- **Single-writer**: the worker skips spines whose cache file exists (complete *or* partial — never renames over a reader-held cache), skips the entered chapter (the reader's sync path owns it, with a handoff: the reader defers its own rebuild while the worker claims that spine, then reopens the cache on the worker's commit), and the legacy in-activity next-chapter prefetch is disabled whenever the worker feature is begun (not merely while a task runs). **Takeover escape hatch (device-soak fix, 2026-09-19)**: with the one-spine cap the plan starts AFTER the notified spine, so the worker may already be building the next chapter when the reader turns into it — a claim holding at that moment would park the user on the Indexing popup for the whole build, so after a 1.5 s grace the reader stops the worker (which aborts at the next page boundary), resumes the worker's partial, and builds only up to the target page at full speed; `updateFibpWorker()` respawns the worker once the session drains and it skips the now-existing cache.
- **DRAM budget**: worker stack 32 KB DRAM; all TTF machinery stays PSRAM-only. Free-heap floor while indexing ≈ 51 KB (82.7 KB measured HeapMin − 32 KB transient stack), still above the reader's 32 KB/16 KB background-build floors, which the worker itself mirrors as a wait gate.
- **Power governor interaction (device-soak fix, 2026-09-19)**: background builds are real work — the input-idle governor now (a) suppresses low-power re-entry for a 2 s dwell after any normal-speed request (`HalPowerManager::NORMAL_POWER_DWELL_MS`), and (b) `EpubReaderActivity::preventAutoSleep()` reports worker/session/section builds so the CPU stays at full speed while they drain. Device evidence for both: spine builds measured 2.6 s/page at LOW_POWER_FREQ (93 pages / 243 s), repeated enter/restore frequency flip-flops per second while a build polled with renders, and a task-WDT abort (IDLE0 starved >WDT window by `fibpprefetch` at low frequency). While a build is delegated to the worker, the reader re-polls at a 250 ms cadence instead of rendering every loop tick.
- **Cache-clear interlock (device-soak fix, 2026-09-19)**: the reader's DELETE_CACHE action stops the worker and closes the TTF runtime BEFORE `clearCache()` — open FIBP/section handles make the recursive `removeDir` fail and leave the whole `epub_<hash>` folder (ficache included) on SD. `clearBookCache()` additionally removes a surviving `<book cache>/ficache` explicitly and logs the outcome.
- **Watch in the device soak**: `[PREF] indexed <href> pages=… ms=… hwm=…` per indexed spine (HWM must stay ≥ 8 KB free of the 32 KB stack — a 93-page image-heavy spine measured HWM 1792 B at 24 KB, hence the raise), HeapMin during indexing, SD-write contention during rapid page turns, settings change mid-index (generation re-seed), battery impact.

## 8. Addendum (freeink-sdk 43fed43 integration, 2026-09-19)

The SDK pin moved `2dbd5a8 → 43fed43` (upstream FtFont work #111/#112: opt-in hinting options `e8eba49`, reusable scalable font APIs `a4f1db2`, keyboard spacing `ab33145`). Firmware-side integration:

- **Hinting requested, gate-enforced, and identity-folded (supersedes the earlier "hinting stays OFF" wording of this addendum, which described the `43fed43` pin only).** Faces call `setRenderOptions(BookFontLoader::kRenderOptions)` (single source of truth) at both `FtFont` creation sites — `tryLoadFace()` and the prefetch worker — and the requested `HintingMode::Light` is a *request*, not a promise: a per-face stack probe (`ensureHintProbeSettled()` at the reader's first TTF render, measured on a dedicated 32 KB task so the reading is independent of the calling task's stack history) degrades any face whose hinted render depth would not fit the 32 KB `FibpPrefetchWorker`, and the effective per-slot modes fold into `BookFontLoader::renderOptionsFingerprintTag()` in BOTH parity sites, so a degrade regenerates the FIBP cache through the existing generation mechanism. The SDK's `Default` mode remains deliberately NOT adopted: it keeps FreeType's phantom-point advance rounding and would change pagination. See `docs/design/2026-09-29-hint-probe-measurement.md`.
- **Cache identity**: the active hinting mode is folded into the font fingerprint via `BookFontLoader::renderOptionsFingerprintTag()` in BOTH parity sites (`computeFingerprint()` and the worker hash), so any future render-affecting option change regenerates FIBP caches through the existing generation mechanism. The fold ships together with the SDK's own `kLayoutRevision` 12→13 bump (GSUB-based `FtFont::ligature()` changes layout output), so this release regenerates caches exactly once.
- **New SDK surface NOT adopted, with reasons**: `inspectMemory`/`inspectStream` (firmware style detection is filename-token based by design and `validateSfntBytes` stays as the cheap pre-parse guard), the 26.6 metrics APIs (firmware speaks only the integer `Font` interface through FreeInkBook — no duplicated fractional math exists), `configureMemory` (FT memory is already routed through the SDK's PSRAM-first `FontAlloc`), and the keyboard/UI fixes (no reader overlap).
- **Validation gates recorded for this pin (as run 2026-09-19)**: host `ctest` 597/597 passed (incl. all `BackendFT` font-backend variants after adding `Gsub.cpp`), bare `./bin/clang-format-fix` clean, `bin/cppcheck-check` PASSED, `pio run -e default` and `-e x4pro` SUCCESS; CI on PR #153 all 10 jobs green (Build default/sticky/x4c/x4pro/papermono, Host unit tests, clang-format, cppcheck, CI Status, Title Check).
- **Gates at pin `43fed43`** (verified at PR #153 head `4d7a9f40`): host ctest 597/597 (test/CMakeLists.txt gained the new SDK source `Gsub.cpp`), `cppcheck` PASS, `clang-format` clean, `pio run -e default` and `-e x4pro` SUCCESS, and the full PR CI matrix (Build default/sticky/x4pro/x4c/papermono, cppcheck, clang-format, host unit tests) green. `.dram0.bss` delta vs. develop: 0 bytes. On-device soak is the owner's pre-merge step (PR #153 checklist).
## 9. Addendum (advance fast path + metrics memo, 2026-09-27)

Chapter-index (FIBP) builds were dominated by `FtFont::advance()`: it loaded the glyph outline — on CFF running the Adobe charstring interpreter — for every measurement, and layout measures every codepoint several times per paragraph (measure walk, per-line `recordLine` re-measure, placement walk). freeink-sdk PR [#34](https://github.com/Belphemur/freeink-sdk/pull/34) (`perf/ft-advance-metrics`, squash-merged at `7004582a`):

- **Fast path**: `FT_Get_Advance(FAST_ONLY | FT_LOAD_NO_HINTING)` reads the advance from the metrics table without decoding the outline (~56x cheaper per call on CFF; measured 2.21 µs → 0.04 µs host). Falls back to the unchanged full metrics load when a driver cannot answer from metrics alone.
- **Bit-identical output — no `kLayoutRevision` bump.** `FT_Get_Advance` returns the same 16.16 value `linearHoriAdvance` does; the same `fixed16_16To26_6` rounding is reused. Asserted by the new `FtFontAdvanceParityTest` (7 sizes × Latin/punct/ligature/missing codepoints, TTF + CFF) plus an independent ~63k-pair sweep on three faces with zero mismatches and zero FAST_ONLY failures. Because pagination is unchanged, FIBP caches stay valid — deliberately NOT folded into the fingerprint.
- **Metrics memo**: per-face direct-mapped tables for `advance()` `(codepoint, size)` and `kerning()` `(left, right, size)`, 512 slots each (`fontAlloc`-lazied, ~14 KB PSRAM per face worst case), flushed inside `flushGlyphCache()` so `setRenderOptions()`/re-init cannot serve a stale value. Collision = replace; a book with more distinct codepoints just misses more. OOM degrades to never-cached.
- **Measured (host, real engine, 25-page chapter)**: CFF/OTF reader font 125.8 ms → 5.1 ms (**24.6x**), TrueType face 41.5 ms → 6.0 ms (**6.9x**). Device confirmation is the existing `[PREF] indexed <href> pages=… ms=… hwm=…` log — compare per-spine `ms=` before/after the pin bump; this index-throughput gain is the point of the change, page-turn rasterization was already cached (P1 glyph bitmap cache).
- **Benchmark harness (firmware-side)**: `test/fibp_bench/FibpLayoutBench` builds the real ChapterLayout + FtFont against the committed `font-prewarm-benchmark.epub` and Amazon Ember/Atkinson fixtures; opt-in (`-DCROSSPOINT_BUILD_BENCH=ON`, not part of the ctest gate). `FIBP_BENCH_CACHE=1` wraps the font in a metrics memo to show the remaining headroom. Rerun before/after any future font-metric change.

### 9.1 EPub-InkPlate typography comparison (evaluated, not ported)

Evaluated against EPub-InkPlate's "Advanced Typography" README claim (adjusted fonts + FreeType kerning + a lightweight custom ligature algorithm), as a possible engine improvement source alongside its page_locs indexer:

- **Their ligatures are a hardcoded 22-entry adjacent-pair table** (`components/fonts/src/font.hpp`): `ff/fi/fl/ffi/ffl` plus `ae/oe/ij/AE/OE/IJ` plus punctuation (`--`→–, `..`→‥, `<<`→«, `''`→", …), applied unconditionally per adjacent pair — the font's OpenType `liga` feature is never consulted.
- **Actively wrong for ordinary text**: their own shipped fonts (Roboto/RedHat/DejaVu, cmap-verified) all contain æ/œ/ĳ, so their table rewrites `aesthetic`→`æsthetic`, `coefficient`→`cœfficient`, `poem`→`pœm`. The punctuation half is redundant for EPUB (XHTML ships real Unicode) and corrupts code/ranges/dialogue. Not ported.
- **Their kerning is legacy-table only** (`FT_Get_Kerning`). Ours is already a superset: legacy `kern` + GPOS pair-kerning fallback (`Gpos.cpp`), and our ligatures resolve through the font's GSUB (`Gsub.cpp`) — context- and language-correct where their table guesses.
- **The one real gap worth a follow-up** (not this change): our GSUB path can only return a substitution as a Unicode codepoint, so a font whose `fi` ligature glyph has no cmap entry (the common case in professionally authored fonts) stays unligated. Extending resolution to fall back to a hardcoded set **for the f-pairs only** is the safe version of what their table attempts.

So the import from EPub-InkPlate that mattered was architectural (their measured 40 KB single-task renderer precedent, already recorded in §6), not their typography layer.

