# Native TTF Implementation Plan (Historical)

ISO date: 2026-09-10
Status: Historical plan — retained for decisions, error handling, risks, testing, rollback
Scope: reader migration from bitmap text to FreeInkBook native TTF

> **Current state (2026-09-14).** The reader-facing plan has shipped on
> PSRAM-class builds: FreeInkBook runtime, FIBP caches, TTF family loading,
> continuous size, quick font sheet, dictionary/footnote parity, ruby, and
> the dual-plane gray pipeline are live. The legacy bitmap reader path is
> still in the tree as the rollback path; the final Phase 4 source deletion
> has **not** been performed yet. This document is the historical record of
> how the migration was planned, not a description of the current code
> layout.

## 1. Original phased plan

### Phase 0 — Build wiring

Add FreeInkBook to `lib_deps` as `symlink://freeink-sdk/libs/book/FreeInkBook`.
Gate: `pio run` links clean.

### Phase 1 — Font loading + storage adapters

- `src/BookFontLoader.{h,cpp}` — scan, PSRAM-first loading, `FontChain`,
  fingerprint, builtin fallback.
- `src/adapters/SdCardBookSource.{h,cpp}` — `BookSource` over `HalFile`.
- `src/adapters/SdCardCacheStorage.{h,cpp}` — `CacheStorage` over `HalFile`
  with tmp + rename `endWrite()`.
- `src/adapters/FrameTargetFactory.{h,cpp}` — orientation → `FrameRotation`.

### Phase 2 — Reader integration

- `TtfBookRuntime` + `EpubReaderActivity` FIBP reader/writer paths,
  `ChapterLayoutSession`, progress mapping, prefetch, all behind
  `-DCROSSPOINT_TTF_READER=1`.
- Gate: full read of a Latin EPUB end-to-end, cache write/reopen/position
  restore/live settings reflow.

### Phase 3 — Settings UI + manifest

- TTF Font/Size/Style rows, preview through the active chain,
  `CrossPointSettings` keys, `SdCardFontSystem` untouched.

### Phase 3.5 — Feature parity gate

Every reader-text feature itemized and closed as: verified on-device,
CrossPoint adapter, upstream ask, or documented v1 loss. Covered engine
items (focus reading, synthetic bold, underline, sup/sub, link/anchor) were
verified; CrossPoint adapters shipped for dictionary hit-testing, selection
groups, footnote list, style-bit table, CSS padding fold, image policy; the
remaining engine gaps were either closed upstream (ruby) or accepted as
documented losses.

### Phase 4 — Legacy reader path removal

Intended to flip the flag default on all builds and delete
`ParsedText.*`, `blocks/`, `Section.*`, and the reader `Page` model after
the parity checklist is green. **Still pending**; the legacy path remains in
the tree as the rollback path.

## 2. Key technical decisions

| # | Decision | Rationale |
|---|---|---|
| 1 | stb_truetype via `TtfFont`, not FreeType | zero new dependencies; FreeType streaming stays the documented upgrade path behind the same `BookFont` interface |
| 2 | PSRAM-first font residency with a bounded DRAM fallback in the loader | C3/Sticky do not link the TTF stack; PSRAM pool exhaustion needs an explicit, budgeted fallback |
| 3 | FIBP + `layoutGenerationHash(params, fingerprint)` replaces the Section `.bin` cache | generalizes the proven settings-validation design; `pageForChar`/`charForAnchor` cover progress restore |
| 4 | Keep `GfxRenderer` + `EpdFont` for UI chrome | ~80 global font objects and all menus/settings/popups stay id-currency; high risk, zero reader value to migrate now |
| 5 | Engine consumed as submodule symlink, unmodified except gated extensions | `platformio.ini` already uses this pattern for FreeInkUI; upstream changes stay narrow |
| 6 | Scan `/fonts/` for discovery; optional `free-fonts.json` for metadata | SD is user-writable; scan-first mirrors `SdCardFontRegistry::discover()` |
| 7 | Letter spacing stays out of v1 | engine `LayoutParams` had no tracking field; a post-hoc x-shift adapter would break justification/kerning |
| 8 | Style mapping is a translation table, not a cast | only BOLD/ITALIC/UNDERLINE coincide; old decoration bits collide with engine sup/sub |
| 9 | 4-level gray parity is CrossPoint-side | engine already has 8-bit coverage; only the last-mile plane packing is CrossPoint-owned |

## 3. Files to create / modify / delete

### Created

| File | Purpose |
|---|---|
| `src/BookFontLoader.{h,cpp}` | TTF discovery, loading, chain management |
| `src/adapters/SdCardBookSource.{h,cpp}` | `BookSource` over `HalFile` |
| `src/adapters/SdCardCacheStorage.{h,cpp}` | `CacheStorage` over `HalFile`, tmp+rename |
| `src/adapters/FrameTargetFactory.{h,cpp}` | orientation → `FrameTarget` |
| `src/adapters/PagePaint.{h,cpp}` | page painting + dual-plane gray parity |
| `src/adapters/EpdBookFont.{h,cpp}` | Atkinson fallback adapter |
| `src/activities/reader/TtfBookRuntime.{h,cpp}` | chapter pipeline + caches |

