# Font Architecture and UX

ISO date: 2026-09-10
Status: Shipped on PSRAM-class builds
Scope: font stack, device-class split, picker UX, reader quick font sheet

## 1. Font stack split

Two device classes, one font stack each:

| Device class | Builds | Reader engine | Font UX |
|---|---|---|---|
| PSRAM (X4 Pro, X4C, Paper Mono) | `CROSSPOINT_TTF_READER=1` | FreeInkBook native TTF (`FontChain`) | family picker + continuous size |
| PSRAM-less (X4, Sticky) | flag absent | legacy EpdFont bitmap path | fixed bitmap picker, no size slider |

- UI chrome remains bitmap (`GfxRenderer` + `EpdFont`) on every device.
- On PSRAM-class builds the legacy reader path is still in the tree as the
  rollback path, but the native-TTF runtime is the active reader path.
- On PSRAM-less builds the TTF stack does not link at all
  (`CROSSPOINT_TTF_READER` absent), so flash/RAM cost and UX stay legacy.

## 2. Font size UX

- PSRAM-class builds: the reader font settings show family + size (8..72 pt,
  `ttfFontPointSize`), because TTF makes size real and continuous.
- PSRAM-less builds: the classic fixed-size list only.
- There is intentionally no mixed bitmap/TTF settings screen; bitmap + size
  control is a no-op and only confuses users.

## 3. Fallback chain

The always-present chain-tail fallback is **Atkinson Hyperlegible Next**,
served from the bitmap font data already baked in flash. A CrossPoint-side
`EpdBookFont : book::RenderFont` adapter (`src/adapters/EpdBookFont.{h,cpp}`)
wraps `EpdFontData` and registers the four atkinson_hn14 style faces as the
chain tail. Fallback semantics:

- never a selectable family;
- covers glyphs/styles a chosen TTF family lacks;
- single baked size (headings render at body size under fallback).

## 4. SD layout and file expectations

The TTF scanner walks the same roots the legacy bitmap registry uses:

```text
/fonts/                      visible root
  <Family>/
    <Family>-Regular.ttf
    <Family>-Bold.ttf
    <Family>_14.cpfont      legacy file: ignored by the TTF scanner
/.fonts/                     hidden root
```

Rules:

- one subfolder per family; nested folders are ignored;
- `.ttf` / `.otf` accepted; `.cpfont`, `.tmp`, `~` backups, `.json`, and
  macOS `._*`/hidden files are skipped;
- same-named families in `/.fonts` and `/fonts` merge by style, with the
  hidden root winning on conflicting styles;
- folder name is the family display name (case preserved);
- up to 4 faces per family, 32-family cap;
- 2MB per-face PSRAM size guard (`BookFontLoader::kMaxFaceBytes`), CWE-400
  discipline.

### 4.1 Style inference

Per accepted file, match the filename (case-insensitive, extension stripped,
word-boundary so `SemiBold` never matches `Bold`) in this priority order:

1. `bolditalic` / `bold_italic` / `bold-italic` → BoldItalic
2. `italic`, `oblique`, `ital` → Italic
3. `bold` → Bold
4. `regular`, `normal`, `book`, `roman`, `text` → Regular
5. no match → heuristics: `semibold`/`demibold`/`medium`/`black`/`heavy`/
   `extrabold` → Bold; `light`/`thin` → Regular; a single-file family is
   Regular regardless of name; a multi-file family keeps its first
   tokenless file as the Regular candidate.

Duplicate style resolution: lexicographically first wins; the rest are
logged. A family with no Regular candidate promotes its first face.

## 5. Settings UI

`TextSettingsActivity` keeps the 4-tab structure (`Font | Size | Layout |
Style`). The TTF additions are:

- **Font tab**: TTF families listed after the built-in and SD-card bitmap
  families. Rows show family name and style availability. The row is
  greyed/disabled when `BookFontLoader::isFamilyAvailable()` fails (no PSRAM
  or a face over the size guard).
- **Size tab**: continuous size picker for TTF families; discrete list for
  bitmap families.
