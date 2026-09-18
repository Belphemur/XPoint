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
- Device-class split: FT path is x4pro/x4pro-profile-class only (S3, 8 MB PSRAM); C3 stb builds dead-strip the amalgam entirely (0 `FtFont` symbols verified in the `default` env).
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
- External precedent: [EPub-InkPlate](https://github.com/turgu1/EPub-InkPlate) — Adobe engine + bytecode interpreter on a single 40 KB `mainTask` (ESP32, Xtensa). Note: task stacks **cannot** live in PSRAM on Xtensa (window-spill/exception path requires internal RAM; IDF supports external-RAM stacks on RISC-V only) — the stack budget must be paid in DRAM.