### Modified

| File | Change |
|---|---|
| `platformio.ini` | `lib_deps` += FreeInkBook symlink; `CROSSPOINT_TTF_READER=1` on PSRAM-class envs |
| `src/CrossPointSettings.{h,cpp}` | TTF keys + resolver |
| `src/main.cpp` | `fontLoader.begin()` beside `sdFontSystem.begin()` |
| `src/ReaderFontSizes.h` | TTF continuous size branch |
| `src/activities/reader/EpubReaderActivity.{h,cpp}` | TTF render path |
| `src/activities/settings/TextSettingsActivity.{h,cpp}` | TTF rows |
| `src/activities/settings/TextSettingsPreview.{h,cpp}` | preview through active chain |
| `src/network/CrossPointWebServer.cpp` | settings schema |

### Planned deletion (not yet executed)

| Path | Why |
|---|---|
| `lib/Epub/Epub/ParsedText.{h,cpp}` | only reader layout + preview + TxtReader use it |
| `lib/Epub/Epub/blocks/` | `TextBlock`/`BlockStyle` consumers are the reader path |
| `lib/Epub/Epub/Section.{h,cpp}` and reader `Page`/`PageLine` model | replaced by FIBP |

## 4. Error handling

Aligned with the project hierarchy (LOG_ERR + return false; no exceptions):

| Failure | Detection | Response |
|---|---|---|
| OOM on arena/font bytes | `nullptr` from `makeUniqueNoThrow`/pool | `LOG_ERR`, fall back to builtin Atkinson chain, reader stays functional |
| Font too large | per-face size guard | family listed but disabled in UI with a size hint |
| `TtfFont::init` failure | `ready() == false` | family dropped from active list, `LOG_ERR`, fallback |
| Arena exhaustion mid-layout | `Arena::alloc` `nullptr` / `failedAllocSize()` | `ChapterLayout` returns `OutOfMemory` → build-error popup, prior page retained |
| Missing glyphs | `renderText` first-missing return | logged once per page; visible gaps, no crash |
| Cache stale | `PageCacheReader::open` → `Stale` | silent rebuild |
| Torn cache file | tmp + rename; failed publish leaves `.tmp` for retry | previous final cache retained; rotate-rename failure non-fatal for `.tmp` |
| SD removed mid-read | `BookSource::readAt < 0` / open failure | `BookStatus::IoError` → activity error screen |
| Legacy progress entry | missing generation tag | restore to chapter start; never fail the open |

## 5. Risk and mitigation

| # | Risk | Mitigation |
|---|---|---|
| R1 | PSRAM-less budget insufficient | TTF stack does not link there; no TTF arenas or font bytes |
| R2 | CJK / very large TTF | 2MB per-face guard; oversized faces rejected; FreeType streaming is the future upgrade path |
| R3 | Feature parity gaps | Phase 3.5 checklist; engine-covered items verified, adapters shipped, remaining gaps documented |
| R4 | Glyph-arena thrash | FIBP skips layout on hits but still rasterizes; monitor `Arena::highWater()`/`failedAllocSize()` |
| R5 | Cache-version migration | separate `ficache/`; old `.bin` files go stale and are swept later |
| R6 | Night mode / inversion | `PageRenderer` writes SET=white; `SETTINGS.screenInverted` inverts at display time |
| R7 | Synthetic style rendering | style coverage in fingerprint; synthetic bold engine-native; italic degrades honestly |
| R8 | Engine unproven on C3 | debug activity + flag-gated soak on PSRAM builds |
| R9 | AA parity regression | dual-plane parity path shipped; uniform quantizer; device validation pending |

## 6. Testing strategy

- Build matrix: `pio run -e default`, `-e gh_release`, `-e x4pro`, `-e sticky`
  at phase boundaries; `pio check` (cppcheck excludes `freeink-sdk/*`).
- Host tests: quantizer boundaries, base/plane consistency, strip/full-frame
  equivalence, `TtfWordSelect` token split, loader fingerprint stability.
- On-device probes (owner): heap telemetry, cache invalidation matrix,
  corpus (Latin serif/sans, small, oversized, CJK, Arabic, `.txt`),
  settings soak, power-loss, 4 orientations.

## 7. Rollback plan

- `CROSSPOINT_TTF_READER=1` is the build-flag kill switch; the legacy reader
  path remains intact.
- Settings keys are append-only; `fromJson` ignores unknown keys on
  downgrade; legacy bitmap families are not repurposed.
- FIBP lives in `ficache/`; a rollback build ignores it completely.
- Removing the FreeInkBook symlink + flag lines returns the tree to the
  pre-TTF build.

## Cross-links

- Architecture: `2026-09-10-native-ttf-architecture.md`
- Font architecture & UX: `2026-09-10-font-architecture-and-ux.md`
- Grayscale pipeline: `2026-09-14-grayscale-pipeline.md`
- Dictionary flow: `2026-09-14-dictionary-and-word-selection.md`
- SDK/miniz policy: `2026-09-14-sdk-submodules-and-miniz.md`