- **Layout tab**: unchanged (line spacing, paragraph spacing, alignment,
  screen margin).
- **Style tab**: focus reading, hyphenation, embedded styles, anti-aliasing.
  Anti-aliasing is real on the TTF path and drives the dual-plane gray
  pipeline when the panel supports it (see
  `2026-09-14-grayscale-pipeline.md`).
- **Reader in-book Text panel**: same options, smaller surface; all changes
  route through `applyTextSettingLive()` → new generation hash → rebuild +
  `pageForChar()` restore.

## 6. Reader quick font sheet

Decision (owner soak feedback, 2026-09-14): size and family quick-adjust is
a compact bottom sheet, not a full-screen activity. The reader Text panel
opens `Overlay::FontSheet`.

### UX contract

- Two-row quick sheet (`ReaderToolbarUi::buildQuickFont`) anchored to the
  bottom edge.
- Row 0 adjusts point size with `-` / `+`; each step applies to the displayed
  page before a FAST refresh.
- Row 1 is a family chooser, not a stepper. Tapping (or confirming) it opens
  the standard modal `OptionPopup`: built-in first, then the discovered TTF
  families in scanner order. The popup lists 8 rows at a time, scrolls by
  drag/button navigation, dismisses on a selection, and returns to the open
  sheet. Unavailable families do not apply a selection.
- The selected row is outlined; the full Settings picker remains available for
  style/coverage details.
- A size step or family selection applies to the currently displayed page and
  refreshes it in place. No full-screen activity push.
- Dismissing the sheet persists settings once and performs the normal full
  reflow / cache invalidation.

### Page-only relayout and memory

`TtfBookRuntime::quickLayoutPage()` runs a transient `ChapterLayout` pass
with the current layout params and a `PageSink` budget:

- skips ahead by `Page::charStart`;
- captures each candidate page out of the engine's per-page arena
  (`QuickPageCapture` deep-copies the run text and records, whose pointers
  die with the callback) instead of painting it — only the last scanned
  page is ever displayed, so one tap costs one layout scan plus a single
  post-scan paint;
- stops once it has passed the displayed anchor (bounded to the first 128
  pages; otherwise the reader falls back to a full reflow);
- uses the runtime's existing scratch/parse arenas, resets them on return;
- never writes or invalidates the committed page cache.

The frame is stored only after the quick page has painted; any prior overlay
snapshot is explicitly discarded before `storeBwBuffer()` to avoid the
BW-buffer "already stored" imbalance. A FAST refresh is used per tap; the
normal render path restores AA plane parity on the final close/reflow.

## 7. Decision log

- 2026-09-10 — one font stack per device class; no dual-mode UX.
- 2026-09-14 — quick bottom sheet replaces full-screen TTF size/family pushes
  in the reader; live page feedback per tap.
- 2026-09-14 — page-only transient layout with a bounded anchor budget; full
  reflow and cache invalidation deferred until sheet close.
- 2026-09-17 — quick-sheet scan captures instead of paints (issue #137):
  one paint per tap after the scan, anchor budget 64 → 128 pages, and
  family selection re-focuses the size row so +/- work without a re-tap.
- 2026-09-14 — manifest path cap 160 bytes, matching
  `SdCardCacheStorage::kDirMax`; longer paths are rejected explicitly.
- 2026-09-14 — two-root family merge; hidden root precedence on conflicts.
- 2026-09-15 — quick-sheet family selection changed from `FontPrev`/`FontNext`
  cycling to the shared `OptionPopup`; the sheet remains open for size/family
  combinations. The picker virtualizes 8 rows so it can expose the scanner's
  33-entry logical list without growing its touch table.
- 2026-09-14 — tokenless Regular candidates retained in multi-file families.

## Cross-links

- Architecture: `2026-09-10-native-ttf-architecture.md`
- Grayscale pipeline: `2026-09-14-grayscale-pipeline.md`
- Implementation plan (historical): `2026-09-10-implementation-plan.md`
- Dictionary flow: `2026-09-14-dictionary-and-word-selection.md`
- SDK/miniz policy: `2026-09-14-sdk-submodules-and-miniz.md`
