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

The §2 architecture (one renderer on the loop task, synchronous `requestUpdate`) is unchanged. `FibpPrefetchWorker` adds a **background chapter-index (FIBP) prefetch task** for S3-class builds (PSRAM + FT): 32 KB DRAM stack, priority 1, pinned to core 0, transient per book (created on the first TTF render with a known generation, cancelled in the reader's `onExit()`; it self-exits once every spine has been attempted so the stack is released while reading). Inert stub on C3 / stb rollback.

- **One indexing implementation**: the build driver lives in `ChapterIndexEngine` (`ensureChapterSession` / `pumpChapterChunk` / `runChapterBuild` with a yield hook) over a narrow `ChapterIndexTarget` interface that `TtfBookRuntime` implements; the synchronous rebuild, background build tick, next-chapter prefetch, and the worker all pump through it.
- **FT threading**: face create/destroy stays on the main thread (worker `begin`/`cancel`, loader `ensureLoaded`) — FreeType only serializes face lifetime, which one thread satisfies without a lock; the worker thread uses only the per-face APIs documented thread-safe. The worker's faces are its own `FtFont` instances over its own PSRAM byte copies of the same font files; its fingerprint replicates `computeFingerprint()` byte-for-byte (same slot order, coverage folded **before** the fallback tail — matching `ensureLoaded()`'s ordering — plus the `0x46545531` backend tag), so worker-built FIBP files carry exactly the generation names the reader resolves.
- **Single-writer**: the worker skips spines whose cache file exists (complete *or* partial — never renames over a reader-held cache), skips the entered chapter (the reader's sync path owns it, with a handoff: the reader defers its own rebuild while the worker claims that spine, then reopens the cache on the worker's commit), and the legacy in-activity next-chapter prefetch is disabled while the worker runs. **Takeover escape hatch (device-soak fix, 2026-09-19)**: a claim that lands right before the user turns into the spine would park them on the Indexing popup for the whole build (the plan builds the entered chapter last), so after a 1.5 s grace the reader stops the worker (which aborts at the next page boundary), resumes the worker's partial, and builds only up to the target page at full speed; `updateFibpWorker()` respawns the worker once the session drains and it skips the now-existing cache.
- **DRAM budget**: worker stack 32 KB DRAM; all TTF machinery stays PSRAM-only. Free-heap floor while indexing ≈ 51 KB (82.7 KB measured HeapMin − 32 KB transient stack), still above the reader's 32 KB/16 KB background-build floors, which the worker itself mirrors as a wait gate.
- **Power governor interaction (device-soak fix, 2026-09-19)**: background builds are real work — the input-idle governor now (a) suppresses low-power re-entry for a 2 s dwell after any normal-speed request (`HalPowerManager::NORMAL_POWER_DWELL_MS`), and (b) `EpubReaderActivity::preventAutoSleep()` reports worker/session/section builds so the CPU stays at full speed while they drain. Device evidence for both: spine builds measured 2.6 s/page at LOW_POWER_FREQ (93 pages / 243 s), repeated enter/restore frequency flip-flops per second while a build polled with renders, and a task-WDT abort (IDLE0 starved >WDT window by `fibpprefetch` at low frequency). While a build is delegated to the worker, the reader re-polls at a 250 ms cadence instead of rendering every loop tick.
- **Cache-clear interlock (device-soak fix, 2026-09-19)**: the reader's DELETE_CACHE action stops the worker and closes the TTF runtime BEFORE `clearCache()` — open FIBP/section handles make the recursive `removeDir` fail and leave the whole `epub_<hash>` folder (ficache included) on SD. `clearBookCache()` additionally removes a surviving `<book cache>/ficache` explicitly and logs the outcome.
- **Watch in the device soak**: `[PREF] indexed <href> pages=… ms=… hwm=…` per indexed spine (HWM must stay ≥ 8 KB free of the 32 KB stack — a 93-page image-heavy spine measured HWM 1792 B at 24 KB, hence the raise), HeapMin during indexing, SD-write contention during rapid page turns, settings change mid-index (generation re-seed), battery impact.

## 8. Addendum (freeink-sdk 43fed43 integration, 2026-09-19)

The SDK pin moved `2dbd5a8 → 43fed43` (upstream FtFont work #111/#112: opt-in hinting options `e8eba49`, reusable scalable font APIs `a4f1db2`, keyboard spacing `ab33145`). Firmware-side integration:

- **Hinting stays OFF, now expressed through the SDK's own mechanism.** Faces call `setRenderOptions(BookFontLoader::kRenderOptions)` (single source of truth; `RenderOptions{}` selects `HintingMode::None`) at both `FtFont` creation sites — `tryLoadFace()` and the prefetch worker — which produces the same `FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT` loads this architecture shipped with. The SDK's `Default` mode was deliberately NOT adopted: it keeps FreeType's phantom-point advance rounding and would change pagination.
- **Cache identity**: the active hinting mode is folded into the font fingerprint via `BookFontLoader::renderOptionsFingerprintTag()` in BOTH parity sites (`computeFingerprint()` and the worker hash), so any future render-affecting option change regenerates FIBP caches through the existing generation mechanism. The fold ships together with the SDK's own `kLayoutRevision` 12→13 bump (GSUB-based `FtFont::ligature()` changes layout output), so this release regenerates caches exactly once.
- **New SDK surface NOT adopted, with reasons**: `inspectMemory`/`inspectStream` (firmware style detection is filename-token based by design and `validateSfntBytes` stays as the cheap pre-parse guard), the 26.6 metrics APIs (firmware speaks only the integer `Font` interface through FreeInkBook — no duplicated fractional math exists), `configureMemory` (FT memory is already routed through the SDK's PSRAM-first `FontAlloc`), and the keyboard/UI fixes (no reader overlap).
- **Validation gates recorded for this pin (as run 2026-09-19)**: host `ctest` 597/597 passed (incl. all `BackendFT` font-backend variants after adding `Gsub.cpp`), bare `./bin/clang-format-fix` clean, `bin/cppcheck-check` PASSED, `pio run -e default` and `-e x4pro` SUCCESS; CI on PR #153 all 10 jobs green (Build default/sticky/x4c/x4pro/papermono, Host unit tests, clang-format, cppcheck, CI Status, Title Check).
- **Gates at pin `43fed43`** (verified at PR #153 head `4d7a9f40`): host ctest 597/597 (test/CMakeLists.txt gained the new SDK source `Gsub.cpp`), `cppcheck` PASS, `clang-format` clean, `pio run -e default` and `-e x4pro` SUCCESS, and the full PR CI matrix (Build default/sticky/x4pro/x4c/papermono, cppcheck, clang-format, host unit tests) green. `.dram0.bss` delta vs. develop: 0 bytes. On-device soak is the owner's pre-merge step (PR #153 checklist).
