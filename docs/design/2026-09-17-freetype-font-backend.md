# FreeType font backend for the native-TTF reader

**Date:** 2026-09-17
**Status:** Approved for implementation
**Branch:** `feat/freetype-font-backend` (firmware), SDK PR per §6 Task 1
**Related:** `references/ttf-native-fonts-directives.md` (standing directives),
`references/sdk-fork-pr-workflow.md`, SDK PR #25 (FreeInkFont split, pin 660dc91)

## 1. Executive summary

The freeink-sdk fork (at pin `660dc91`) ships a second font backend in the
standalone `FreeInkFont` library: `freeink::font::FtFont`, a FreeType-backed
implementation of the same `RasterFont` interface the reader already consumes
(`TtfFont`, stb_truetype-backed). Both backends implement the identical vtable
(`advance`/`lineHeight`/`ascent`/`kerning`/`hasGlyph`/`rasterize`), and the
firmware already compiles the FreeType amalgam today (the linker GCs it only
because nothing references it).

Switching the native-TTF reader from stb to FreeType buys:

- **True bold from variable fonts** — the `wght` axis instead of outline
  emboldening; real `ital`/`slnt` axes with synthesized oblique fallback.
- **The streaming path** for multi-MB CJK faces (`initStream`) — Phase 2,
  not this PR.
- FreeType allocations already route to PSRAM via the SDK's custom
  `FT_Memory` (`FontAlloc`), matching the PSRAM-only directive.

The work is an adapter + selection change, not a port: one firmware PR plus
one small SDK PR (`FtFont::glyphBounds`). Estimated firmware-side delta:
~200–400 lines.

## 2. Current chain (facts this design builds on)

| Fact | Location |
|---|---|
| `BookFontLoader::tryLoadFace` does sfnt table-directory validation, loads the face file into PSRAM (`poolMakeBytes`, guard `kMaxFaceBytes = 2 MiB`), builds a per-face glyph `Arena` (`kGlyphArenaBytes`), then `TtfFont::init(data, len, arena)` and `FontChain::add(face, styleFlags)` | `src/BookFontLoader.cpp:719+` |
| `fontFingerprint()` = FNV-1a over the loaded font bytes XOR styleCoverage; feeds `layoutGenerationHash()` → the section cache (`section.bin`) invalidates on change. Fallback chain → fingerprint 0 | `src/BookFontLoader.cpp:295`, `src/BookFontLoader.h:64` |
| `PagePaint::walkText` culls per glyph via `RasterFont::glyphBounds` and immediately consumes `rasterize()` output (row-by-row draw inside the same iteration) — glyph-lifetime contract "valid until next rasterize on the same face" is already respected | `src/adapters/PagePaint.cpp:50–100` |
| `FtFont` is **size-agnostic in practice**: `ensureSize()` re-runs `FT_Set_Pixel_Sizes` only when `sizePx` changes, so one instance serves body + ruby sizes. The header comment "one instance = one pixel size" is stale; the real binding is (weight, italic) applied via variable-font axes at init | `freeink-sdk/libs/font/FreeInkFont/src/FtFont.cpp:167–171` |
| `FtFont` does **not** override `glyphBounds`; `Font.h`'s default returns `false` → `PagePaint`'s cull falls through to rasterize-then-filter (correct, slower on banded gray pages) | `freeink-sdk/libs/font/FreeInkFont/include/Font.h:100–109` |
| `FtFont` needs **no caller glyph arena** — FreeType owns the glyph slot; all FT heap goes through `FontAlloc` (PSRAM when present) | `FtFont.h` header comment, `FontAlloc.h` |
| The FreeType amalgam (11 modules) already compiles in every firmware build; it is dead-stripped at link because nothing references `FtFont` | `FreeInkFont/library.json` build flags; verified 2026-09-17: no `FtFont` symbols in the x4pro `.elf` |
| Reader sources include the SDK only through FreeInkBook's re-export shim (`render/TtfFont.h` → `freeink::book::TtfFont`/`FontChain`) | `freeink-sdk/libs/book/FreeInkBook/include/render/TtfFont.h` |

## 3. Decisions (decision log — append-only)

