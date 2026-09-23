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
- `.ttf` / `.otf` accepted; `.ttc` collections are accepted on the FT
  backend only (§14.4.2 — stb_truetype cannot parse TTC, so the rollback
  backend skips them explicitly); `.cpfont`, `.tmp`, `~` backups, `.json`,
  and macOS `._*`/hidden files are skipped;
- same-named families in `/.fonts` and `/fonts` merge by style, with the
  hidden root winning on conflicting styles;
- folder name is the family display name (case preserved);
- up to 4 faces per family, 32-family cap;
- 2MB per-face residency threshold (`BookFontLoader::kMaxFaceBytes`): faces
  beyond it stream from SD (§14.5) instead of staying resident in PSRAM —
  only the 24MB stream cap (`kMaxStreamFaceBytes`) still disqualifies a
  face. CWE-400 discipline.

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

### 4.1.1 Face-metadata style resolution (FT backend)

Ported from upstream #3646 (`refineVectorStyles`). After the two-root scan,
`BookFontLoader::refineStyles()` re-derives every family's style roles from
the faces' REAL OS/2 weight + italic flag via
`FtFont::inspectStream` (sfnt header tables only — no face retained, SD
access through the `HalFile` `ReadFn` thunk). Role assignment is
DETERMINISTIC, never SD enumeration order:

- upright face nearest 400 = regular; nearest 700 = bold (must be genuinely
  heavier than the regular pick, else synthesized by the engine);
- same for the italic pair; boldItalic requires a genuinely heavier italic
  than the italic pick;
- an all-italic family promotes the italic nearest 400 to regular;
- ties break to lower weight, then lexicographically smaller path.

Filename inference (§4.1) remains the FALLBACK: an unreadable face keeps its
filename-derived weight/italic estimate as the pick input. Unselected
candidates are dropped from the manifest (the chain synthesizes missing
styles).

**Fingerprint coupling**: `computeFingerprint()`/`computeFingerprintCached()`
fold each loaded slot's face-path hash into the FNV chain after the byte
walk. A role re-assignment can move files between slots without changing the
sequential byte-hash order, so the per-slot path hashes must participate —
otherwise a stale section cache renders the new role map over the old
layout. The stb backend (no `FtFont`) keeps the filename-derived roles
unchanged.

### 14.4.2 TrueType collections (.ttc)

Ported from upstream #3646's registry `.ttc` acceptance. `FtFont` gained
face-index support (SDK PR, `FaceInfo.faceIndex`/`numFaces`): the
metadata pass inspects with `faceIndex = -1`, which scans faces
`0..num_faces-1` and reports the first face with a Unicode cmap; the chosen
index lands in `FontFaceInfo.faceIndex` and is handed to
`FtFont::init()`/`initStream()` when the face loads. A whole `.ttc` file is
one manifest row: filename style inference applies to the container (no
per-face styles), and `validateSfntBytes()` validates the embedded face's
directory (container-absolute table offsets, per the TTC spec). The
fingerprint folds the per-slot face index — two faces of one container share
its bytes, so only the index distinguishes them. The stb backend skips
`.ttc` at scan with an explicit debug log.

### 14.5 SD streaming for oversized faces

