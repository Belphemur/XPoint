# Dictionary and Word Selection on the TTF Path

ISO date: 2026-09-14
Status: Shipped
Scope: reader word lookup, footnotes, and anchors on the native-TTF path

## 1. Contract

The TTF path must support the same dictionary flow the legacy bitmap path
already had: long-press / touch-select a word, move through words in reading
order, resolve bare numeric markers as footnotes, look the word up, and
return to the reader with position intact.

## 2. Entry point

`EpubReaderActivity::openDictionaryWordSelect()` (TTF branch):

1. Shows the "no dictionary configured" popup first — engine-independent
   behavior parity with the legacy path.
2. If the active runtime is TTF:
   - takes the render lock, marks the runtime scratch arena;
   - reads the current page via `TtfBookRuntime::readPage()`;
   - builds the word payload with `buildTtfWordSelectData()` using the chain
     from `makeLayoutParams()`;
   - attaches the page's captured `currentPageFootnotes`;
   - releases the scratch mark;
   - launches the TTF-mode selector with the legacy `FootnoteResult`
     callback.
3. Touch coordinates pass straight through: the TTF selector takes touch
   coords and does not need the oriented margins the legacy `Page` path uses.
4. The legacy `Page`-based path is unchanged for bitmap-engine books.

## 3. Word payload

`src/activities/reader/TtfWordSelect.{h,cpp}` builds `TtfWordSelectData`:

- token text storage in a `PoolBytes` arena (PSRAM-backed on PSRAM builds);
- `std::vector<TtfWordBox> boxes` — one selectable token per box;
- `std::vector<FootnoteEntry> footnotes` — captured page footnotes.

Each `TtfWordBox` carries:

| Field | Meaning |
|---|---|
| `text` | pointer into the payload arena (NUL-terminated raw token) |
| `x`/`y`/`width`/`height` | page-logical screen coordinates, `y` = line-box top |
| `textOffset` / `textLength` | trimmed lookup range |
| `rawLength` | pre-trim span, needed so footnote resolution can see parens |
| `styleFlags` | engine style bits for the run |
| `selectionGroup` | per-page unique id of the logical word |
| `syntheticHyphen` | engine-baked line-break hyphen flag |

`buildTtfWordSelectData()` copies everything out of the page, so the
caller's scratch mark can be released as soon as it returns — the reader's
scratch arena is not held across the child activity's lifetime.

## 4. Run splitting and anchoring

The engine's `PageTextRun` now carries per-run chapter character anchoring
(`charStart`, `charLen`), which is the substrate for reliable word grouping
and footnote resolution (upstream SDK #19). `TtfWordSelect` splits each run
into whitespace-delimited tokens and joins fragments across run boundaries
using `PageTextRun::LayoutFirstContinues` / `LayoutLastContinues`, so a word
split by pagination stays one logical selection.

Selection group ids are per-page unique, mirroring the legacy `TextBlock`
`selectionGroup` semantics. Synthetic hyphens are flagged with
`PageTextRun::LayoutHyphenated` and stripped from the trimmed lookup text.

## 5. Selector activity

`DictionaryWordSelectActivity` has a dedicated TTF constructor:

- `ttfMode = true`;
- `ttfData` owns the token text and footnotes;
- `ttfRender` is a `PageRenderFn` hook back into
  `EpubReaderActivity::renderTtfSelectorPage()` — the engine run text lives
  in the reader's scratch arena and cannot be re-read by the child, so the
  reader repaints the page for the selector.
- Footnote resolution uses the legacy `FootnoteResult` callback, so
  `navigateToHref()` / `EpubReaderFootnotesActivity` behave exactly as on
  the bitmap path.

Word-box geometry and hit-testing are shared with the legacy path
(`wordAt()`, `closestInRow()`, row navigation). Only the data source
differs.

## 6. Footnote flow

- `TtfBookRuntime` / `ChapterLayoutSession` emit `PageLink` anchors.
- The reader captures the current page's footnote list into
  `currentPageFootnotes` during render.
- `openDictionaryWordSelect()` attaches that list to the selector.
- `resolveFootnoteOrFinish()` checks the raw token (including parens) against
  the footnote list and, when it matches a bare numeric marker, returns the
  footnote href through `FootnoteResult`.
- The reader's activity-result callback calls `navigateToHref(...,
  /*savePosition=*/true)`, preserving reading position.

## 7. Host tests

- `test/ttf_word_select/TtfWordSelectTest.cpp` — real Amazon Ember TtfFont;
  verifies token split, trim-range semantics (`textOffset`/`textLength` vs
  `rawLength`), and line-box geometry from the same `FontChain`.
- `CROSSPOINT_TTF_READER=1` is defined for the host test target in
  `test/ttf_word_select/CMakeLists.txt`.

## Cross-links

- Architecture: `2026-09-10-native-ttf-architecture.md`
- Font architecture & UX: `2026-09-10-font-architecture-and-ux.md`
- Grayscale pipeline: `2026-09-14-grayscale-pipeline.md`
- SDK/miniz policy: `2026-09-14-sdk-submodules-and-miniz.md`