| # | Decision | Rationale |
|---|---|---|
| D1 | **Full backend replacement** on the TTF device class, compile-time. No runtime toggle, no stb/FT hybrid. | KISS: both backends satisfy the same `RasterFont` contract; a hybrid doubles the parity/test matrix for no user value; the only capability stb lacks (streaming) is also FT-only. stb `TtfFont` stays in the SDK (rollback = flip one constant). |
| D2 | Backend selection = `CROSSPOINT_FONT_BACKEND_FT` compile flag in the same device-class gate as `CROSSPOINT_TTF_READER` (PSRAM builds: x4pro/x4c/papermono). Default `1`. Under the flag `BookFontLoader::tryLoadFace` constructs `FtFont` instead of `TtfFont`; the stb path remains compilable. | Mirrors the compile-time device-class split directive; C3/legacy class never sees FreeType code in its link. |
| D3 | **SDK fork PR #1:** add `FtFont::glyphBounds` (override returning real ink bounds from `FT_Load_Char(FT_LOAD_DEFAULT)` + `face->glyph->metrics`, no render) with host tests, merged to `Belphemur/freeink-sdk` main before the firmware PR's final pin. | Restores `PagePaint`'s band-cull parity (~30 lines). The fork owns the SDK; engine gaps go through `sdk-fork-pr-workflow`. |
| D4 | `computeFingerprint()` mixes in a backend tag (`FNV-1a ^ 0x46545531` under the FT flag) on top of the existing byte-hash. `SECTION_FILE_VERSION` unchanged. | Backend switch can change advances/kerning (different hinting) → a stale section cache would render FT layout over stb metrics. Folding the tag into the fingerprint invalidates only TTF-family caches (bitmap fallback chains keep fingerprint 0 and stay cached). Format unchanged → no version bump. |
| D5 | **Streaming (`initStream` via a `HalFile` ReadFn) is out of scope** — follow-up design doc after Phase 1 ships on-device. Phase 1 keeps the ≤2 MiB PSRAM-borrow guard and the current byte-loading path unchanged. | Smallest-first parity gate: mutex-per-SD-read latency under `storageMutex` is unmeasured; borrowing bytes keeps `tryLoadFace`'s validation and lifetime story identical to today. |
| D6 | Under the FT flag, `tryLoadFace` skips the per-face glyph `Arena` + `glyphBacking_` pool allocation entirely (FT owns glyph memory). The stb path keeps them. | Direct PSRAM budget win per face; less state. Arena plumbing stays for the stb path. |
| D7 | Phase 1 keeps the 4-file family manifest model (REGULAR/BOLD/ITALIC/BOLD_ITALIC as separate files). Single-variable-file families (one file → 4 styles via axes) are a follow-up. | Requires manifest + settings UI changes; orthogonal to the backend switch. |
| D8 | **DRAM gate:** `.dram0.bss` (and `.data`) size diff vs the pre-change x4pro build must be ≈ 0 (threshold: < 4 KB) for the FT link-in. FT's own allocations already go to PSRAM via `FontAlloc`; the zero-DRAM-statics directive applies to everything we add firmware-side. | Standing directive (measured ~148 KB boot-DRAM loss when violated during the TTF migration). |
| D9 | Host tests: run the existing font corpus through **both** backends where the fixture set is backend-agnostic. No cross-backend metric equality assertions — stb and FreeType legitimately differ by ±1 px (hinting); each backend is asserted against its own invariants. | Cross-backend equality tests would be flaky and assert an implementation detail. |
| D10 | The loader-side sfnt validation boundary stays exactly as is (FT adds a second validation layer underneath, not a replacement). | The corpus includes 2 malformed fonts pinning that boundary; FT's own acceptance is looser in places. |

## 4. Firmware-side changes (all under `CROSSPOINT_FONT_BACKEND_FT`)

1. **`src/BookFontLoader.h/.cpp`**
   - `tryLoadFace`: construct `FtFont` (via the FreeInkBook re-export shim or
     `freeink::font::FtFont` directly — use the direct include
     `<FtFont.h>`; the shim re-exports only TtfFont/FontChain) and
     `init(data, len, kInitSizePx, weight = styleToWeight(styleFlags),
     italic = styleFlags & StyleItalic)` where `kInitSizePx` is the existing
     default body size constant the loader already uses; per-run sizes (ruby,
     preview) adapt at runtime through `FtFont::ensureSize`.
     Skip the `glyphBacking_`/`Arena` block (D6).
   - Style → axis mapping helper: `StyleBold → weight 700`, else 400;
     `StyleItalic → true`. Faux-oblique/embolden fallbacks are FT-side, free.
   - `computeFingerprint()`: XOR the backend tag (D4).
   - Teardown/deinit paths: `FtFont`'s destructor releases the face; borrowed
     bytes lifetime rules unchanged (still borrowed, still outlive the face).