Ported from upstream #3646's `openTtfSource`/`prefixRead` pattern (owner
amendment: SD-streaming APPROVED, replacing the old skip behavior). Faces
beyond the 2MB PSRAM residency guard (`kMaxFaceBytes`) load through
`FtFont::initStream` over an open `HalFile` (absolute-offset reads, count 0
= seek probe; ALL access via HalStorage's mutex): the file is never resident.
A ~1MB PSRAM prefix caches the file head (cmap/loca/hmtx sit before the
multi-MB glyf table), collapsing each glyph fault's scattered SD seeks into
one glyf read; when PSRAM cannot fund the prefix (largest-block gate) the
face falls back to pure streaming. `kMaxStreamFaceBytes` (24MB) is the
absolute CWE-400 cap.

**Trade-offs (documented)**: streamed faces give up GPOS kerning
(`setGposByteBudget(0)` — the lazily-copied table would pull scattered
multi-MB SD reads into the render path; GPOS-only variable fonts lose kern
correction when streamed). Advances are identical; kern pairs collapse.
The open `HalFile` is a borrowed source under the same lifetime rules as the
resident borrowed-bytes contract: released in `releaseResidentCaches()`/
`ensureLoaded()`'s clear loop (close-before-reopen discipline). The picker
(`isFamilyAvailable`) no longer greys oversized rows — only the stream cap
disqualifies. The stb backend has no `initStream` and keeps the skip.

**Fingerprint coupling**: the streamed slot has no resident bytes, so its
fingerprint folds the SD head hash (FNV-1a over the first 4KB, chunked off
SD) plus the file size and the SD mtime instead of a full byte walk,
bypassing the SD fp-cache. The mtime distinguishes a same-sized replacement
whose header region is identical. `FibpPrefetchWorker` streams oversized
faces the same way (own HalFile + prefix + `initStream`) and folds the
identical values in the same order, preserving exact `computeFingerprint()`
parity.

### 5.1 TTF-backed CJK/script UI fallback (design §14.6)

Ported from upstream #3646's `setupTtfUiFallbacks`, adapted to the fork's
architecture. On TTF builds, when the ACTIVE reader family covers scripts the
built-in bitmap UI fonts lack (probes: Han, Hiragana, Katakana, Hangul,
Greek, Cyrillic, Hebrew, Arabic, Thai, Devanagari — probed against the
loaded chain), `freeink::book::ttfUiFallback.update()` registers an
`EpdFontFamily` view of that family (one `TtfUiFont` instance per built-in
UI size, SMALL/UI_10/UI_12) as the fallback for each UI font id through the
EXISTING `GfxRenderer::setFallbackFont` / `resolveTextFontId` plumbing — no
new draw path.

- **Adapter** (`src/adapters/TtfUiFont.*`): stub `EpdFontData` per style with
  the generic `glyphMissHandler`/`coverageHandler` hooks (the SD-font seam).
  Glyphs fault on demand: each miss rasterizes the borrowed face at the UI
  size and thresholds 8-bit coverage into a 1bpp MSB-first ring (16 slots).
  A new `EpdFontData::missKind` tag (`MISS_CTX_RING`) tells
  `GfxRenderer::getGlyphBitmap` to take the standard `bitmap[dataOffset]`
  tail instead of reinterpreting the ctx as an SdCardFont overflow ring.
- **Bytes DRY**: style faces borrow the loader's resident font bytes
  (`BookFontLoader::slotFaceBytes`); streamed families (no resident bytes)
  get no UI fallback. Faces are per-instance so a UI draw can never flip the
  reader faces' AA/Crisp render mode; the adapter rasterizes monochrome when
  the module exists, degrading Light→Default→mono-off (a refused
  `setRenderOptions` leaves the requested options applied — every later
  rasterize would fail — so the funnel must retry until accepted).
- **Lazy**: non-regular style faces are created on first use; there is no SD
  prewarm to pay (the brief's 'fault glyphs on demand' choice).
- **Heap gate**: skipped when PSRAM largest block < 256KB; per-instance
  ring/face allocation is nothrow and the instance is skipped on failure.
- **Lifecycle**: registrations are fingerprint-keyed — a loader reload
  (family change, release) invalidates them and the next `update()` releases
  and re-registers. Wiring sites: boot, SettingsActivity refresh, reader
  entry, TextSettings family apply. Non-TTF builds compile a stateless
  no-op singleton (same call sites, zero cost).
- **Fingerprint rule**: UI fallback registration does not alter layout or
  the font fingerprint (UI chrome only), so no tag is folded.

## 5. Settings UI

`TextSettingsActivity` keeps the 4-tab structure (`Font | Size | Layout |
Style`). The TTF additions are:

- **Font tab**: TTF families listed after the built-in and SD-card bitmap
  families. Rows show family name and style availability. The row is
  greyed/disabled when `BookFontLoader::isFamilyAvailable()` fails (no
  PSRAM, or a face over the 24MB stream cap on the FT backend).
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
- 2026-09-23 — face-metadata style resolution ported from upstream #3646:
  roles assigned by real OS/2 weight + italic flag via
  `FtFont::inspectStream` (deterministic nearest-400/700 pick; filename
  heuristics demoted to the unreadable-face fallback), and the per-slot
  face-path hashes fold into the font fingerprint so a role re-assignment
  invalidates FIBP/section caches (FT backend only; stb keeps filename
  inference).
- 2026-09-23 — TTF CJK/script UI fallback ported from upstream #3646:
  `TtfUiFont` EpdFontFamily adapters at the built-in UI sizes fault glyphs
  on demand through the `EpdFontData` miss seam (new `missKind` tag keeps
  the SdCardFont overflow path separated), borrowing the loader's resident
  bytes — no byte copies, no SD prewarm, PSRAM heap-gated, fingerprint-keyed
  lifecycle.
- 2026-09-23 — `.ttc` collection support ported from upstream #3646 via the
  SDK face-index PR (`FtFont::init/initStream/inspect*` gain faceIndex;
  inspect scan mode resolves the first Unicode-cmap face). Registry accepts
  `.ttc` on the FT backend only; the resolved face index joins the
  fingerprint role-map tag. FibpPrefetchWorker folds the same role-map tag
  (path hash + face index) restoring exact fingerprint parity with
  `computeFingerprint()`.
- 2026-09-23 — SD streaming for oversized faces ported from upstream #3646
  (owner amendment 2026-09-23: streaming approved over skip): faces beyond
  the 2MB residency guard load via `FtFont::initStream` over a borrowed
  open HalFile with a 1MB PSRAM head prefix; GPOS kerning off on streamed
  faces; fingerprint identity = SD head hash + size; prefetch worker mirrors
  the streamed path for exact parity.

## Cross-links

- Architecture: `2026-09-10-native-ttf-architecture.md`
- Grayscale pipeline: `2026-09-14-grayscale-pipeline.md`
- Implementation plan (historical): `2026-09-10-implementation-plan.md`
- Dictionary flow: `2026-09-14-dictionary-and-word-selection.md`
- SDK/miniz policy: `2026-09-14-sdk-submodules-and-miniz.md`
