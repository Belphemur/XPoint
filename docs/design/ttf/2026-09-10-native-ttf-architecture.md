# Native TTF Font Architecture

ISO date: 2026-09-10
Status: Shipped on PSRAM-class builds (`x4pro`, `x4c`, `papermono`)
Scope: EPUB reader text pipeline (`EpubReaderActivity`, FreeInkBook)

## 1. Problem

The legacy reader text path is bitmap-only: `Epub` library (`ParsedText`,
`TextBlock`, `Section .bin` cache) drawing through `GfxRenderer` / `EpdFont`.
Users could not drop a raw `.ttf` or `.otf` on the SD card and read with it.
The reader also needed engine-grade typography (kerning, hyphenation, UAX#14
line breaking, focus reading, links/footnotes, ruby) without a bespoke
renderer.

## 2. Current Architecture

The book-text pipeline now runs through **FreeInkBook** on
`CROSSPOINT_TTF_READER` builds:

```
EpubReaderActivity
  └─ renderBookTtf()
      └─ TtfBookRuntime
          ├─ SdCardBookSource (HalFile-backed BookSource)
          ├─ SdCardCacheStorage (HalFile-backed CacheStorage, tmp+rename)
          ├─ BookCatalog (catalog.fibc, fingerprint-keyed)
          ├─ ChapterLayoutSession (SAX → UAX#14 → Page)
          ├─ PageCacheWriter / PageCacheReader (FIBP)
          └─ PagePaint / GfxRenderer (paint + chrome)
```

Key components:

| Component | Path | Role |
|---|---|---|
| `BookFontLoader` | `src/BookFontLoader.{h,cpp}` | Family scan, chain construction, content fingerprint |
| `TtfBookRuntime` | `src/activities/reader/TtfBookRuntime.{h,cpp}` | Catalog + layout session + FIBP cache + prefetch |
| `SdCardBookSource` | `src/adapters/SdCardBookSource.{h,cpp}` | `BookSource` over `HalFile` |
| `SdCardCacheStorage` | `src/adapters/SdCardCacheStorage.{h,cpp}` | `CacheStorage` over `HalFile`, tmp + rename |
| `PagePaint` | `src/adapters/PagePaint.{h,cpp}` | Page text/plane painting, 4-level gray parity |
| `EpdBookFont` | `src/adapters/EpdBookFont.{h,cpp}` | Builtin Atkinson fallback adapter |

The legacy bitmap path (`ParsedText`, `TextBlock`, `Section`) is still
present for rollback but is not used on the native-TTF path.

## 3. Target Architecture (now current state)

### 3.1 Runtime

`TtfBookRuntime` owns the chapter pipeline:

- `bookArena_` — catalog resident tables (reset on book switch).
- `cacheArena_` — current chapter's FIBP reader index.
- `scratch_` — layout working set, writer index chunks, page decode.
- `parseArena_` — resident session parse state (inflate window).
- `prescanArena_` — transient image pre-scan, freed after `begin()`.
- `prefetchArena_` — next chapter's cache-reader index.

All are `PoolBytes` (PSRAM-backed on `BOARD_HAS_PSRAM`) and no static BSS.
`openChapterCache()`, `beginChapterSession()`, `stepBuild()`,
`finishSession()`, and `abortSession()` expose the build/serve state machine
to the activity.

### 3.2 Caches

- `<book cache path>/ficache/catalog.fibc` — one per book, keyed by a
  fingerprint of the ZIP central directory + catalog format version.
- `<book cache path>/ficache/s<spine>-<hash8>.fibp` — per-chapter page cache.
  The `<hash8>` is the 8-hex-digit `layoutGenerationHash(params, fingerprint)`
  value. Cache hits replay layout directly; a mismatch rebuilds only the
  affected spine.

Position restore uses `pageForChar()` / `charForAnchor()`; progress records
use the generation-tagged 16-byte shape (see
`docs/design/2026-09-10-progress-save-timer.md`).

### 3.3 Font loading

`BookFontLoader` walks `/fonts/` and `/.fonts/` (one subfolder per family,
up to 4 faces per family, 32-family cap). `fontFingerprint()` is an FNV-1a
over the loaded font bytes XOR `FontChain::styleCoverage()`. The chain is
never null: if no TTF family is selected or load fails, the loader appends
the four baked Atkinson Hyperlegible Next `EpdBookFont` faces as the
chain-tail fallback (`src/adapters/EpdBookFont.{h,cpp}`).

### 3.4 Live settings and quick sheet

Changing family, size, line spacing, alignment, margins, or orientation
changes the generation hash, so the affected FIBP files rebuild and the
reader restores position via `pageForChar()`. The reader toolbar's quick
font sheet (`Overlay::FontSheet`) performs a transient page-only relayout
through `TtfBookRuntime::quickLayoutPage()` and a full reflow on close — see
`2026-09-10-font-architecture-and-ux.md`.

### 3.5 Dictionary and footnotes

Word selection, footnotes, and anchors are live on the TTF path.
`TtfWordSelect` tokenizes engine runs; `DictionaryWordSelectActivity` has a
dedicated TTF constructor; `PageLink` + `charForAnchor()` drive footnote
navigation. Details: `2026-09-14-dictionary-and-word-selection.md`.

### 3.6 Grayscale pipeline

Full-frame dual-plane gray fallback and the uniform tone quantizer are
shipped (`PagePaint::grayTone`, `pagepaint::paintText`, `paintPlanes`).
Details: `2026-09-14-grayscale-pipeline.md`.

## 4. Build gating

`CROSSPOINT_TTF_READER=1` is defined only on PSRAM-class builds in
`platformio.ini` (`x4pro`, `x4c`, `papermono` families). PSRAM-less builds
(C3 / sticky) do not link the TTF stack, so their binaries remain the legacy
bitmap reader path.

## Cross-links

- Executive summary: `docs/design/native-ttf-support-brief.md`
- Font architecture & UX: `2026-09-10-font-architecture-and-ux.md`
- Grayscale pipeline: `2026-09-14-grayscale-pipeline.md`
- Implementation plan (historical): `2026-09-10-implementation-plan.md`
- Dictionary flow: `2026-09-14-dictionary-and-word-selection.md`
- SDK/miniz policy: `2026-09-14-sdk-submodules-and-miniz.md`