2. **`platformio.ini`** — no new deps (FreeInkFont symlink already registered
   on `develop` at 257ca929). Add `-DCROSSPOINT_FONT_BACKEND_FT=1` to the TTF
   device-class envs, hoisted next to the existing `CROSSPOINT_TTF_READER`
   definition (verify per-env resolution with `pio project config`).
3. **`src/adapters/PagePaint.cpp`** — no changes (interface identical).
4. **Host tests** (`test/book_font_loader`, `test/page_paint`,
   `test/ttf_word_select`, plus the SDK-side test in Task 1):
   - Build both backend variants of the loader suites (a second test target
     per suite compiled with `-DCROSSPOINT_FONT_BACKEND_FT=1` against
     `FtFont.cpp` + the FT amalgam; host glibc is fine).
   - Assert per-backend invariants: positive advances for the DejaVu/Ember/
     Atkinson fixtures; `glyphBounds` box ⊇ rasterized bitmap box; malformed
     corpus fonts rejected (D10); variable-font fixture (add one small VF
     fixture if the corpus lacks it) reports true-bold advances ≠ regular.

## 5. Verification gates (all mandatory before push)

1. `pio run -e x4pro` and `pio run -e default` (the non-TTF class must build
   byte-identical in behavior: flag off, FT still dead-stripped — verify no
   `FtFont` symbols in the default `.elf`).
2. `.dram0.bss` + `.data` diff vs pre-change x4pro build (D8 threshold).
3. Full host ctest (both backend variants).
4. `./bin/clang-format-fix` (whole tree) + `git diff --exit-code`;
   `pio check` with CI's exact invocation (`bin/cppcheck-check`).
5. Device soak per `references/ttf-device-soak-checklist.md` — **human
   tester scope** (Antoine), not pi: flag in the PR body.
6. On-device parity: same book, same family, before/after screenshots
   compared for ruby size, bold weight, italic slant (human scope).

## 6. Task plan (ordered, each = its own commit tagged `[taskN]`)

- **[task1] SDK PR — `FtFont::glyphBounds`** (branch `feat/ftfont-glyph-bounds`
  in the submodule, pushed to `Belphemur/freeink-sdk`):
  override + host tests + SDK host gate (`libs/font/FreeInkFont/test/host`
  run script), PR against `Belphemur/freeink-sdk` `main`, review loop to
  convergence (per the pi-orchestrator review-loop section), squash-merge.
- **[task2] Firmware backend switch** on `feat/freetype-font-backend`:
  §4 items 1–2, host tests (§4.4), gates §5.1–5.4. Pin the submodule to the
  merged Task-1 commit on `main`.
- **[task3] PR + review loop:** push the branch, open the PR against
  `Belphemur/XPoint` `develop` (draft until Task 2 lands), run the
  answer-code-review loop to convergence (CI green, zero unresolved
  threads, re-review clean on the final commit).
- **[task4] Docs:** CHANGELOG entry + a pointer row in
  `references/ttf-native-fonts-directives.md`'s engine notes if the skill
  references font-backend assumptions (doc-lockstep rule).

Out of scope (file cards, do not implement): streamed CJK faces (D5),
single-variable-file families (D7), FT hinting-mode tuning beyond defaults.

## 7. Risks

| Risk | Mitigation |
|---|---|
| FT adds DRAM statics once linked | D8 gate; `FontAlloc` PSRAM routing is already in the SDK; escalate if threshold exceeded |
| Metrics differ stb→FT → all TTF caches invalidate once | Expected + intended (D4); bitmap-family caches untouched |
| Build time grows (amalgam no longer dead-stripped on x4pro) | Accepted; C3 unaffected (dead-stripped) |
| FT face init slower than stb per face (font-load path) | Bounded by 4 faces; measure font-load wall time in the device soak |
| `FtFont::glyphBounds` extra `FT_Load_Char` per culled glyph | No render, metrics-only; bounded by FT's own caching; acceptable per SDK-side measurement in Task 1 |
