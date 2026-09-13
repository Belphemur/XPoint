# Design: Native TTF Font Support for CrossPoint Reader

| | |
|---|---|
| **Status** | Draft — reviewed against codebase 2026-09-09 |
| **Worktree** | `crosspoint-x-reader-design-ttf` (branch `design/native-ttf-support`) |
| **Engine** | FreeInkBook (`freeink-sdk/libs/book/FreeInkBook/`), FreeInkUI (`freeink-sdk/libs/ui/FreeInkUI/`) |
| **Authoritative engine reference** | `freeink-sdk/docs/freeink-book.md` |

Every file path, class name, and function signature cited below was verified against the code in this worktree on 2026-09-09. Line numbers refer to the current revision and may drift.

---

## 1. Problem Statement

The reader currently renders book text only through the **EpdFont bitmap engine** (`lib/EpdFont/`): either flash-baked Noto Serif / Atkinson Hyperlegible Next families at four fixed point sizes, or `.cpfont` v4 files that must be produced by an offline conversion toolchain (`lib/EpdFont/scripts/fontconvert_sdcard.py`). Users cannot drop a plain `.ttf` on the SD card and use it.

**Goals**

1. **Native TTF rendering, no conversion.** Raw `.ttf`/`.otf` files under `/fonts/` on the SD card render through the FreeInkBook engine (`TtfFont` → `FontChain` → `ChapterLayout` → `PageRenderer`), replacing the EpdFont-based reader path (`lib/Epub/Epub/ParsedText`, `lib/Epub/Epub/blocks/`, `lib/Epub/Epub/Section`).
2. **On-the-fly settings changes.** Font family, size, line spacing, and alignment changes from the Text settings UI (and the reader's in-book Text panel) re-paginate live without leaving the book, preserving reading position.
3. **PSRAM-aware caching.** Rendered chapters persist as FreeInkBook **FIBP** page caches (`PageCacheWriter`/`PageCacheReader`, generation-hashed); glyph and working arenas are allocated from PSRAM where present and sized down for the PSRAM-less ESP32-C3 where not.
4. **Remove the bitmap engine from the reader path.** `GfxRenderer`/`EpdFont` remains for UI chrome (headers, menus, popups, settings screens); book text rendering goes exclusively through FreeInkBook.

**Non-goals**

- UI chrome fonts stay bitmap (`EpdFont` families inserted in `src/main.cpp`) for now; migrating chrome to TTF is a follow-up (§11).
- No Gray8 (8-bit page buffer) rendering — a full-page 8bpp buffer (800×480 = 384KB) cannot fit C3 DRAM and no driver in the build matrix consumes raw Gray8. Panels are 1-bit controllers but still deliver 4-level text AA today: the bitmap reader path drives dual LSB/MSB plane RAM + the panel AA waveform (`lib/GfxRenderer/GrayPlanes.h` tone mapping → `GfxRenderer::displayGrayBuffer()`, `SETTINGS.textAntiAliasing`). TTF v1 pages render `FrameFormat::Mono1Dithered`/`Mono1Sharp`; 4-level gray parity is a **CrossPoint-side pipeline change** (the engine already rasterizes true 8-bit glyph coverage — only the dual-plane packing is missing), scoped in §5 D9 / §8 R9 / §11 Q7.
- No OpenType feature engineering (GSUB beyond FreeInkBook's built-in fi/fl/ff/ffi/ffl ligatures), no new Arabic shaping beyond what the engine's `arab_shaping` module already does, no variable-font axis control.
- No FreeType streaming for multi-MB CJK files in the first iteration — the whole-file-addressable constraint of `TtfFont` is accepted and enforced with a file-size gate; the FreeType upgrade path keeps the same `BookFont` interface and is documented in `freeink-sdk/docs/freeink-book.md`.

---

## 2. Current Architecture

### 2.1 Reader text pipeline today

```
src/main.cpp
  ├─ global EpdFontFamily objects (flash) → renderer.insertFont(id, family)   [main.cpp:538-556]
  ├─ FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());  [main.cpp:56]
  └─ sdFontSystem.begin(renderer)                                            [main.cpp:559]
        └─ SETTINGS.sdFontIdResolver = trampoline → resolveFontId()           [SdCardFontSystem.cpp:44-47]

src/activities/reader/ReaderActivity.cpp:58 → sdFontSystem.ensureLoaded(renderer) on reader entry

EpubReaderActivity::renderBook()                                             [EpubReaderActivity.cpp:1412-1781]
  ├─ renderer.getOrientedViewableTRBL() + SETTINGS.screenMargin + status bar → viewport
  ├─ SETTINGS.readerRenderSpec(viewportW, viewportH) → ReaderRenderSpec       [CrossPointSettings.cpp:307-321]
  ├─ Section::loadSectionFile(renderSpec)      ← cache hit  (lib/Epub/Epub/Section.cpp)
  ├─ Section::createSectionFile / buildSomeMore ← cache miss (paged build, partial files)
  │     └─ ParsedText::layoutAndExtractLines(renderer, fontId, viewportWidth, cb)
  │           ├─ measureWordWidth → renderer.getTextAdvanceX / getSpaceWidth / getKerning
  │           ├─ computeLineBreaks / hyphenation / focus-reading splits
  │           └─ TextBlock lines (flat arena: textOff/xpos/styles/focus arrays + text)
  └─ Page::render(renderer, fontId, x, y)                                     [Page.h:119]
        └─ TextBlock::render(renderer, fontId, x, y)                          [TextBlock.h:117]
              └─ GfxRenderer::drawText / drawCharDither → EpdFontFamily → EpdFont / SdCardFont
```

**Component table**

| Component | Path | Role | Fate under this design |
|---|---|---|---|
| `GfxRenderer` | `lib/GfxRenderer/GfxRenderer.{h,cpp}` | 1bpp framebuffer, orientation transforms, `drawText`/`getTextWidth`/`getSpaceAdvance`/`getKerning`/`getLineHeight(fontId[, compression])`, SD-font registration (`registerSdCardFont` :168, `fallbackFontMap_` :96, `resolveTextFontId` :102) | **RETAINED** for UI chrome; reader text calls removed in Phase 4 |
| `EpdFont` / `EpdFontData` | `lib/EpdFont/EpdFont.h`, `EpdFontData.h` | flash bitmap glyphs, kerning, ligatures (`getKerning`, `applyLigatures`) | RETAINED (UI chrome fonts) |
| `EpdFontFamily` | `lib/EpdFont/EpdFontFamily.h` | 4-style container; `Style` enum REGULAR=0/BOLD=1/ITALIC=2/BOLD_ITALIC=3 (:10-14) | RETAINED (UI) |
| `SdCardFont` | `lib/EpdFont/SdCardFont.h` | `.cpfont` v4 loader (`CPFONT_VERSION 4` :20), `prewarm()` :52, `buildAdvanceTable()` :73, overflow ring, `contentHash()` :149 | RETAINED (UI CJK fallback) until UI migration; **removed from reader path** |
| `SdCardFontManager` / `SdCardFontRegistry` | `lib/EpdFont/SdCardFontManager.{h,cpp}`, `lib/EpdFont/SdCardFontRegistry.{h,cpp}` | family discovery + per-size loading | RETAINED (UI fallback) |
| `SdCardFontSystem` | `src/SdCardFontSystem.{h,cpp}` | facade: `begin(GfxRenderer&)`, `ensureLoaded(GfxRenderer&)`, `resolveFontId(family, pt)` (h:27; def cpp:173), `registry()`, `markRegistryDirty()`/`refreshIfDirty()` | EXTENDED (§3.6): also fronts TTF fonts |
| `ParsedText` | `lib/Epub/Epub/ParsedText.{h,cpp}` | word-level layout: `layoutAndExtractLines` (ParsedText.h:129), `computeLineBreaks` :87, `calculateWordWidths` :101 | REPLACED by `ChapterLayout` |
| `TextBlock` / blocks | `lib/Epub/Epub/blocks/TextBlock.{h,cpp}` | cached line render (`render(renderer, fontId, x, y)` :117), flat per-page arena | REPLACED by `Page` + `PageRenderer` |
| `Section` | `lib/Epub/Epub/Section.{h,cpp}` | per-spine `.bin` cache; `SECTION_FILE_VERSION = 46` (Section.cpp:57), partial sentinel `0xFE-(v-28)` :74, header stores `spec.fontId`+`lineCompression` and validates on load (:128, :195) | REPLACED by FIBP `PageCache` |
| `ReaderRenderSpec` | `lib/Epub/Epub/ReaderRenderSpec.h` | `fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth/Height, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled` (:13-24); section cache validates on every field | MAPPED into `LayoutParams` + generation hash (§3.4) |
| `EpubReaderActivity` | `src/activities/reader/EpubReaderActivity.{h,cpp}` (3280 lines) | navigation, prefetch (`prefetchNextChapterDuringDisplay` cpp:478), heap gates (`BACKGROUND_BUILD_MIN_FREE_HEAP=32KB`, `RENDER_MIN_FREE_HEAP=24KB`, h:149-154), toolbar/footnotes/dictionary/stats | KEPT; page source swapped (§3.5) |
| `TextSettingsActivity` / `TextSettingsPreview` | `src/activities/settings/TextSettingsActivity.{h,cpp}`, `TextSettingsPreview.{h,cpp}` | 4 tabs `Tab::{Family,Size,Layout,Style}` (TextSettingsActivity.h:21); preview re-lays sample text through `ParsedText` with `PreviewKey` cache (TextSettingsPreview.cpp:96-111) | EXTENDED for TTF (§3.6, §12) |
| `CrossPointSettings` | `src/CrossPointSettings.{h,cpp}` | `fontFamily` (FONT_FAMILY enum :117), `fontPointSize` (default 14 :124), `sdFontFamilyName[32]` :361, `getReaderFontId()` (cpp:407-433), `clearSdFontFamily()` (cpp:400-405), `readerRenderSpec()` (cpp:307) | EXTENDED (§3.6) |
| `ReaderFontSizes` | `src/ReaderFontSizes.h` | `BUILTIN_READER_POINT_SIZES = {12,14,16,18}` :16, `readerFontPointSizes(registry, sdFamilyName)`, `snapToNearestPointSize` | EXTENDED: TTF continuous size set |
| `fontIds.h` | `src/fontIds.h` | generated by `lib/EpdFont/scripts/build-font-ids.sh`; NOTOSERIF/ATKINSON_HN (aliased NOTOSANS)/UI_10/UI_12/SMALL ids; 0 = sentinel | UNCHANGED (bitmap only); TTF has **no font IDs** |

**Key observations that drive the design**

- The section cache key already mixes render-affecting settings: Section.cpp:195 rejects a cached `.bin` when `spec.fontId`/`lineCompression`/viewport/etc. differ. FIBP generalizes this with `layoutGenerationHash(params, fontFingerprint)`.
- `fontId` is the currency of the current text API — a stable int resolving to `EpdFontFamily` or `SdCardFont`. TTF has no stable int identity (files come and go on SD); the design replaces the *currency* with `FontChain*` objects and a fingerprint hash for cache keys.
- `EpdFontFamily::Style` BOLD=1/ITALIC=2 coincide with FreeInkBook `StyleBold=1, StyleItalic=2` (`include/BookFont.h:17-24`), and the old UNDERLINE=4 happens to coincide with `StyleUnderline=4` — but the old STRIKETHROUGH=8/SUP=16/SUB=32/RUBY_CONTINUE=64 decoration bits collide with `StyleSuperscript=8`/`StyleSubscript=16`, so reader-path style mapping needs an explicit translation table, not a cast (§5 D8, Phase 3.5.9).
- The reader already tolerates slow builds: `Section` supports partial files (`SECTION_FILE_PARTIAL_VERSION`), background `buildSomeMore()`, and the `FrameBufferLoan` idiom loans the 48KB framebuffer to the builder. FIBP's `PageCacheWriter::suspend()` + `PageCacheReader::isPartial()` + `ChapterLayoutSession::step()` reproduce this exact UX.

### 2.2 FreeInkBook engine (to be adopted)

Library is present as a submodule but **not yet linked**: `platformio.ini` `lib_deps` (:120-148) includes `FreeInkUI=symlink://freeink-sdk/libs/ui/FreeInkUI` (:133) but nothing from `freeink-sdk/libs/book/FreeInkBook/`. FreeInkBook is C++17, freestanding (no exceptions/RTTI), depends primarily on its caller-provided `Arena` buffers — **with one caveat**: its XML layer (`XmlSax`) creates an Expat parser whose internal pools use the system allocator (`XmlSax.h:11`), bounded to the chapter XML size (§3.3).

| Component | Path | Contract (verified) |
|---|---|---|
| `BookFont` / `RenderFont` / `GlyphBitmap` | `include/BookFont.h` | `advance(cp, sizePx, styleFlags)`, `lineHeight/ascent(sizePx)`, `kerning(left,right,sizePx,style)`, `ligature(left,right,style)`; `RenderFont::hasGlyph(cp)`, `rasterize(cp, sizePx) → const GlyphBitmap*` (8-bit coverage, valid until next `rasterize` on the same font) |
|| `TtfFont` | `include/render/TtfFont.h`, `src/render/TtfFont.cpp` | `bool init(const uint8_t* data, uint32_t len, Arena& glyphArena)` — borrows `data` for the duration of `init()` (stb_truetype builds its `stbtt_fontinfo` from the addressable font bytes), then the raw file bytes can be released; only parsed glyph data stays resident in the arena. Direct-mapped advance/glyph tables **append** to the arena; when it fills the cache flushes entirely and rebuilds (no LRU, TtfFont.cpp:133 — see §3.3 for the shared-arena caveat). `init` only checks `len < 12` (TtfFont.cpp:36); caller must validate sfnt structure before calling (§3.3). |
| `FontChain` | `include/render/TtfFont.h` | `bool add(RenderFont*, uint8_t styleFlags)` (≤8 faces); `RenderFont* fontFor(cp, styleFlags, uint8_t* faceFlagsOut)` — selection: exact style with glyph → partial → regular → anything with glyph; `styleCoverage()` bitmask (for fingerprints); metrics from the first registered face; kerning only when both codepoints resolve to the same face. **No synthetic bold/italic in the chain itself** — `faceFlagsOut` lets a renderer know the chosen face is not the requested style (render-side synthetic-bold double-strike exists in `PageRenderer::renderText`, PageRenderer.cpp:183-195; no synthetic italic). |
| `Arena` | `include/BookArena.h` | bump allocator over a caller buffer: `alloc(size, align)`, `mark()/release()`, `reset()`, `highWater()`, `failedAllocSize()`; returns `nullptr` when exhausted, never aborts, does no internal malloc |
| `ChapterLayout` | `include/layout/ChapterLayout.h` | `layout(source, zip, entry, href, params, scratch, sink, ...)`; `LayoutParams{pageWidth/Height, margins, baseSizePx, language, font, lineSpacingPct, paragraphSpacingPct, defaultAlign, orphan/widowLines, embeddedStyles, focusReading, stylesheet, hyphenator}` — **no letter-spacing field** (engine change required, §5); `ChapterLayoutSession` for resumable builds (`begin/step(minNewPages)/done/abort`, `parseScratch`/`prescanScratch` split-out arenas); `layoutPlainText(...)` for `.txt` |
| `PageSink` / `Page` | `include/layout/ChapterLayout.h` | `onPage(const Page&)`; `Page{PageTextRun{text,len,x,baselineY,sizePx,styleFlags}, PageImage, PageLink, charStart}` — `charStart` is the relayout-stable position anchor |
| `PageRenderer` | `include/render/PageRenderer.h`, `src/render/PageRenderer.cpp` | `FrameTarget{framebuffer, width, height, widthBytes, format, rotation}`; `FrameFormat::Mono1Dithered` (1bpp MSB-first, SET=white; glyphs carry real 8-bit coverage consumed as e-paper-tuned dithered edge AA — engine default), `Mono1Sharp` (coverage ≥120 hard threshold, no edge softening), `Gray8` (8bpp `lum = 255-coverage`; unused here, §1); `renderText(page, fontChain, target, &firstMissing)`, `renderImages(...)` (Floyd–Steinberg error diffusion for mono targets, PageRenderer.cpp:120-158; Bayer `kBayer4` is text-edge only via `inkPixel`), `render(page, fontChain, source, zip, scratch, target)` |
| `PageCache` (FIBP v3) | `include/cache/PageCache.h` | `layoutGenerationHash(params, fontFingerprint)`, `pageCacheName(spine, hash, out, cap)` → `s<N>-<hash8>.fibp`; `PageCacheWriter` (streaming, `suspend()` commits partial files, `readPage` via `readBackAt`), `PageCacheReader::open(storage, name, expectedHash, arena)` (Stale on mismatch), `pageForChar(charOffset)`, `charForAnchor(idHash)` |
| `Book` / `BookSource` / `CacheStorage` | `include/FreeInkBook.h`, `include/BookStorage.h` | EPUB container: `open(source, bookArena, scratch)`, `zip()`; `BookSource{readAt, size}`; `CacheStorage{exists/remove/fileSize/readAt/beginWrite/write/endWrite/readBackAt}` (impls should write temp + rename) |
| `BookProfile` | `include/BookProfile.h` | `-DFREEINK_BOOK_SMALL=1` (PSRAM-less, ~200KB-free-heap class) or `-DFREEINK_BOOK_LARGE=1`, mutually exclusive; **there is no `-DFREEINK_BOOK_PROFILE` flag** — capacities live at use sites (advance/glyph slots `TtfFont.h:97-99`; `kMaxPages`/`kMaxAnchors` `PageCache.h:105-106`) |
| `BitmapBookFont` / `TtfGlyphSource` | FreeInkUI `include/FreeInkUIBookFont.h` | `BitmapBookFont(const BitmapFont& = kNotoSansFont) : book::RenderFont` — adapter for the existing bitmap fonts (metrics ignore sizePx; ~4KB mutable `coverage_[64*64]` per instance, FreeInkUIBookFont.h:86 — 4 styles = 16KB static BSS, accounted in C3 budget); `TtfGlyphSource` registered via `DisplayTarget::setGlyphFallback` for UI text |

FreeInkUI is already linked (`platformio.ini:133`) and is used for reader toolbar chrome today, so the chrome integration point already exists.

### 2.3 Concept mapping: GfxRenderer/EpdFont → PageRenderer/TtfFont

| Concern | Today (bitmap) | Target (FreeInkBook) | Notes |
|---|---|---|---|
| Font identity | `int fontId` → `fontMap` / `sdCardFonts_` (GfxRenderer.h:54, :59) | `FontChain*` owned by `BookFontLoader`; no int id | IDs die with the reader path; cache keys use fingerprints |
| Style | `EpdFontFamily::Style` (REGULAR..BOLD_ITALIC 0..3, plus STRIKETHROUGH=8/SUP=16/SUB=32/RUBY_CONTINUE=64 decoration bits) | `StyleFlags` (StyleBold=1, StyleItalic=2, StyleUnderline=4, StyleSuperscript=8, StyleSubscript=16, BookFont.h:17-24) | BOLD/ITALIC/UNDERLINE coincide; the old decoration bits collide with engine sup/sub — explicit translation table, not a cast (§5 D8, Phase 3.5.9) |
| Per-glyph metrics | `EpdGlyph` from `.cpfont`/flash; `buildAdvanceTable` + `getAdvance` (SdCardFont.h:73-79) | `TtfFont` direct-mapped advance table over `BookFont::advance` | engine-internal, arena-backed |
| Glyph raster | 2-bit `.cpfont` bitmaps + overflow ring (8 slots) | `rasterize()` → 8-bit coverage `GlyphBitmap`, arena glyph cache with flush-and-rebuild | no SD I/O on glyph miss; whole file already resident |
| Word measurement | `GfxRenderer::getTextAdvanceX` (per UTF-8 word, GfxRenderer.h:327) via `ParsedText::calculateWordWidths` | `ChapterLayout` internal shaping/measure per run; UAX#14 line breaking | bitmap path is word-callback based; engine is stream/SAX based |
| Page model | `Page` of `PageLine{TextBlock}` with selection groups, focus split, ruby, link spans | `Page{PageTextRun[], PageImage[], PageLink[]}` + `charStart` anchors | engine covers focus-reading word splits (per-word `markFocusWords`, ChapterLayout.cpp:498-523) and link spans (`PageLink`+`onAnchor`); **remaining gaps**: strikethrough, ruby, selection groups, synthetic-hyphen flag, drawn `<hr>` — itemized and gated in Phase 3.5 (§4) |
| Rendering | `TextBlock::render → GfxRenderer::drawText` in logical coords with orientation transform | `PageRenderer::renderText` writes panel-native 1bpp with `FrameRotation` | both target the same 48KB framebuffer; chrome can draw on top afterwards (§3.5) |
| Caching | `Section` `.bin` v46, per-spine, partial-build support | FIBP `s<N>-<hash8>.fibp`, streaming writer, partial suspend | coexistence during migration (§4 Phase 2) |
| Progress anchors | `Section` visible-text offset, `getPageForAnchor` | `charStart` + `pageForChar` / `charForAnchor` | `ProgressMapper` continues to work off spine + char offset |

### 2.4 Memory environment today (verified, and it changes the plan)

`platformio.ini` build matrix (§ references):

- `[env:default|gh_release|gh_release_rc|slim]` → `board = esp32-c3-devkitm-1` (:11), **no `BOARD_HAS_PSRAM`**. The Sticky comment (:265-268) states it explicitly: *"PSRAM is intentionally left off (48KB framebuffer fits in DRAM, same as X4)"* — the X4 (C3) ships PSRAM-less.
- `[env:sticky*]` → ESP32-S3, no PSRAM.
- `[env:x4pro|x4c|papermono*]` → ESP32-S3 + `BOARD_HAS_PSRAM` (:321, :365, :445), 8MB PSRAM (n16r8).
- `[env:x4pro_profile]` additionally sets `-DBOOK_PROFILE` (:347) for the profiling logs used across `EpubReaderActivity`.

Heap pressure data points: `~380KB` usable DRAM on C3 (project constraint), the wolfSSL memory comment cites *"the ~50KB free heap a reading session leaves"* (platformio.ini:60-62), and `EpubReaderActivity` gates builds at 32KB free / 16KB max-alloc (EpubReaderActivity.h:149-150). The `firmware_tuned` core rebuild reclaims ~32–37KB (:158-183).

**Consequence for this design:** the brief's "8 MB PSRAM on ESP32-C3" assumption is **not true for this repo's build matrix**. `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` returns `nullptr` on every C3 environment. **Per the round-2 directive (§3.3), the native-TTF feature is PSRAM-ONLY**: it is available on X4 Pro/X4C/Paper Mono (S3) and **unavailable on X4/Sticky**, where the built-in bitmap fallback chain is the only reader font and no font bytes are ever allocated.

---

## 3. Target Architecture

### 3.1 Overview

```
SD card                          RAM (BookFontLoader)                        Reader activity
─────────                        ─────────────────────                       ────────────────
/fonts/*.ttf|*.otf ──read──▶ PSRAM/heap font bytes (whole-file residency)
/fonts/free-fonts.json ─────▶ family/style map          ──▶ FontChain* ──▶ LayoutParams.font
                                                                │
/.crosspoint/<book>/...        TtfFont (stb) per face ◀─────────┘
  ficache/s<N>-<hash>.fibp ◀── PageCacheWriter ◀── ChapterLayout(SAX→UAX#14→Page)
  ficache/s<N>-<hash>.fibp ──▶ PageCacheReader ──▶ Page ──▶ PageRenderer::render ──▶ FrameTarget(fb)
                                                                  │
GfxRenderer (48KB fb, logical coords) ◀── status bar / toolbar / popups (chrome, drawn after)
```

Nothing else changes for the user: same `EpubReaderActivity` shell, same Text settings screens, same progress persistence.

### 3.2 `BookFontLoader` (new, `src/BookFontLoader.{h,cpp}`)

A singleton (mirrors `SdCardFontSystem`'s facade style) that owns the entire TTF lifecycle.

```cpp
class BookFontLoader {
 public:
  struct FontFaceInfo {          // one .ttf/.otf file
    char name[48];               // family display name (manifest or filename stem)
    char file[64];               // path under /fonts/
    uint8_t styleFlags;          // BookFont::StyleFlags this file provides
    uint32_t fileSize;
    uint32_t mtime;              // for fingerprinting
  };
  struct FamilyInfo {
    char name[48];
    uint8_t faceCount;           // up to 4: REGULAR/BOLD/ITALIC/BOLD_ITALIC
    FontFaceInfo faces[4];
    bool isBuiltinFallback;      // the BitmapBookFont chain
  };

  void begin();                  // scan /fonts/, load manifest, probe PSRAM
  void ensureLoaded();           // (re)load the active family if settings changed or registry dirty
                              // MUST be called before getReaderFont() or layoutGenerationHash()
  // The live reader chain (regular/bold/italic/bold-italic faces registered as available).
  // Never null: falls back to the built-in BitmapBookFont chain.
  book::FontChain* getReaderFont();
  uint32_t fontFingerprint() const;   // hash of loaded font bytes ⊕ chain->styleCoverage() — §3.4 (content-based, not path+timestamp)
  const FamilyInfo* families() const { return families_; }  // for the settings Font tab (bounded, see §7)
  uint8_t familyCount() const { return familyCount_; }      // capped at kMaxDiscoveredFamilies
  void markDirty();              // web upload / SD change (thread-safe, atomic flag)
  // Scrub arenas + unload file bytes when leaving the reader with low heap pressure
  void releaseResidentCaches();
 private:
  // Bounded storage for discovered families (no std::vector — bare new aborts
  // under -fno-exceptions; see FontFaceInfo for the 4-byte filename key).
  static constexpr uint8_t kMaxDiscoveredFamilies = 32;
  StaticArray<FamilyInfo, kMaxDiscoveredFamilies> families_;
  uint8_t familyCount_ = 0;
  // Per-face byte ownership: TtfFont::init borrows the complete font buffer,
  // which must remain resident while FontChain uses the face. faces_ owns
  // TtfFont instances; faceBytes_ owns the raw file buffers; release
  // destroys TtfFont before freeing its buffer.
  bool loadFaceBytes(const FontFaceInfo&);
  uint8_t faceBytesOwner_[4];   // 1 = BookFontLoader owns the buffer, 0 = borrowed
  // RAII owner with a no-op deleter (custom type, not a repository alias):
  // std::unique_ptr<uint8_t[], void(*)(const uint8_t*)> faceBytes_[4] with
  // [](const uint8_t*){}, matching the project rule of no bare `new` under
  // -fno-exceptions; buffers freed in the reverse of loadFaceBytes construction order.
  book::TtfFont* faces_[4];     // only active faces constructed; destroyed in onExit
  book::FontChain chain_;
  uint32_t fingerprint_ = 0;
  std::atomic<bool> dirty_{false};
};
};
extern BookFontLoader fontLoader;   // defined in main.cpp, beside sdFontSystem (main.cpp:55)
```

**Built-in fallback**: a singleton `book::FontChain` wrapping `BitmapBookFont` instances over the built-in bitmap font data (one `BitmapBookFont` per *style* — the type's metrics ignore `sizePx` but its `coverage_[64*64]` (FreeInkUIBookFont.h:86) is a 4KB mutable per-instance buffer: 4 styles = 16KB, accounted in the C3 budget as static BSS — this cost was missing from the 104KB arena figure and is added here). When `SETTINGS` selects a built-in family, or a TTF load fails, `getReaderFont()` returns this chain. This keeps the reader functional on day one with **zero** TTFs on the card, and gives the release builds a safety net.

### 3.3 Memory strategy (PSRAM-only, per round-2 directive)

> **P1 transitional (see §14.1):** on PSRAM-less builds this loader exists in the binary for the `CROSSPOINT_TTF_DEBUG` rig only; Phase 2 compile-outs it per §14.1. The fallback chain below is `BitmapBookFont` in the P1 interim; Phase 2 swaps it to `EpdBookFont` per §14.5.

**USER DIRECTIVE (round-2): the native-TTF feature is PSRAM-ONLY. There is no DRAM tier for font bytes.** On boards without `BOARD_HAS_PSRAM` (ESP32-C3 X4/Sticky) the feature is **not available**: `getReaderFont()` returns the built-in `BitmapBookFont` fallback chain (`BookFontLoader::builtinFallback()`, `src/BookFontLoader.cpp` — the P1a reference implementation), font families stay listed-but-unavailable, and `begin()` logs once "native TTF fonts unavailable (no PSRAM)". The former DRAM-tier machinery (framebuffer-loan `loadFaceBytes`, `initBudget`/`remainingBudget_`, `kMaxDramFontBytes`) is **REMOVED BY DIRECTIVE**; in the merged P1a loader that logic is legacy and will be deleted in Phase 2. PSRAM-less builds must never allocate font bytes.

On PSRAM boards (X4 Pro / X4C / Paper Mono, 8MB), font file bytes load via `poolMakeBytes` (`PoolBytes` → `poolMalloc` → `heap_caps_malloc(MALLOC_CAP_SPIRAM)`, `lib/Memory/Memory.h`) and stay **resident for the face's lifetime** — `TtfFont::init` borrows the buffer (stb_truetype holds pointers into it; TtfFont.h "must outlive the font"), so a transient load-and-release model is not an option. A per-face size guard (2MB) precedes the allocation (CWE-400). Presence is the discriminator at `begin()`:

```cpp
// Presence (total capacity, not free) is the discriminator.
// heap_caps_get_free_size(MALLOC_CAP_SPIRAM) returns currently free bytes —
// an S3 with low free PSRAM must not be treated as no-PSRAM.
// esp_psram_size() is 0 when no PSRAM is fitted.
const size_t psramTotal = esp_psram_size();  // 0 on C3/X4/Sticky
const bool nativeTtfAvailable = psramTotal >= 4u * 1024 * 1024;
```

Arena allocation always goes to PSRAM via `heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` (explicit capability — plain `malloc()` does not reliably place large blocks in PSRAM). On PSRAM-less boards no native-TTF arenas are ever allocated; the bitmap fallback needs none.

**Arena budgets** (caller-provided arenas; `Arena` never allocates internally). The C3/Sticky column is historical P1 sizing kept for reference only — the `CROSSPOINT_TTF_DEBUG` rig is PSRAM-only too (§14.1 compile-time split), so on PSRAM-less boards **no native-TTF arena is ever allocated** — layout scratch included — and the path is bitmap-fallback only per the directive:

| Arena | Purpose | C3 / Sticky (debug rig only) | X4 Pro / X4C / Paper Mono (PSRAM) |
|---|---|---|---|
| Font file bytes | `TtfFont::init` borrows the buffer for the face's lifetime | **n/a — native TTF unavailable (no PSRAM, directive §3.3)** | resident per loaded face in `PoolBytes` (`poolMakeBytes`, `heap_caps_malloc` in `MALLOC_CAP_SPIRAM`); 2MB per-face guard |
| `bookArena` | `Book::open` container data (catalog, toc, manifest) | 32KB | 512KB |
| `scratch` (layout) | `ChapterLayoutSession` block flow, line breaking, `Page` emission | **n/a — never allocated** (debug rig is PSRAM-only) | 256KB (PSRAM via `poolMakeBytes`; STANDARD profile fixture peak ~152KB — see §13 item 6) |
| `parseScratch` | inflate window + decompressor + XML | **4KB peak** during build ticks (loaned from framebuffer via `FrameBufferLoan`, not resident) | 46KB (drops to ~8KB resident when `chapterSource` extraction is used) |
| `glyphArena` (per active chain) | `TtfFont` direct-mapped advance+glyph cache | 24KB | 64KB |
| FIBP writer index arena | `PageCacheWriter::begin` index chunks (~1KB per 128 pages) | 8KB | 8KB |
| Engine parser heap (Expat) | transient `XmlSaxSession::open()` — `XmlSax.h:11` states Expat's internal pools use the **system allocator**, bounded to the chapter XML size | accounted as transient, ≤ 16KB measured on OPF/NCX | same |
| framebuffer | existing `GfxRenderer` 48KB single buffer | unchanged | unchanged |

Debug-rig steady state on PSRAM boards ≈ **256KB of PSRAM layout scratch** (`poolMakeBytes`) plus the 16KB static BSS for `BitmapBookFont` coverage buffers (4 styles × 4KB — P1a DRAM debt, deleted in Phase 2, kanban t_f7a102a2). Per the directive (§3.3), native-TTF arenas and resident font bytes exist **only on PSRAM boards**; PSRAM-less boards never allocate them and keep the bitmap fallback.

1. The reader **already** loans the 48KB framebuffer to builders via `GfxRenderer::releaseFrameBufferForBuild()` / `FrameBufferLoan` (GfxRenderer.h:405-424; used at EpubReaderActivity.cpp:1509). This idiom remains for the existing EPUB builder; the native-TTF path itself is PSRAM-only (directive, §3.3), so TTF layout windows on PSRAM boards draw `parseScratch` from PSRAM rather than the loaned region. Font bytes never use the loan — they are PSRAM-resident for the face lifetime.
2. Existing heap gates carry over verbatim: `buildTickHeapGate()` (EpubReaderActivity.cpp:471-476) pauses builds below `BACKGROUND_BUILD_MIN_FREE_HEAP` (32KB) / `BACKGROUND_BUILD_MIN_MAX_ALLOC` (16KB).
3. `-DFREEINK_BOOK_SMALL=1` on the C3 environments selects the engine's small capacities (advance slots 256 / glyph slots 64, `kMaxPages 4096`, `kMaxAnchors 192`), matching the ~200KB-free-heap class the profile was built for. S3 environments use the default (STANDARD) profile. **Flag name check: the draft's `-DFREEINK_BOOK_PROFILE=1` does not exist.**

**Font size gate (PSRAM-only):** with the DRAM tier removed by directive (§3.3), there is no `kMaxDramFontBytes` budget and no framebuffer-loan load path. The gate on the PSRAM tier is the 2MB per-face guard plus free-PSRAM availability at scan time; a face that fails keeps the family listed but greyed out in the Font tab (§12). stb_truetype requires the **font file addressable in memory** during `init()` (to build the `stbtt_fontinfo`) — satisfied by the PSRAM-resident `PoolBytes`, which must outlive the face. A streaming (FreeType) backend may later replace this behind the same `BookFont` interface (`freeink-sdk/docs/freeink-book.md`). **Malformed-font safety**: stb_truetype warns against untrusted fonts (offsets not range-checked). `TtfFont::init` only checks `len < 12` (TtfFont.cpp:36) — `BookFontLoader` must add a validation boundary before `init()`: basic `sfnt` header sanity (table directory bounds, `numTables` sanity), and on failure skip the face with a `LOG_ERR`. Phase 2 corpus includes 2 deliberately malformed fonts to verify the boundary.

**Glyph cache behavior note:** when the glyph arena fills, `TtfFont` flushes the entire cache and rebuilds (no LRU). At C3 sizes (24KB) on a book mixing scripts this can thrash; the mitigation is the same as today's `.cpfont` prewarm economics — one active size, per-page codepoint sets, and FIBP so cached pages skip **layout** but still **rasterize glyphs** on every display (cache hits call `PageRenderer::renderText`, not just reuse stored pixels; the glyph arena is not checkpointed in FIBP). `renderText`'s `fontFor` + `rasterize` hit the direct-mapped advance cache first. Measured via `Arena::highWater()` / `failedAllocSize()` logging in Phase 2 (§9). Note: a single glyph arena backs all faces in a chain — when `TtfFont::flushGlyphs()` calls `glyphArena_->release(glyphBase_)` (TtfFont.cpp:133), it rewinds to that face's init mark, invalidating glyphs cached by later-initialized faces. The initial design uses one shared arena; Phase 2 must verify that alternating styles does not trigger cross-face cache invalidation storms, with per-face arenas as the fix if it does.

### 3.4 Cache invalidation: FIBP + generation hash

Replace `Section`'s per-spine `.bin` (v46, header = `fontId + lineCompression + …`, validated at Section.cpp:195) with FIBP under the existing per-book cache dir:

```
.crosspoint/epub_<hash>/
  ficache/s3-1a2b3c4d.fibp     ← pageCacheName(spineIndex, generationHash)
  (sections/*.bin untouched during migration; deleted in Phase 4 sweep)
```

```cpp
book::LayoutParams p{...};
const uint32_t generation = book::layoutGenerationHash(p, fontLoader.fontFingerprint());
book::pageCacheName(spineIndex, generation, name, sizeof(name));   // "s3-1a2b3c4d.fibp"
```

Cache note: FIBP stores `PageTextRun` layout records, not rendered glyph bitmaps. Cache hits skip layout (`ChapterLayoutSession`) but **still call `PageRenderer::renderText`** to rasterize glyphs into the framebuffer. Glyph-cache eviction within the 24KB (C3) / 64KB (PSRAM) `glyphArena` can cause re-rasterization of evicted glyphs on cache-hit page display — the same economics as today's bitmap `FontCacheManager`. Phase 2 telemetry logs `Arena::highWater()` / `failedAllocSize()` per page to measure cache-hit rendering cost and glyph-cache flush frequency.

|`fontFingerprint()` | FNV-1a over each active face's **loaded font bytes** ⊕ `chain_.styleCoverage()`. Content-based identity (not path+timestamp): replacement files whose timestamp and size are preserved produce a different byte hash and correctly invalidate caches. `HalFile` exposes `size()` but no modification timestamp (`HalStorage.h:95-119`), so byte hashing is the robust choice. Adding a real bold file to a family that previously rendered synthetic-regular changes `styleCoverage()` → new fingerprint → stale caches rebuild. |

Mapping of the current `ReaderRenderSpec` fields into `LayoutParams`:

| ReaderRenderSpec (ReaderRenderSpec.h:13-24) | LayoutParams | Notes |
|---|---|---|
| `fontId` | `font` (`FontChain*`) | identity moves to the chain + fingerprint |
| `lineCompression` (0.95/1.0/1.1/1.2) | `lineSpacingPct` (95/100/110/120) | single source: `getReaderLineCompression()` (CrossPointSettings.cpp:323) |
| `extraParagraphSpacing` | `paragraphSpacingPct` (100 / 150) | 150 = the half-line gap the preview uses (TextSettingsPreview.cpp:88) |
| `paragraphAlignment` (JUSTIFIED..BOOK_STYLE) | `defaultAlign` (`CssTextAlign`; BOOK_STYLE→Justify per TextSettingsPreview.cpp:24-27) | |
| `viewportWidth/Height` | `pageWidth/Height` + margins (from `getOrientedViewableTRBL` + `screenMargin` + status-bar height, exactly as renderBook() computes today, EpubReaderActivity.cpp:1443-1464) | |
| `hyphenationEnabled` | `hyphenator` (engine `Hyphenator`, `hyph_en_us.h`) | |
| `embeddedStyle` | `embeddedStyles` | |
| `imageRendering` | post-layout image policy at `renderImages()` call sites | placeholder/suppress handled by the activity, not the engine |
| `focusReadingEnabled` | `focusReading` | engine-side per-word fixation bolding (`markFocusWords`, ChapterLayout.cpp:498-523) |
| `size` | `baseSizePx` | TTF: continuous; bitmap fallback chain: ignored by `BitmapBookFont` metrics |

Because the hash mixes the full `LayoutParams` plus the engine's layout version (automatic in `layoutGenerationHash`), **any** settings change, orientation change, or firmware layout change invalidates precisely and only the affected caches. `PageCacheReader::open` returns `Stale` on mismatch and the activity rebuilds — the exact semantics `Section::loadSectionFile` provides today.

### 3.5 Reader activity integration

`EpubReaderActivity` (navigation, progress, prefetch, toolbar, footnotes, dictionary, stats) is kept; only the *page source* is replaced. Concretely:

1. **New member state**: `book::Book book_`, arenas as members (allocated in `onEnter()`, freed in `onExit()` — activity-lifecycle rule), `book::PageCacheReader cacheReader_`, `book::ChapterLayoutSession session_` (optional, active during background builds), `book::PageCacheWriter writer_`.
2. **`renderBook()`** (EpubReaderActivity.cpp:1412-1781 today) keeps its viewport computation, then:
   - `loadSectionFile(renderSpec)` → `cacheReader_.open(StorageCache, name, generation, bookArena)`; `Stale` → rebuild path.
   - Cold path: `ChapterLayoutSession::begin(...)` + first `step()` to emit page 1 immediately (matches today's "build shows first page, indexes in background" UX), `PageCacheWriter` attached as the sink so pages persist as they are emitted; `writer_.suspend(bytesConsumed, bytesTotal)` **only at `onExit()`/session end** — NOT at each `step()` chunk end, because `PageCacheWriter::suspend()` commits the partial footer and calls `endWrite()`, making the writer closed to further `onPage()` calls. Reopening a partial FIBP accepts it as-is (`isPartial()`); resuming the build requires a fresh `ChapterLayoutSession` pass since the API has no append/resume (§7).
3. **Rendering**: after the (already) oriented `FrameTarget` is filled, page pixel output happens once per page via `PageRenderer::renderText(page, *fontLoader.getReaderFont(), target)` (+ `renderImages` when the page has images and `imageRendering == IMAGES_DISPLAY`). Then existing chrome draws over it: `renderStatusBar()`, toolbar overlays, popups — all through `GfxRenderer` in logical coordinates as today. Framebuffer is single-buffer (`EINK_DISPLAY_SINGLE_BUFFER_MODE=1`), and `PageRenderer` writes the same physical buffer `GfxRenderer` draws to; ordering (page first, chrome second) is preserved.
4. **Orientation mapping** (new `src/adapters/FrameTargetFactory.{h,cpp}`):

   | CrossPointSettings::ORIENTATION | GfxRenderer::Orientation | FrameTarget.rotation |
   |---|---|---|
   | PORTRAIT (0) | Portrait (480×800 logical) | `FrameRotation::Portrait` (90° CW) |
   | LANDSCAPE_CW (1) | LandscapeClockwise | `FrameRotation::UpsideDown` |
   | INVERTED (2) | PortraitInverted | `FrameRotation::PortraitInverted` |
   | LANDSCAPE_CCW (3) | LandscapeCounterClockwise (native) | `FrameRotation::None` |

   `FrameTarget{renderer.getFrameBuffer(), panelWidth=HalDisplay::DISPLAY_WIDTH, panelHeight, panelWidthBytes, format, rotation}` — same convention GfxRenderer already uses (1bpp MSB-first, SET=white). `format = SETTINGS.textAntiAliasing ? FrameFormat::Mono1Dithered : FrameFormat::Mono1Sharp` (settings row unchanged, CrossPointSettings.h:289, default ON). Parity note: the bitmap path renders 4-level gray edges via dual LSB/MSB plane RAM + the panel AA waveform (`lib/GfxRenderer/GrayPlanes.h` tone mapping → `GfxRenderer::displayGrayBuffer()`); FreeInkBook has no dual-plane `FrameFormat`, so TTF v1 pages use dithered 1bpp, with a no-engine-change 4-level parity path via `PagePaint` tracked in §5 D9 / §11 Q7.
5. **Position/progress**: `ProgressMapper`-visible offset ↔ `PageCacheReader::pageForChar(charOffset)`; TOC/anchor jumps via `charForAnchor(idHash)` (engine `onAnchor`); percent-restore keeps `pendingOffsetJump` semantics with char offsets instead of Section's visible-text offset. Position save format (`progress.bin`) gains a generation-tagged char offset; old progress entries degrade to "chapter start" (§7 error handling).
6. **Live settings changes**: `applyReaderTextSettings()` / `applyTextSettingLive()` (EpubReaderActivity.h:190-195) abort `session_` (`ChapterLayoutSession::abort()`), recompute `LayoutParams` + generation hash, reopen/rebuild — the FIBP hash mismatch does the invalidation; `pageForChar` restores the position. This is the mechanism behind "on-the-fly" (§1 goal 2).
7. **Prefetch** (`prefetchNextChapterDuringDisplay`, EpubReaderActivity.cpp:478-521): unchanged shape — open next spine's `PageCacheReader`, else start a `ChapterLayoutSession` behind the same `buildTickHeapGate()`.

**Feature-parity note (enforced, not hand-waved):** FIBP `PageTextRun` carries `text/len/x/baselineY/sizePx/styleFlags` only (ChapterLayout.h:56-63). The old TextBlock page model additionally carries selection groups (dictionary wrapped-word grouping, TextBlock.h:107), per-word focus splits, ruby (TextBlock.h:73), link spans (TextBlock.h:76), and synthetic-hyphen flags (TextBlock.h:111). Two of these the engine already covers — focus reading is per-word (`markFocusWords`, ChapterLayout.cpp:498-523; the earlier "paragraph-level `focusReading`" claim was wrong) and synthetic bold (PageRenderer.cpp:183-195) — and footnotes/links ride `PageLink` + `onAnchor`. The remaining deltas — strikethrough, ruby, selection groups, synthetic-hyphen flag, drawn `<hr>`, run-granular dictionary hit-testing, footnote list assembly, and the style-bit translation — are itemized with file:line evidence in **Phase 3.5 (§4)**, which gates Phase 4 deletion.

### 3.6 Settings model changes

`CrossPointSettings` additions (append-only; existing keys untouched):

```cpp
// Reader font engine selection
static constexpr uint8_t READER_ENGINE_BITMAP = 0;   // built-in / .cpfont (legacy, rollback path)
static constexpr uint8_t READER_ENGINE_TTF    = 1;
uint8_t readerFontEngine = READER_ENGINE_BITMAP;
char    ttfFontFamilyName[48] = "";                   // family display name under /fonts/
uint8_t ttfFontPointSize = 14;                        // continuous 8..72 pt
// (lineSpacing / paragraphAlignment / screenMargin / focusReading / hyphenation are shared
//  by both engines — no new keys for them)
```

`getReaderFontId()` (CrossPointSettings.cpp:407-433) is replaced by a resolver that returns a *font object*, not an id:

```cpp
// CrossPointSettings or BookFontLoader-facing helper
book::FontChain* getReaderFontChain() const {
  if (readerFontEngine == READER_ENGINE_TTF && ttfFontFamilyName[0] != '\0' &&
      fontLoader.isFamilyAvailable(ttfFontFamilyName)) {
    fontLoader.ensureLoaded();   // reload the active family before returning the chain
    return fontLoader.getReaderFont();          // active TTF chain
  }
  return fontLoader.builtinChain();             // BitmapBookFont chain, never null
}
```

The `ensureLoaded()` call is critical on the **live-settings path**: when the user selects family B while family A is still the loaded active chain, `fontFingerprint()` and `PageRenderer::renderText()` must use family B. Without `ensureLoaded()` the chain stays stale at family A, producing stale rendering and a matching-but-wrong FIBP generation hash (cache hit on stale content). If `fontFingerprint()` is changed to hash the already-loaded font bytes (see §8 R7), the stale-chain risk drops to a hash-only mismatch with a rebuild on reopen rather than a stale-render — both behaviors are safe, but the explicit reload is the simpler invariant.

The `sdFontIdResolver` indirection (CrossPointSettings.h:429-431) remains for the legacy bitmap path only; the TTF path deliberately does **not** reintroduce an id-resolution callback — it returns the chain directly, and cache keys never need it (`fontFingerprint()` does the identification).

Migration: `fromJson()` (CrossPointSettings.cpp has the NOTOSANS→ATKINSON_HN precedent at :269-274) maps any prior SD bitmap family to `READER_ENGINE_BITMAP` + `sdFontFamilyName`; nothing to migrate for TTF (new keys). `clearSdFontFamily()` (cpp:400-405) is mirrored by a `clearTtfFontFamily()` that also snaps `ttfFontPointSize` and persists in one write.

Size handling: `readerFontPointSizes()` (ReaderFontSizes.h:21) gains a TTF branch — continuous 8..72 for TTF families (UI renders a slider/list, §12), `BUILTIN_READER_POINT_SIZES` for the fallback chain; `snapToNearestPointSize` unchanged for bitmap.

Point→pixel conversion (the one place the two unit systems meet): `baseSizePx = roundf(pointSize * 150.0f / 72.0f)` — 150 DPI is the existing convention of the SD-font converter and the built-in UI sizes (`kUiFontSizes` SMALL=8pt / UI_10=10pt / UI_12=12pt, SdCardFontSystem.cpp:31-35, comment "at 150 DPI, matching the SD-font converter"). So 14pt → 29px `baseSizePx`. This keeps TTF optical sizes visually consistent with the bitmap families users already know.

**CJK in the reader chain (v1: none)**: Phase 4 removes `SdCardFontSystem` from the reader path, but retains it for UI CJK. No SD-`.cpfont` fallback is wired into the reader chain: there is no `SdCardBookFontAdapter`, and under directive §3.3/§14.4 the PSRAM-less device class never loads font bytes at all, so an SD-font CJK adapter would have no data source there. On PSRAM-less boards the reader chain is the `BitmapBookFont` fallback only (§14.1); CJK text renders only as far as the builtin bitmap families' coverage goes — their Atkinson HN glyph data already merges `NotoSansHebrew`/`NotoSansArabic` (see the `fontconvert.py` command lines in `lib/EpdFont/builtinFonts/*.h`), but **CJK ideographs are NOT covered** and render as missing glyphs. This is a documented v1 limitation (risk R2); the FreeType streaming upgrade behind the `BookFont` interface is the revisit path, not an `.cpfont` reader fallback.

### 3.7 Font manifest: scan + optional manifest (decision)

**Scanning is the source of truth; the manifest is metadata only.** Analysis:

- SD is user-writable; any manifest can rot (renamed/deleted files, manual copies). `SdCardFontRegistry::discover()` already proves scan-first works well in this codebase and drives `VISIBLE_BUILTIN_FONT_COUNT + N` UI lists (TextSettingsActivity.cpp:81-91).
- The engine cannot derive family/style identity from a bare file reliably — `TtfFont::init` exposes no name table API, and FreeInkBook does not parse `name` tables. So identity needs *either* a manifest or a filename convention.

Decision: `/fonts/` directory layout follows the authoritative §14.4 contract — one subfolder per family, reusing the SAME folders as the legacy bitmap registry, both roots (`/fonts` + hidden `/.fonts`), `.ttf`/`.otf` accepted only inside family folders. The details of the walk, filtering, and one-level depth are specified in §14.4 and are not repeated here. What §3.7 still contributes beyond §14.4:

- **`free-fonts.json` semantics**: the root-level manifest is *optional* and only enriches: `{"families":[{"name":"Literata","display":"Literata","faces":{"regular":"literata-regular.ttf","bold":"literata-bold.ttf","italic":"literata-italic.ttf","boldItalic":"literata-bolditalic.ttf"}}]}` — it may also pin display names and license strings for the future download UI. When manifest and disk disagree, disk wins and the stale entry is dropped with a `LOG_DBG`.
- **Disk-wins rule**: scanning is the source of truth (SD is user-writable; any manifest can rot); `SdCardFontRegistry::discover()` already proves scan-first works well in this codebase (TextSettingsActivity.cpp:81-91).
- **Style inference**: per-file style detection follows §14.4's priority table; the style-availability behavior below applies unchanged.

Style availability is honest: a family whose chain registers only `Regular` still works — `FontChain::fontFor(cp, StyleBold, &faceFlagsOut)` falls back to the regular face and reports the shortfall via `faceFlagsOut`; the fingerprint includes `styleCoverage()` so installing the real bold file later triggers correct re-layout (§3.4). `PageRenderer::renderText` already stroke-simulates the missing bold face: it double-strikes glyphs +1px when the selected face is not bold (PageRenderer.cpp:183-195, verified in this worktree), so bold needs no upstream ask; italic shortfall degrades to regular glyphs with no synthetic slant (§8 R7).

---

## 4. Implementation Plan

Phased, each ending in a verifiable gate. `pio run` (default C3 env) must pass at every phase boundary; S3 envs (`x4pro`, `sticky`) built at Phases 0, 2, 4.

**Phase 0 — Build wiring (engine compiles, unused)**
- `platformio.ini`: add `FreeInkBook=symlink://freeink-sdk/libs/book/FreeInkBook` to `lib_deps` (after `FreeInkUI`, :133).
- C3 envs (`default`, `gh_release`, `gh_release_rc`, `slim`): `-DFREEINK_BOOK_SMALL=1`. S3 envs: no profile flag (STANDARD). Sticky: SMALL as well (PSRAM-off).
- Gate: `pio run` links clean; `bookStatusName()` reachable in a `LOG_DBG`.

**Phase 1 — Font loading + storage adapters (no reader changes)**
- `src/BookFontLoader.{h,cpp}`: scan, manifest, PSRAM-only font-byte loading (round-2 directive: the P1a two-tier allocation is legacy and its DRAM machinery is deleted in Phase 2), `FontChain`, `fontFingerprint()`, builtin `BitmapBookFont` fallback.
- `src/adapters/SdCardBookSource.{h,cpp}`: `BookSource` over `HalFile` (`Storage.openFileForRead`, mutex-wrapped — SdFat must never be touched directly per AGENTS.md).
- `src/adapters/SdCardCacheStorage.{h,cpp}`: `CacheStorage` over `HalFile` with temp-file + rename `endWrite()` (torn-file safety the interface docs require).
- `src/adapters/FrameTargetFactory.{h,cpp}`: orientation → `FrameRotation` table (§3.5).
- Debug gate: a temporary settings-hidden activity renders one hard-coded chapter to the framebuffer with `ChapterLayout` + `PageRenderer` and displays it. Manual on-device check in all 4 orientations.
- Gate: `pio run`; on-device page render visible; heap delta logged via `ESP.getFreeHeap()`/`ESP.getFreePsram()` before/after.

**Phase 2 — Reader integration behind `-DCROSSPOINT_TTF_READER=1`**

> **Status: Phase 2a SHIPPED (2026-09-10).** Reader integration merged behind the
> flag on the nine PSRAM-class envs (see §14.1): `TtfBookRuntime`
> (catalog + `ChapterLayoutSession` + FIBP reader/writer + next-spine prefetch,
> all PSRAM arenas), the separate `renderBookTtf()` path, generation-tagged
> progress records (16-byte shape, load-side degrade — see
> `activities/reader/ProgressRecord.h` and docs/file-formats.md), and the
> Atkinson `EpdBookFont` fallback chain. The legacy `Section` path stays intact
> (kill switch: runtime open failure falls back to it).
>
> **Phase 2b handoff (not in 2a):** TextSettings TTF UX (family/size rows,
> §3.6 resolver), dictionary/footnote parity (word hit-testing, footnotes,
> links — surfaced as v1 losses in 2a), extract-to-stored chapterSource
> optimization (parse arena stays 64KB), CSS padding fold (§3.5 item 10),
> hyphenator wiring, `TxtReaderActivity` migration, chapter-time-left/stats
> parity polish. Flag stays OFF on C3/sticky; PSRAM-less binaries are
> byte-identical to `develop`.

- `EpubReaderActivity`: FIBP reader/writer paths, `ChapterLayoutSession`, progress mapping, prefetch — all inside `#if CROSSPOINT_TTF_READER` alongside the existing `Section` code (build-flag kill switch, §10).
- Night-mode + chrome-over-page ordering verified (§8 R8).
- Gate: full read of a Latin EPUB end-to-end on C3 with `LOG_LEVEL=2`: cache write → reopen hits cache → position restore → live font-size change re-flows in place.

**Phase 3 — Settings UI + manifest**
- `TextSettingsActivity`: Font tab TTF section, Size tab continuous mode, Style tab coverage honesty (§12); `TextSettingsPreview` re-routed through the TTF chain (preview can reuse `ChapterLayout` on a tiny synthetic source; `PreviewKey` gains engine+fingerprint fields).
- `CrossPointSettings` new keys + migration; `SdCardFontSystem` untouched.
- Gate: settings toggles do not leak (`onEnter`/`onExit` heap parity), all 4 orientations.

**Phase 3.5 — Feature-parity enforcement (blocking gate for Phase 4)**

`PageTextRun` carries `text/len/x/baselineY/sizePx/styleFlags` only (ChapterLayout.h:56-63); the old TextBlock page model additionally carries selection groups, per-word focus splits, ruby, link spans, and synthetic-hyphen flags. This phase itemizes every reader-text feature the EpdFont path supports, with verified anchors, and closes each one before Phase 4 deletion may start. Old-path anchors live in `lib/Epub/Epub/` (+ `src/activities/`); engine anchors in `freeink-sdk/libs/book/FreeInkBook/`.

*Engine-covered — verify visual equivalence on-device, then close:*

1. **Focus reading is per-word, not paragraph-level.** `markFocusWords` bolds each word's first ~45% with the same 1..9-codepoint clamp the old `ParsedText::addWord` applies to `focusBoundary` (ChapterLayout.cpp:498-523; bold prefix applied per word at :1224-1227), and layout splits runs at focus boundaries (:1505-1587) so the bold prefix is its own run — the earlier "paragraph-level `focusReading`" claim was wrong. Compare against the bitmap split render (TextBlock.cpp:237-253).
2. **Synthetic bold ships in the engine.** `PageRenderer::renderText` double-strikes glyphs +1px when the selected face lacks the requested bold (PageRenderer.cpp:183-195); §3.7/§5 D5/§8 R7 updated. Check stroke density at 12-18 pt; no synthetic italic exists (§8 R7).
3. **Underline.** Engine parses `text-decoration: underline` (Css.cpp:183-184; `vertical-align` :185, mapped ChapterLayout.cpp:957-962) and draws a per-run arm (PageRenderer.cpp:200-206); the old path merges the rule across words (TextBlock.cpp:195-198, `DecorationLineTracker`). The seam between adjacent runs is cosmetic — accept and note.
4. **Superscript/subscript.** Engine shifts baseline at layout time (−33%/+12% of sizePx, ChapterLayout.cpp:1728-1731); the old path shifts ±40%/25% of ascender with 50% glyph scale at draw time (TextBlock.cpp:140-310). Visual check at reader sizes; bit translation is item 9.
5. **Link/anchor substrate.** `PageLink{target, fragment, x, y, width, height}` emitted per span (ChapterLayout.cpp:1739-1749), `onAnchor(idHash, charStart)` (:792, :1882-1883), and `PageCacheReader::charForAnchor(idHash)` (PageCache.h:161) replace the old `Page::links`/`FootnoteEntry` substrate (Page.h:76-98).

*CrossPoint-side adapter work — no engine change, but blocking:*

6. **Dictionary word hit-testing.** `DictionaryWordSelectActivity.cpp` consumes per-word data — `wordText(i)` :153, absolute x from `block->wordXpos(i)` :160, `wordStyle(i)` :162, `selectionGroup(i)` :170, `hasSyntheticHyphen(i)` :171 (accessors TextBlock.h:107/:111), SLOP-4 `wordAt` hit-test :213-226, row navigation on `renderer.getLineHeight` :90, advance measurement via `renderer.getTextAdvanceX` :188-193. Engine runs are run-granular with no width field — the adapter tokenizes runs into words and re-measures via `FontChain::advance`/`kerning`. Gate: identical word selection on a shared test EPUB in both engines.
7. **Selection groups (wrapped-word reassembly).** Old `selectionGroup[]` (TextBlock.h:107) groups wrapped pieces of one logical word for `DictionarySelection::groupTokens` (DictionaryWordSelectActivity.cpp:199); engine runs carry no grouping key and expose no per-run char offset (only `Page.charStart`). Needs upstream per-run char offset (ask below) or a text-continuation adapter heuristic.
8. **Footnote list + labels.** The old parser emits per-page `FootnoteEntry{number, href}` from internal `<a href>` links (ChapterHtmlSlimParser.cpp:1270-1303 link detection, :1556-1582 label text, :1857-1872 entry construction; Page.h:80-98) consumed by the button-driven flow (EpubReaderActivity.cpp:797-813 Power shortcut → `navigateToHref`/`EpubReaderFootnotesActivity`, menu :1023, `currentPageFootnotes` move :1708; rect hit-testing via `linkAtPoint`, EpubReaderUtils.h:44-59, is currently dead code). Adapter: derive footnote entries from engine `PageLink`s with internal targets and recover the number labels from runs overlapping the link rect; `resolveFootnoteHref` maps the touched word to its containing `PageLink` (obsoletes the `normalizeMarker` match contract, :1537-1561).
9. **Style-bit translation table.** Old STRIKETHROUGH=8/SUP=16/SUB=32/RUBY_CONTINUE=64 (EpdFontFamily.h:10-21) collide with `StyleSuperscript=8`/`StyleSubscript=16` (BookFont.h:17-24); only BOLD=1/ITALIC=2/UNDERLINE=4 coincide (§5 D8 corrected). Explicit mapping with static_asserts on the coinciding bits.
10. **CSS padding fold.** Engine `Css.cpp` parses margins (:145-178) but has no `padding` property; the old `CssParser` stores paddings. Fold padding into margins when building `LayoutParams.stylesheet` (or upstream).
11. **Image pipeline parity.** Formats match (JPEG+PNG on both sides: `ImageDecoderFactory.cpp:14-42` vs pngle/tjpgd in `ImageRenderer.cpp`). Differences: the old path caches decoded `.pxc` per image on SD with a RAM chunk cache and a lazy-extract hook (ImageBlock.cpp:36-42, :111-114); the engine streams a fresh decode from the zip on every render. Measure per-page cost on image-heavy books; placeholder/polarity/suppress stay CrossPoint-side via `PageImage` geometry (§3.4 mapping).

*Genuine engine gaps — upstream ask + explicit v1-loss decision required (each blocks Phase 4):*

12. **Strikethrough.** No `StyleStrikethrough` bit (BookFont.h:17-24), no `line-through` parse (Css.cpp), no render arm; old path: `<s>/<del>/<strike>` (ChapterHtmlSlimParser.cpp:162) + CSS `line-through` (CssParser.cpp:445-454, :527) drawn as 2px rules merged across words (TextBlock.cpp:195-198). Upstream ask: style flag + underline-style arm + CSS.
13. **Synthetic-hyphen flag.** The engine bakes the line-break `-` into run text unflagged (ChapterLayout.cpp:1723-1726, `addHyphen`); dictionary lookups must strip it when joining wrapped segments (`logicalSegmentLength`, DictionaryWordSelectActivity.cpp:242). Upstream ask: flag on `PageTextRun` (or recoverable via per-run char offset).
14. **Drawn horizontal rule.** Engine `<hr>` emits blank space only (ChapterLayout.cpp:818-820); old `PageHorizontalRule` (Page.h:62-74) draws a rule line. Upstream ask: page element + renderer arm; else document the v1 loss.
15. **Ruby.** No annotation concept in the engine; the old path groups words via `RUBY_CONTINUE` + `rubyTexts` (TextBlock.h:73) with an `ascender/2` line lift and centered SUP-style text (TextBlock.cpp:140-310, `getRubyShift` TextBlock.h:113). Largest ask — resolution tracked in §11 Q3.

**Gate:** every item is closed (verified on-device), shipped as an adapter, or has a filed upstream ask plus an explicit, documented v1-loss decision in §11. The `CROSSPOINT_TTF_READER` default flip may proceed independently, but **Phase 4 deletion may not start until this checklist is green**.

**Phase 4 — Legacy reader path removal (blocked by Phase 3.5)**
- **Blocking gate:** deletion starts only when the Phase 3.5 checklist is fully green — engine-covered items verified on-device, adapters shipped, and every genuine engine gap either closed upstream or accepted as a documented v1 loss (§11).
- Flip `CROSSPOINT_TTF_READER` default on; soak; then delete: `lib/Epub/Epub/ParsedText.*`, `lib/Epub/Epub/blocks/`, `lib/Epub/Epub/Section.*`, the reader `Page` model, `SdCardFont` reader registration (`GfxRenderer::registerSdCardFont` reader call sites).
- **Not deleted in this phase** (still bitmap consumers): `GfxRenderer`/`EpdFont*` (UI chrome), `TxtReaderActivity` (migrate to `ChapterLayout::layoutPlainText` as its own step), `DictionaryDefinitionActivity`/`DictionaryWordSelectActivity` (render definitions via the same chain or keep on the builtin bitmap chain initially), `SdCardFontSystem` CJK UI fallbacks.
- Gate: `pio run` all C3+S3 envs; `pio check`; corpus regression (§9).

---

## 5. Key Technical Decisions

| # | Decision | Rationale (evidence-based) |
|---|---|---|
| 1 | **stb_truetype via `TtfFont`, not FreeType.** Whole-file residency in PSRAM only (directive §3.3 — no DRAM tier); 2MB per-face guard. | Zero new dependencies (engine vendors stb); FreeType streaming for multi-MB CJK is the documented upgrade path in `freeink-sdk/docs/freeink-book.md` behind the same `BookFont` interface — swapping later costs no call-site changes. |
| 2 | **PSRAM-only font-byte residency with a runtime PSRAM presence probe.** | `platformio.ini` proves C3 envs ship PSRAM-less (§2.4); `esp_psram_size()` (0 when no PSRAM — not `heap_caps_get_free_size`, which returns free bytes, see §3.3) is the availability discriminator; PSRAM-less boards get no TTF path at all per the directive. Plain `malloc` is not a PSRAM strategy. |
| 3 | **FIBP + `layoutGenerationHash(params, fontFingerprint)` replaces the Section `.bin` cache.** | Generalizes the proven `spec`-validation design (Section.cpp:195) to settings the engine owns; partial-file suspend matches the existing partial-`.bin` UX; `pageForChar`/`charForAnchor` cover progress restore without a parallel index. |
| 4 | **Keep `GfxRenderer` + EpdFont for UI chrome.** | ~80 global font objects and every menu/settings/popup draw path are id-currency; rewriting chrome is high-risk/zero reader value. `BitmapBookFont` bridges the two worlds for the reader's fallback chain. |
| 5 | **Engine consumed as submodule symlink, unmodified except gated extensions.** | `platformio.ini` already uses this pattern for FreeInkUI (:133). Engine-side changes stay narrow and flagged upstream: (a) letter-spacing field on `LayoutParams` (§5.7); (b) the Phase 3.5 parity asks — strikethrough flag+arm+CSS, per-run char offset, synthetic-hyphen flag, drawn `<hr>`, ruby. Synthetic bold needs no ask: `PageRenderer::renderText` already double-strikes +1px on a style shortfall (PageRenderer.cpp:183-195). |
| 6 | **Scan `/fonts/` for discovery; optional `free-fonts.json` for metadata.** | Registry-rot analysis (§3.7); mirrors `SdCardFontRegistry::discover()`; filename convention supplies the style map the engine cannot read from the font itself. |
| 7 | **Letter spacing: engine change, not adapter.** | `LayoutParams` (ChapterLayout.h) has `lineSpacingPct`/`paragraphSpacingPct` but no tracking field. Adding `int8_t letterSpacingPct` (or px) upstream is the correct place — a post-hoc x-shift adapter would break justification and kerning. If upstream declines, the UI ships without the control (§11). |
| 8 | **Style mapping is a translation table, not a cast.** | Only BOLD=1/ITALIC=2/UNDERLINE=4 coincide between `EpdFontFamily::Style` (EpdFontFamily.h:10-21; plus STRIKETHROUGH=8, SUP=16, SUB=32, RUBY_CONTINUE=64) and `BookFont::StyleFlags` (BookFont.h:17-24; `StyleSuperscript=8`, `StyleSubscript=16` collide with the old STRIKETHROUGH/SUP bits). A naive cast would render strikethrough text as superscript. The adapter maps explicitly, with static_asserts on the coinciding bits (Phase 3.5.9). |
| 9 | **`textAntiAliasing` maps to `FrameFormat::Mono1Dithered` (ON) / `Mono1Sharp` (OFF) in v1; dual-plane 4-level gray parity is a CrossPoint-side pipeline change — the engine needs no modification.** | The engine rasterizes true 8-bit glyph coverage (`stbtt_MakeGlyphBitmap`, TtfFont.cpp:165; `GlyphBitmap.pixels` is "w*h, 8-bit coverage", BookFont.h:26-27) and consumes it per `FrameFormat` in `inkPixel` (PageRenderer.cpp:68-95): dithered-contrast 1bpp (default; `kInkSolid=140`/`kInkFloor=40` contrast curve + Bayer, "stems stay black, edges stay smooth"), hard-threshold 1bpp (`kInkThreshold=120`, PageRenderer.cpp:14), or Gray8 (`lum = 255 - coverage`, min-composed). `SETTINGS.textAntiAliasing` (CrossPointSettings.h:289, default ON) keeps its existing Style-row toggle (TextSettingsActivity.h:33) with real hardware meaning. The bitmap reader's 4-level gray — 2-bit glyph tones through `lib/GfxRenderer/GrayPlanes.h` (`setMsb`/`setLsb`, tones 0=white..3=black) into dual LSB/MSB plane RAM + the panel AA waveform (`GfxRenderer::displayGrayBuffer()`, GfxRenderer.h:372 → HalDisplay → panel driver; UC8279 XTF_AA LUT bank and SSD1677 external AA LUT — both drivers expose `supportsStripGrayscale()=true` + `writeGrayscalePlaneStrip`, Uc8279Driver.h:51-60, Ssd1677Driver.h:87-93) — has no engine `FrameFormat` equivalent, but does not need one: **the AA signal (8-bit coverage) is already native in the engine; only the last-mile plane packing is CrossPoint-side** (§11 Q7). Trade-off of the parity path: a second glyph walk per page — the bitmap path also walks glyphs twice (BW pass + gray pass, EpubReaderActivity.cpp:1892-1907); TTF re-`rasterize()` hits the glyph arena (CPU-only) unless it flushes mid-page (R4). Gray8 as a page-wide format stays out of scope. |

---

## 6. Files to Create / Modify / Delete

**Create**

| File | Purpose |
|---|---|
| `src/BookFontLoader.{h,cpp}` | TTF discovery, PSRAM-only loading (§3.3), `FontChain` ownership, fingerprints (§3.2) |
| `src/adapters/SdCardBookSource.{h,cpp}` | `book::BookSource` over `HalFile` |
| `src/adapters/SdCardCacheStorage.{h,cpp}` | `book::CacheStorage` over `HalFile`, temp+rename |
| `src/adapters/FrameTargetFactory.{h,cpp}` | `GfxRenderer` state → `book::FrameTarget` (§3.5 table) |
| `src/adapters/PagePaint.{h,cpp}` (Phase 2) | page→framebuffer paint + night-mode inversion + chrome-over-page ordering helpers |

**Modify**

| File | Change |
|---|---|
| `platformio.ini` | `lib_deps` += FreeInkBook symlink; `-DFREEINK_BOOK_SMALL=1` on C3 envs; Phase 2 `-DCROSSPOINT_TTF_READER=1` |
| `src/CrossPointSettings.{h,cpp}` | new keys (§3.6), `getReaderFontChain()`, `clearTtfFontFamily()`, `fromJson` migration |
| `src/BookFontLoader` bootstrap in `src/main.cpp` | construct + `fontLoader.begin()` beside `sdFontSystem.begin(renderer)` (:559) |
| `src/ReaderFontSizes.h` | TTF continuous size branch in `readerFontPointSizes()` |
| `src/activities/reader/EpubReaderActivity.{h,cpp}` | FIBP/session integration behind build flag (§3.5) |
| `src/activities/reader/ReaderActivity.cpp` | `fontLoader.ensureLoaded()` next to `sdFontSystem.ensureLoaded(renderer)` (:58) |
| `src/activities/settings/TextSettingsActivity.{h,cpp}` | TTF Font/Size/Style rows (§12) |
| `src/activities/settings/TextSettingsPreview.{h,cpp}` | preview through the active chain; `PreviewKey` + fingerprint |
| `src/network/CrossPointWebServer.cpp` | settings schema gains the new keys (web editor list at :1163/:1267) |

**Delete (Phase 4, reader path only)**

| Path | Why safe |
|---|---|
| `lib/Epub/Epub/ParsedText.{h,cpp}` | only reader layout + preview + TxtReader use it; preview migrated (Phase 3), TxtReader migrated before deletion (§4 Phase 4 note) |
| `lib/Epub/Epub/blocks/` | TextBlock/BlockStyle consumers are the reader path |
| `lib/Epub/Epub/Section.{h,cpp}`, reader `Page`/`PageLine` model (`lib/Epub/Epub/Page.{h,cpp}`) | replaced by FIBP; **only after** the Phase 3.5 parity checklist is green and `TxtReaderActivity` + dictionary flows are off them |

`lib/EpdFont/`, `lib/GfxRenderer/`, `lib/Epub/Epub.{h,cpp}` (container/metadata), `src/SdCardFontSystem.*` are **retained**.

---

## 7. Error Handling

Aligned with the project pattern hierarchy (LOG_ERR + return false dominant; `assert` only for impossible states; no exceptions/`abort()`):

| Failure | Detection | Response |
|---|---|---|
| `heap_caps_malloc`/`makeUniqueNoThrow` OOM (arena or font bytes) | `nullptr` | `LOG_ERR("BFNT", "OOM: %u bytes (tier=%s)", ...)`; fall back to the builtin `BitmapBookFont` chain; reader stays functional |
| Font file too large / PSRAM exhausted | 2MB per-face guard or free-PSRAM unavailable at scan time (PSRAM-only, §3.3) | family listed but disabled in UI with a size hint; selection refused |
| `TtfFont::init` failure (corrupt/unsupported) | `ready() == false` | family dropped from the active list, `LOG_ERR`; fall back |
| Arena exhaustion mid-layout | `Arena::alloc` `nullptr` / `failedAllocSize()` | `ChapterLayout` returns `OutOfMemory` → activity shows the existing build-error popup (the `showBuildError` lambda inside `renderBook()`, EpubReaderActivity.cpp:1427), keeps prior page displayed |
| Missing glyphs (CJK in a Latin-only chain) | `renderText` `firstMissingOut` / return count | logged once per page (`LOG_DBG("BFNT", "missing=%u first=U+%04X", ...)`); visible gaps like today's tofu, no crash |
| Cache stale | `PageCacheReader::open` → `Stale` | silent rebuild (normal settings-change path) |
| Torn cache file (power loss mid-write) | `CacheStorage` temp+rename on `endWrite()`; hard power cut never reaches `endWrite()` or the rename | A hard power cut during a cold build leaves a temp file with no committed footer and no final-name alias — `PageCacheReader` cannot accept or resume it. The reader **retains the previous final cache** (if one exists for that generation) and falls back to a fresh `ChapterLayoutSession` rebuild; the orphaned temp file is left in `ficache/` and reaped on the next successful `endWrite()` (the activity deletes stale temps older than 7 days on `onEnter`). A full power-loss-resume checkpoint (mid-build state in flash + append/resume in `PageCacheWriter`) is out of scope — the API has no append/resume operation. (§3.5 suspend is end-of-session only.) |
| SD removed mid-read | `BookSource::readAt < 0` / `HalFile` open failure | `BookStatus::IoError` → activity error screen; `Storage` mutex discipline keeps SdFat state consistent |
| Legacy progress entry (pre-char-offset) | missing generation tag in `progress.bin` | restore to chapter start + keep page-0 fallback; never fail the open |

All allocations go through `makeUniqueNoThrow`/`heap_caps_malloc` with explicit `LOG_ERR` on failure (project rule: bare `new` aborts under `-fno-exceptions`). `FsFile` locals need no `close()` (`DESTRUCTOR_CLOSES_FILE=1`), but member/handle files are closed before `Storage.remove()` per the documented exceptions.

---

## 8. Risk & Mitigation

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | **C3 PSRAM-less budget insufficient** (arenas + transient Expat heap + activity state + stacks) | Medium | High | **Resolved by directive (§3.3)**: native TTF is unavailable on PSRAM-less boards — no font bytes and no TTF arenas are ever allocated there; the reader uses the built-in bitmap fallback chain, so the historical 32KB/16KB heap gates are the only C3 cost. The legacy DRAM budget machinery in the merged P1a loader is deleted in Phase 2. On PSRAM boards the SMALL/default profile and FIBP (cached pages skip **layout**, still rasterize glyphs — see R4) carry over. |
| R2 | **CJK/very large TTF cannot meet the DRAM-tier loading path** on C3 | High (eventually) | Medium | **Resolved by directive (§3.3)**: no DRAM tier exists, so C3 never attempts TTF loading — CJK/very-large TTFs are readable only on PSRAM boards (subject to the 2MB guard and free-PSRAM availability), while PSRAM-less boards keep the `BitmapBookFont` fallback chain (§14.1). FreeType streaming upgrade documented behind `BookFont` for the PSRAM tier. **PSRAM-less CJK is a documented v1 limitation**: the reader chain has no SD-`.cpfont` CJK fallback (§3.6), and the builtin bitmap families cover Latin/Greek/Cyrillic plus Hebrew/Arabic (merged NotoSansHebrew/NotoSansArabic glyph data) but NOT CJK ideographs. |
| R3 | **Feature-parity gaps block the migration** (strikethrough, ruby, selection groups, synthetic-hyphen flag, drawn `<hr>`, run-granular dictionary hit-testing, footnote list assembly, style-bit translation) | Certain — gap inventory verified (Phase 3.5) | High | **Blocking, not deferrable**: Phase 4 deletion cannot start until the Phase 3.5 checklist is green. Engine already covers focus reading per-word (ChapterLayout.cpp:498-523) and synthetic bold (PageRenderer.cpp:183-195); the rest closes via CrossPoint adapters where the engine substrate suffices (dictionary hit-testing, footnote list, style-bit table, CSS padding fold) and via upstream asks where it does not (strikethrough, ruby, selection-group keys, hyphen flag, drawn rule). Anything left open must be an explicit, documented v1 loss — no silent drop. |
| R4 | **Glyph-arena thrash** (flush-and-rebuild, no LRU) on mixed-script pages at C3 sizes | Medium | Medium | FIBP skips layout on cache hits, but cached pages still rasterize glyphs (§3.3). Monitor `Arena::highWater()/failedAllocSize()` in Phase 2 logs; the one-shared-arena design (TtfFont.cpp:133 `flushGlyphs` — see §3.3) may need per-face arenas if alternating-style tests show cross-face invalidation storms; raise `glyphArena` budget if logs show rebuild storms. |
| R5 | **Cache-version migration**: old `.bin` sections + new `.fibp` coexist | Certain (transition) | Low | Separate `ficache/` dir; old files simply go stale and are swept in Phase 4; `docs/file-formats.md` gains the FIBP chapter when Phase 2 lands. |
| R6 | **Night mode / screen inversion interaction**: `PageRenderer` writes SET=white; `SETTINGS.screenInverted` inverts at display time per ActivityManager | Medium | Medium | Verified in Phase 2 gate; if the panel-level invert cannot cover the page region, `PagePaint` inverts the page rect post-render (48KB bitwise pass) before chrome draws. |
| R7 | **Synthetic style rendering**: missing bold/italic faces degrade | Medium | Low | `styleCoverage()` in fingerprint keeps caches correct; synthetic bold is engine-native (double-strike, PageRenderer.cpp:183-195); italic shortfall degrades to regular glyphs with no synthetic slant — labeled honestly in the UI (§12). |
| R8 | **Engine unproven on C3 single-core RISC-V @160MHz** | Medium | High | Phase 1 debug activity + Phase 2 flag-gated soak; per-page timing logs alongside the existing `BOOK_PROFILE` phase logs (EpubReaderActivity pattern, :480-518). |
| R9 | **AA parity regression vs the bitmap path**: TTF v1 pages render 1bpp dithered edges; the bitmap path draws true 4-level gray (dual-plane) when `textAntiAliasing` is on | Certain (v1) | Low | `Mono1Dithered` is the engine's e-paper-tuned default (contrast curve keeps stems solid, edges dithered, PageRenderer.cpp:16-23); accept for v1. The §11 Q7 parity path (PagePaint coverage walk → 2-bit quantization → the reader's existing dual-plane strip pipeline) closes the gap with no engine changes — the engine's 8-bit coverage is already the full AA signal. Phase 2 soak confirms the second glyph walk is affordable on C3 before the parity pass ships. OFF → `Mono1Sharp` keeps the settings row honest for users who prefer hard edges. |

---

## 9. Testing Strategy

**Build matrix (CI):** `pio run -e default`, `-e gh_release`, `-e x4pro`, `-e sticky` at Phases 0/2/4; `pio check` (cppcheck config already excludes `freeink-sdk/*`, platformio.ini:28).

**On-device probes (human tester scope, flagged per AGENTS.md):**

1. *Heap telemetry*: log `ESP.getFreeHeap()`, `ESP.getMaxAllocHeap()`, `ESP.getFreePsram()`, and each `Arena`'s `used()/highWater()/failedAllocSize()` at: activity enter, cache hit, cold-build start/end, settings change, exit. Regression rule: no phase leaves the reader with <24KB free heap (the existing `RENDER_MIN_FREE_HEAP`).
2. *Cache invalidation matrix*: for each of {family, size, lineSpacing, alignment, margin, orientation, style-file install, firmware relayout} → expect exactly the affected spines' `.fibp` to go stale (`Stale` log) and rebuild, others untouched.
3. *Corpus*: Latin serif (Liberation/Literata ~400KB), Latin sans (Atkinson TTF), small font (well under the 2MB PSRAM guard), oversized font (700KB: accepted on X4 Pro; PSRAM-less boards run the bitmap fallback per directive §3.3), CJK font (expect the 2MB guard to reject it on PSRAM boards; PSRAM-less boards run the `BitmapBookFont` fallback per directive §3.3, which does NOT cover CJK ideographs — missing glyphs are the expected v1 result), Arabic text (engine `arab_shaping` — visual check only), .txt book (`layoutPlainText`).
4. *Settings soak*: 50 rapid font/size toggles in-session; verify no heap drift, session never leaves the book, position preserved within ±1 paragraph.
5. *Power-loss*: cut power during a cold build; reopen → if a prior final cache exists for the generation, use it (no crash); else rebuild from scratch. Orphaned temp file reaped on next `onEnter`. Full mid-build checkpoint/resume is out of scope (§7).
6. *Orientations*: all 4 modes × {page render, status bar overlay, toolbar overlay, dictionary popup} — verifies the `FrameRotation` table (§3.5) against `GfxRenderer` chrome.

**Unit-ish checks (host, where practical):** `fontFingerprint()` stability/instability properties; `pageCacheName` formatting; pt↔px conversion table snapshot.

---

## 10. Rollback Plan

- **Kill switch**: the entire reader integration lives behind `-DCROSSPOINT_TTF_READER=1` (Phase 2-4). Default builds until Phase 4 flip it; `readerFontEngine` settings default is `READER_ENGINE_BITMAP`, so even a TTF-era binary falls back to the intact bitmap path when the flag is off.
- **Settings**: new keys are append-only with bitmap defaults; `fromJson` ignores unknown keys on downgrade. No legacy key is repurposed (the NOTOSANS→ATKINSON_HN style migration precedent, CrossPointSettings.cpp:269-274).
- **Cache**: FIBP lives in its own `ficache/` directory. A rollback build ignores it completely and keeps using `.bin` sections — no cache invalidation storm, no disk sweep needed.
- **SdCardFontSystem**: untouched through Phase 3, so `.cpfont` users lose nothing until Phase 4; even then the UI fallback path stays until the UI-migration follow-up (§11 Q4).
- **Engine**: symlink lib_dep — removing it from `lib_deps` + the two flag lines returns the tree to the pre-TTF build.

---

## 11. Open Questions

1. **TTF directory & acquisition**: `/fonts/` confirmed as the scan root; should `FontDownloadActivity` (which today serves `.cpfont` builds via `FONT_MANIFEST_URL`, see SdCardFont.h:12-14) grow a TTF source, or stay bitmap-only until the UI migration? Leans: bitmap-only first.
2. **`LayoutParams` letter-spacing**: is upstream FreeInkBook willing to take the field (§5.7), or does the Layout tab ship without tracking in v1?
3. **Ruby + per-word focus splits — RESOLVED into the Phase 3.5 gate; "defer" is no longer an available answer.** Focus splits are engine-covered per-word (`markFocusWords` bolds each word's first ~45%, same 1..9-codepoint clamp as `ParsedText::addWord`, ChapterLayout.cpp:498-523, runs split at focus boundaries :1505-1587) — the earlier "paragraph-level `focusReading` interim" framing was wrong and is corrected in §2.3/§3.5/§12. Ruby has no engine counterpart at all (old: `rubyTexts` + `RUBY_CONTINUE` groups + `ascender/2` line lift, TextBlock.cpp:140-310, TextBlock.h:73/:113) and is the largest remaining engine gap: it must be either implemented upstream before Phase 4 or explicitly accepted as a documented v1 loss with the upstream ask filed (Phase 3.5 item 15).
4. **UI chrome TTF migration timing**: keep bitmap chrome indefinitely, or follow-up phase binds `TtfGlyphSource` via FreeInkUI's `DisplayTarget::setGlyphFallback`? (Cost: the ~4KB-per-instance coverage buffers and a sizing policy.)
5. **Dictionary/Txt migration order**: migrate `TxtReaderActivity` to `layoutPlainText` and dictionary rendering to the chain **before** Phase 4 deletion (recommended), or hold Phase 4 until both are done?
6. **Exact C3 arena floor**: *Superseded by the round-2 directive (§3.3) — native TTF (and its arenas) never allocates on PSRAM-less boards; the reader path there is bitmap-fallback only, and the `CROSSPOINT_TTF_DEBUG` rig is PSRAM-only as well (no DRAM scratch). The engine docs' STANDARD-profile `layoutPlainText` fixture peak is ~152KB (freeink-book.md "Memory profiles"; Standard `kParTextCap`=8192×3 + span/run/image/link/rule/line arrays + `styleText` 12KB + CSS rule table + page arena 24KB + 4KB read buffer) — covered by the rig's 256KB PSRAM scratch with headroom.*
7. **AA format for TTF pages**: v1 ships `Mono1Dithered`/`Mono1Sharp` (§5 D9). **Investigation conclusion (verified against this worktree): the engine already supports AA natively** — `stbtt_MakeGlyphBitmap` produces true 8-bit coverage (TtfFont.cpp:165; `GlyphBitmap.pixels`, BookFont.h:26-27) and `inkPixel` consumes it for all three `FrameFormat`s including Gray8 (PageRenderer.cpp:68-95). The only gap is last-mile plane packing: no `FrameFormat` emits CrossPoint's dual LSB/MSB 1bpp planes. That packing is CrossPoint-side — **no engine change is required**; the upstream dual-plane `FrameFormat` ask is downgraded to optional (it would merely let `PageRenderer::renderText` emit planes directly, saving PagePaint's run-loop replication). Remaining question: does the panel's 4-level AA waveform (UC8279 XTF_AA LUT bank, SSD1677 external AA LUT + dual DTM planes) visibly beat dithered coverage at 12–18 pt reader sizes on X4/X3 panels? If not, `Mono1Dithered` is final and the parity pass is dropped.

**Concrete fix path (no engine change required), corrected mechanics.** CrossPoint's 4-level gray lives in `GfxRenderer`'s `grayplanes` system (`GrayPlanes.h`; `renderCharImpl` GRAYSCALE_DUAL at GfxRenderer.cpp:540-545), driven by 2-bit glyph tones from `EpdFont`, and the reader's tiled gray pass (`beginStripTarget` 80-row bands, EpubReaderActivity.cpp:1982-2012) flags plane bands via `drawGrayDualPixel` (GfxRenderer.cpp:1715-1737). The naive formulation — "call `renderText` with `FrameFormat::Gray8` into a `beginStripTarget` band" — does **not** work as written, for two reasons: (i) `beginStripTarget` bands are 1bpp plane strips (`panelWidthBytes` per row — declaration + geometry doc GfxRenderer.h:231-239, strip state members :74-88), not the 8bpp geometry Gray8 needs (`width` bytes per row, PageRenderer.h:48) — and `PageRenderer` writes through its own `FrameTarget`, never through `GfxRenderer`'s strip state; (ii) `FrameTarget` has no band window — `toPanel` bounds-checks against full logical dims (PageRenderer.cpp:32-58), so `renderText` cannot clip to a band, and pointing it at a band-sized buffer would write runs above the band out of bounds. Two viable CrossPoint-side constructions:

   - **(a) Direct coverage walk (recommended).** `PagePaint` replicates `renderText`'s ~45-line glyph loop (PageRenderer.cpp:165-209 — UTF-8 decode → `FontChain::fontFor` → `rasterize` → `advance`/`kerning`; the font calls are all public engine API — two caveats: the engine's UTF-8 decoder `decodeUtf8` is translation-unit-local (PageRenderer.cpp:97-118), so `PagePaint` carries its own ~20-line len-bounded decoder, and the synthetic-bold double-strike + underline arms at :183-206 must be replicated for visual parity), quantizes each coverage sample inline (≥144→3, ≥96→2, ≥48→1, else 0 — the exact banding the `.cpfont` converters use to produce the bitmap path's 2-bit tones: `bm = coverage >> 4`, then bm≥9→3, bm≥6→2, bm≥3→1, anchored to the engine's `kInkSolid=140`/`kInkFloor=40` contrast curve, fontconvert.py:361-390 / fontconvert_sdcard.py:673-689; these bands are what the `grayplanes` tone semantics 0=white..3=black encode), and flags the existing dual plane bands via `grayplanes::setMsb`/`setLsb` + `renderer.drawGrayDualPixel` inside the reader's existing `beginStripTarget(lsb, y, rows, msb)` walk, with band culling mirroring `glyphIntersectsStrip` (GfxRenderer.cpp:1739). Zero additional large buffers, no Gray8 intermediate, and the reader's downstream pipeline (`writeGrayscalePlaneStrip`, `displayGrayBuffer`, cleanup) is reused unchanged. (`grayplanes::planBlock` is *not* involved — it serves only the 2×2-downsampled super/subscript/ruby path, which TTF text runs don't use.) Because `glyphIntersectsStrip` and `drawGrayDualPixel` rotate logical→physical per glyph/pixel (GfxRenderer.cpp:1721, 1747-1748), (a) is orientation-agnostic, exactly like the bitmap path.
   - **(b) Band-clipped sub-page + Gray8 intermediate.** `PagePaint` shallow-copies the `Page`'s run array, keeps runs intersecting the band, translates `baselineY` by −y0 (`Page`/`PageTextRun` are PODs), and calls stock `renderText` with `FrameFormat::Gray8` into a *separately allocated* 8bpp band buffer (480×80 = 38.4KB portrait / 800×80 = 64KB landscape transient — heap-gated on C3, or 40-row bands at half that), then walks the buffer quantizing to tones and flagging planes as in (a). Costs the extra band buffer; keeps the engine's run loop verbatim (kerning, synthetic-bold double-strike, underline handling included). **Rotation limitation:** as written, (b) is coherent only for the 0°/180° orientations (`FrameRotation::None`/`UpsideDown` = LANDSCAPE_CCW/LANDSCAPE_CW), where logical-horizontal bands stay physical-horizontal. Under the 90°-rotated orientations (`Portrait`/`PortraitInverted` = PORTRAIT/INVERTED — 2 of 4 modes), a physical row band maps to a logical *x-column* range, so baselineY culling/translation is the wrong axis; x-axis culling needs each run's advance+kerning re-walked (`PageTextRun` stores only the run start x, no width — ChapterLayout.h:56-63), which replicates the very loop (b) exists to avoid, and the band-buffer geometry above no longer matches the physical plane bands. This is what makes (a) the recommended path.

   Either way, the **BW base for AA pages must plot every pixel the planes can flag**: the bitmap base plots any nonzero tone as solid black (GfxRenderer.cpp:527-531). The matching TTF base rule is "coverage ≥ 48 → black" (the tone-1 boundary) — *not* stock `Mono1Sharp` (threshold 120) or `Mono1Dithered` (floor 40 + dithered mid-band), which would leave plane-flagged pixels without black base underneath. Construction (a) gets this for free by emitting base and planes from the same walk; (b) needs a base pass with the 48 threshold (a PagePaint walk, or an engine threshold parameter — the only remaining optional upstream nicety). Tone 3 never sets plane bits (the BW base carries it), matching `GrayPlanes.h`. Trade-off: a second glyph walk per page — the bitmap path also walks glyphs twice (BW pass + gray pass; tiled path: 1 + nBands culled walks, EpubReaderActivity.cpp:1892-1907); TTF re-`rasterize()` hits the glyph arena (CPU-only, no SD I/O) unless it flushes mid-page (R4). Phase 2 soak measures whether the extra engine work is acceptable on C3.

---

## 12. UI Wireframes

Current screens (from `TextSettingsActivity` code and the three provided screenshots: Font tab, Size tab, Layout tab): a 4-position tab bar `Font | Size | Layout | Style` (UiTabListActivity idiom), a row list under the tab, and a persistent live preview pane at the bottom labeled `Preview "<family>, <size pt>"` (TextSettingsPreview.cpp:72).

**Font tab (Tab::Family)** — today: `Noto Serif`, `Atkinson Hyperlegible Next` (`VISIBLE_BUILTIN_FONT_COUNT = 2`, TextSettingsActivity.cpp:37/81), then registry families appended into the same flat `fonts_` list (no section header, TextSettingsActivity.cpp:86-92). Proposed: keep all of it, append a `TTF Fonts` section listing `BookFontLoader::families()`:

```
┌ Font ─ Size ─ Layout ─ Style ┐
│  Noto Serif                  │
│  Atkinson Hyperlegible Next  │
│  SD CARD FONTS               │
│  Bookerly · 4 styles         │
│  TTF FONTS                   │
│  Literata        ← available │
│  TinySerif       ⓘ >256 KB * │
├──────────────────────────────┤
│ Preview "Literata, 14 pt"    │
│ The quick brown fox …        │
└──────────────────────────────┘
```

`*` = PSRAM-only size gate (§3.3): row drawn dithered/disabled with a one-line reason on Confirm. Confirm applies: writes `readerFontEngine=READER_ENGINE_TTF`, `ttfFontFamilyName`, snaps size, persists once (`saveToFile()`), then the preview re-flows via the chain.

**Size tab (Tab::Size)** — today: discrete rows from `readerFontPointSizes()` ("12 pt"…, `snapToNearestPointSize` in `rebuildSizeList`, TextSettingsActivity.cpp:150). Proposed: when the active family is TTF, a **continuous picker**: a row pager 8→72 pt stepping 1 (or 2) pt with wrap, live numeric label, preview re-layout debounced per stop (the e-ink refresh is the debounce). Built-in/bitmap families keep the discrete list unchanged.

**Layout tab (Tab::Layout)** — today: Line spacing, Paragraph spacing, Alignment, Screen margin rows (LayoutRow enum, TextSettingsActivity.h:32). Proposed: add **Letter spacing** row (engine `LayoutParams` extension, §5.7) with range −20..+20 % in 5 % steps, previewed live. Ships only if upstream accepts the field; otherwise omitted (§11 Q2).

**Style tab (Tab::Style)** — today: Focus Reading, Hyphenation, Embedded styles, Anti-aliasing (StyleRow, TextSettingsActivity.h:33; the row toggles `SETTINGS.textAntiAliasing`, CrossPointSettings.h:289, default ON). Proposed: the Anti-aliasing row **stays visible on TTF families** — it is real hardware capability, not a bitmap-only nicety. v1 semantics: ON → `FrameFormat::Mono1Dithered` (engine-native dithered edge AA from true 8-bit glyph coverage, zero extra RAM), OFF → `Mono1Sharp` (hard coverage threshold, crispest stems) (§5 D9). Once the `PagePaint` dual-plane parity pass lands (CrossPoint-side, no engine change — §11 Q7), ON upgrades to true 4-level gray edges through the same dual-plane panel waveforms the bitmap path uses (`GrayPlanes.h` tone mapping → `GfxRenderer::displayGrayBuffer()`); OFF stays `Mono1Sharp`. Focus Reading maps to engine `focusReading` (per-word fixation bolding via `markFocusWords`, ChapterLayout.cpp:498-523). Family style coverage ("Regular · Bold · Italic" availability) surfaces in the Font tab, not here.

**Reader in-book Text panel** (`EpubReaderActivity` toolbar Text panel, `textRowName/textRowValue`, EpubReaderActivity.h:186-195): same options, smaller surface — family row lists TTF families + built-ins; size row uses the same TTF continuous mode. All changes route through `applyTextSettingLive()` → abort session → new generation hash → rebuild + `pageForChar` restore (§3.5.6).

---

## 13. References

Code (this worktree):

- Reader today: `lib/GfxRenderer/GfxRenderer.h`, `lib/GfxRenderer/GrayPlanes.h`, `lib/hal/HalDisplay.h`, `lib/EpdFont/EpdFont.h`, `lib/EpdFont/EpdFontFamily.h`, `lib/EpdFont/SdCardFont.h`, `lib/EpdFont/SdCardFontManager.h`, `lib/EpdFont/SdCardFontRegistry.h`, `lib/Epub/Epub/ReaderRenderSpec.h`, `lib/Epub/Epub/ParsedText.h`, `lib/Epub/Epub/blocks/TextBlock.h`, `lib/Epub/Epub/Section.cpp`, `src/SdCardFontSystem.{h,cpp}`, `src/CrossPointSettings.{h,cpp}`, `src/ReaderFontSizes.h`, `src/fontIds.h`, `src/main.cpp`, `src/activities/reader/EpubReaderActivity.{h,cpp}`, `src/activities/reader/ReaderUtils.h`, `src/activities/settings/TextSettingsActivity.{h,cpp}`, `src/activities/settings/TextSettingsPreview.{h,cpp}`
- Engine: `freeink-sdk/libs/book/FreeInkBook/include/BookFont.h`, `include/BookArena.h`, `include/BookProfile.h`, `include/BookStorage.h`, `include/BookTypes.h`, `include/FreeInkBook.h`, `include/render/TtfFont.h`, `include/render/PageRenderer.h`, `src/render/PageRenderer.cpp`, `include/layout/ChapterLayout.h`, `include/cache/PageCache.h` (under `freeink-sdk/libs/book/FreeInkBook/`); FreeInkUI: `freeink-sdk/libs/ui/FreeInkUI/include/FreeInkUIBookFont.h`, `include/FreeInkUIDisplayTarget.h`, `include/FreeInkUIGfxRenderer.h`; FreeInkDisplay: `freeink-sdk/libs/display/FreeInkDisplay/src/driver/PanelDriver.h`
- Engine design doc: `freeink-sdk/docs/freeink-book.md` (authoritative; note: a stale `docs/freeink-book-design.md` filename is referenced at `TtfFont.h:25`, `FreeInkBook.h:9`, and `freeink-book.md:11`)
- Bitmap font tooling (the constraint TTF replaces): `lib/EpdFont/scripts/fontconvert_sdcard.py`, `lib/EpdFont/scripts/cpfont_version.py`, `lib/EpdFont/scripts/build-font-ids.sh`
- Research: LVGL/TinyTTF/stb PSRAM strategies — see `TtfFont.h:25` and `freeink-sdk/docs/freeink-book.md` for engine-level references

Discrepancies found in the previous draft, all corrected above:

1. `lib/Epub/ReaderRenderSpec.h` → actual `lib/Epub/Epub/ReaderRenderSpec.h` (include `<Epub/ReaderRenderSpec.h>` resolves via the lib include root).
2. `lib/EpdFont/EpdfFontFamily.h` typo → `lib/EpdFont/EpdFontFamily.h`.
3. `-DFREEINK_BOOK_PROFILE=1` does not exist → `-DFREEINK_BOOK_SMALL=1` / `-DFREEINK_BOOK_LARGE=1` (`include/BookProfile.h`).
4. Section cache "version 25" → `SECTION_FILE_VERSION = 46` (Section.cpp:57).
5. `sdFontFamilyName[64]` → actual `char sdFontFamilyName[32]` (CrossPointSettings.h:361).
6. `src/Section.cpp` → actual `lib/Epub/Epub/Section.cpp`.
7. §10 references cited `src/SdCardFontManager.*` / `src/SdCardFontRegistry.*` → actually in `lib/EpdFont/`.
8. "8 MB PSRAM on ESP32-C3" → C3 build environments ship PSRAM-less (platformio.ini:11, :265-268); PSRAM exists on S3 devices only (§2.4).
9. Kagi share URL removed from references (non-canonical, rot-prone).
10. **Submodule init required**: `freeink-sdk` is a git submodule; `git submodule update --init` must be run to inspect the FreeInkBook/FreeInkUI/FreeInkDisplay engine sources referenced throughout this doc (CodeRabbit/Copilot review note, 2026-09-08).
11. §11 quantization "uniform quartiles" (≥192/≥128/≥64) → actual converter banding ≥144/≥96/≥48 (`bm = coverage >> 4`, edges 9/6/3; fontconvert.py:361-390, fontconvert_sdcard.py:673-689); BW base rule "coverage ≥ 64" → "coverage ≥ 48" (the tone-1 boundary; a 64 threshold would leave plane-flagged pixels at coverage 48-63 without black base).
12. §11 construction (b) "band-clipped sub-page" — added the rotation limitation: coherent only for 0°/180° (`FrameRotation::None`/`UpsideDown`); under 90°/270° (`Portrait`/`PortraitInverted`) a physical row band maps to a logical x-column range, and `PageTextRun` stores no run width (ChapterLayout.h:56-63), so x-culling would re-walk advance/kerning — the loop (b) exists to avoid.
13. §11 "all public engine API" → caveat: `decodeUtf8` is translation-unit-local (PageRenderer.cpp:97-118); `PagePaint` carries its own len-bounded decoder and replicates the synthetic-bold/underline arms (:183-206).
14. `renderBook()` ranges :1443-1516 / :1425-1529 → actual extent EpubReaderActivity.cpp:1412-1781.
15. Heap-gate constants cited at EpubReaderActivity.cpp:147-154 → actual EpubReaderActivity.h:149-154.
16. `displayGrayBuffer()` attributed to `GrayPlanes.h` (4 places) → it is `GfxRenderer::displayGrayBuffer()` (GfxRenderer.h:372, GfxRenderer.cpp:2482 → HalDisplay.h:93 → FreeInkDisplay.cpp:786); `GrayPlanes.h` holds only the tone-mapping helpers.
17. `renderImages(...)` "(Bayer dither)" → mono images use Floyd–Steinberg error diffusion (PageRenderer.cpp:120-158); Bayer `kBayer4` is text-edge only.
18. `Page::render` cited [Page.h:28] → [Page.h:119] (:28 is `PageElement::render`, the pure virtual).
19. `resolveFontId` cited SdCardFontSystem.cpp:27 → declaration h:27, definition cpp:173.
20. `beginStripTarget` cited GfxRenderer.h:74-88 (strip-state members) → declaration + geometry doc :231-239 (def GfxRenderer.cpp:1693-1703).
21. `BOOK_PROFILE` phase logs cited :479-519 → actual blocks span :480-518.
22. Bitmap double-walk cite :1892-1898 (gray-pass lambda only) → :1892-1907 (BW walk at :1907; tiled path: 1 + nBands culled walks).
23. §12 "SD Card Fonts section" → the code builds a flat `fonts_` list with no section header (TextSettingsActivity.cpp:86-92).
24. §12 `snapToNearestPointSize` cited TextSettingsActivity.cpp:382 (direct assignment in `applySize`) → actual snap at :150 in `rebuildSizeList`.
25. `BookProfile` "capacities live at use sites (ChapterLayout.cpp, TtfFont.h)" → advance/glyph slots TtfFont.h:97-99; `kMaxPages`/`kMaxAnchors` PageCache.h:105-106.
26. Stale `docs/freeink-book-design.md` filename noted only at `TtfFont.h:25` → also referenced at `FreeInkBook.h:9` and `freeink-book.md:11`.
27. Focus reading described as engine "paragraph-level" (`focusReading`) → engine is per-word (`markFocusWords` bolds each word's first ~45% with the 1..9-codepoint clamp, ChapterLayout.cpp:498-523; runs split at focus boundaries :1505-1587); §2.3/§3.5/§12 corrected.
28. Synthetic bold "must be verified in Phase 2" (§3.7) / "optional upstream ask" (§5 D5b) → already implemented upstream: `PageRenderer::renderText` double-strikes +1px on a bold shortfall (PageRenderer.cpp:183-195); §3.7/§5 D5/§8 R7 corrected.
29. "Style bits map 1:1 / mapping is a cast" → only BOLD/ITALIC/UNDERLINE coincide; the old STRIKETHROUGH=8/SUP=16/SUB=32/RUBY_CONTINUE=64 bits (EpdFontFamily.h:10-21) collide with `StyleSuperscript=8`/`StyleSubscript=16` (BookFont.h:17-24) — a cast would render strikethrough as superscript; §2.1/§2.3/§5 D8 corrected.
30. **Phase 3.5 added (§4)**: the §3.5 feature-parity note previously deferred ruby + per-word focus splits (and mis-cited §8 R6) — replaced by the blocking Phase 3.5 parity-enforcement gate with the full verified feature ledger (engine-covered items, CrossPoint adapter work, genuine engine gaps), §8 R3 rewritten as a blocking risk, §11 Q3 resolved into the gate, and Phase 4 deletion gated on the checklist.


---

## 14. Font architecture & UX split (round-3 directive)

> **Status: USER DIRECTIVE, 2026-09-10.** Supersedes any earlier wording in this
> document about a mixed/dual-font UX or a DRAM font tier (§3.3 directive stands:
> PSRAM-only). This section is the authoritative font-architecture plan for Phase 2+.

### 14.1 Two device classes, one font stack each

| Device class | Builds | Font engine | Reader activity | Font UX |
|---|---|---|---|---|
| **PSRAM** (X4 Pro, X4C, Paper Mono; `BOARD_HAS_PSRAM`) | `CROSSPOINT_TTF_READER=1` (default ON) | FreeInkBook native TTF (`FontChain`) | New reader activity only | Family picker + continuous size |
| **PSRAM-less** (X4, Sticky; C3 / no-PSRAM S3) | flag absent | Legacy EpdFont bitmap path | Existing `EpubReaderActivity` unchanged | Existing bitmap font picker, no size slider |

- **Compile-time split, not runtime.** On PSRAM-less builds none of the TTF stack
  links at all (`BookFontLoader`, adapters, `ChapterLayout`/`PageRenderer` path,
  font/size settings UI) — zero flash/RAM cost, zero new UX. On PSRAM builds the
  legacy reader activity, `FontCacheManager`, and the old font-settings tab are
  **compiled out**; the two render paths never coexist in one binary.
- **No runtime toggle, no dual-mode testing.** A device is one class or the
  other for its whole life.
- **UI chrome stays bitmap on every device** (`GfxRenderer` + `EpdFont`):
  menus, settings, dialogs are crisp at fixed sizes and need no scaling. "TTF
  devices" differ only in the book-reading surface.
- **CJK on PSRAM-less is a documented v1 limitation.** The PSRAM-less reader
  chain is the `BitmapBookFont` fallback only; CJK text renders only as far as
  the builtin bitmap families' coverage goes — they already include
  NotoSansHebrew/NotoSansArabic glyph data, but CJK ideographs are NOT covered
  and render as missing glyphs. No SD-`.cpfont` font is wired into the reader
  chain (§3.6).
- **Flag scope (Phase 2a shipped).** `CROSSPOINT_TTF_READER=1` is defined
  exactly on the nine PSRAM-class envs: `x4pro`, `x4pro_profile`, `x4c`,
  `x4c-gh_release`, `x4pro-gh_release`, `x4pro-gh_release_rc`, `papermono`,
  `papermono-gh_release`, `papermono-gh_release_rc`. It is absent from every
  C3 and sticky env, so PSRAM-less binaries stay byte-identical to `develop`
  (zero flash/RAM cost; verified by `pio run -e default`). During Phase 2 both
  render paths still coexist in PSRAM binaries (the legacy reader is the
  build-flag kill switch); the legacy reader is compiled out only in Phase 4.

### 14.2 Font size UX

Continuous size control exists **only on the TTF device class** — bitmap fonts
are baked at fixed sizes (`BitmapBookFont` metrics ignore `sizePx`), so a size
control over the legacy engine is meaningless. Consequences:

- PSRAM builds: the reader font settings show family + size (8..72 pt,
  `ttfFontPointSize`) because TTF makes size real. One settings shape, no
  conditional widgets.
- PSRAM-less builds: the classic fixed-size list, exactly as today.
- We do **not** ship a mode-dependent settings screen that mixes both — that
  would cost code *and* confuse users (bitmap + size = no-op).

### 14.3 Fallback font: Atkinson Hyperlegible Next (round-3 directive)

The always-present chain-tail fallback for PSRAM builds is **Atkinson
Hyperlegible Next, served from the bitmap font data already baked in flash**
(`lib/EpdFont/builtinFonts/atkinson_hn_*.h`) — no new font payload, no
`gen_font.py` run. Consumed through a CrossPoint-side `EpdBookFont :
book::RenderFont` adapter over `EpdFontData` (see §14.5 for the full design
and rationale; `BitmapBookFont`'s contiguous-range/1-4bpp assumptions do not
fit the compressed 2-bit interval-based builtin data).

### 14.4 SD layout & file expectations (normative for Phase 2)

**Round-3 directives: (a) one subfolder per family; (b) reuse the SAME font
folders as the legacy bitmap system.** The TTF scanner walks the roots the
legacy registry already uses and simply ignores the files it does not care
about — one folder tree hosts both engines, no migration, no duplication.

```
/fonts/                            ← visible root (legacy: /fonts)
  Literata/
    Literata-Regular.ttf           ← TTF: style inferred from the name (below)
    Literata-Bold.ttf
    Bookerly_14.cpfont             ← legacy file: IGNORED by the TTF scanner
  Bookerly-SD/
    Bookerly-SD_14.cpfont          ← legacy .cpfont bundle, untouched
  SomeFamily/
    regular.otf                    ← .otf works too
  /.fonts/                         ← hidden root (legacy-preferred, SdCardFontRegistry.h:33-34)
  free-fonts.json                  OPTIONAL, at root level (display names,
                                   license); disk scan always wins
```

**Folder = family; extension filtering is the only gate.** Both roots
(`/fonts` and the hidden `/.fonts`) are scanned — hidden-root families win,
exactly as `SdCardFontRegistry::scanRoot()` de-duplicates today
(SdCardFontRegistry.cpp:201-202). Within a family folder the TTF scanner
accepts only `.ttf`/`.otf`; `.cpfont` (legacy bundles), `.tmp`, `~` backups,
`.json`, and macOS `._*`/hidden files are skipped without a log. The legacy
`SdCardFontRegistry` in turn ignores `.ttf`/`.otf` (it only accepts
`<name>_<size>.cpfont`, SdCardFontRegistry.cpp:62-95) — the two scanners
coexist on the same folders with zero interference.

**Folder name = family display name** (case preserved). Nested folders inside
a family folder are ignored (one level deep only).

**Style inference (per accepted file).** Match the filename
(case-insensitive, extension stripped, word-boundary so `SemiBold` never
matches `Bold`) against, in priority order:

1. `bolditalic` / `bold_italic` / `bold-italic` → BoldItalic
2. `italic`, `oblique`, `ital` → Italic
3. `bold` → Bold
4. `regular`, `normal`, `book`, `roman`, `text` → Regular
5. No match → heuristics: `semibold`/`demibold`/`medium`/`black`/`heavy`/
   `extrabold` → Bold; `light`/`thin` → Regular; folder with exactly one file
   → Regular regardless of its name; otherwise the file is skipped with
   `LOG_DBG` (unknown style). Duplicate style resolution: lexicographically
   first wins, rest logged. A family with no Regular match but ≥1 file
   promotes its first file to Regular.

- One file = one face; up to 4 faces per family (regular/bold/italic/
  bold-italic); 32-family cap (`kMaxDiscoveredFamilies`).
- Per-face size guard: 2MB (CWE-400) on the PSRAM tier; fonts are loaded
  whole-file into PSRAM and stay resident while the face is live (§3.3).
- Enumeration: `HalStorage::listFiles()` on each root +
  `HalFile::openNextFile()`/`isDirectory()` per family folder — the same
  two-level walk `SdCardFontRegistry::scanDirectory()` already performs
  (SdCardFontRegistry.cpp:98-142). No new HAL surface needed.
- `free-fonts.json` stays optional metadata; when it and the disk disagree,
  disk wins (§3.7).
- PSRAM-less boards: the TTF scanner does not run at all; the legacy
  registry keeps the folders to itself (§3.3 directive).

### 14.5 Fallback font: Atkinson Hyperlegible Next from EXISTING bitmap data (round-3 directive)

The always-present chain-tail fallback for PSRAM builds is **Atkinson
Hyperlegible Next reusing the bitmap data already shipped in flash** — no new
font payload is added:

- The repo already bakes Atkinson Hyperlegible Next at 4 sizes × 4 styles as
  compressed 2-bit `EpdFontData` (`lib/EpdFont/builtinFonts/atkinson_hn_*.h`,
  generated by `fontconvert.py` with `--2bit --compress`; families wired in
  `src/main.cpp:118-146`).
- The SDK's `BitmapBookFont` cannot consume that data directly (it assumes a
  contiguous codepoint range and raw 1/4-bit glyphs; `EpdFontData` uses
  unicode intervals, optional DEFLATE groups, and 2-bit packing). Phase 2
  therefore adds a small **`EpdBookFont : book::RenderFont`** adapter
  (CrossPoint-side, alongside `src/adapters/`) that wraps one
  `EpdFontData*`: interval lookup → `FontDecompressor` → 2-bit expansion to
  8-bit coverage → `GlyphBitmap`, mapping `EpdGlyph.left/top` to
  `xoff/yoff` and `fp4::toPixel(advanceX)` to `advance`. The four
  atkinson_hn14 style faces register as the fallback chain in place of the
  four `kNotoSansFont` instances (BookFontLoader.cpp:208-211).
- Rationale: the fallback is the same typeface the bitmap reader already
  uses; zero additional flash; kern/ligature tables come along for free.
- Fallback semantics unchanged: end-of-chain, never a selectable family,
  covers glyphs/styles a chosen TTF family lacks; single baked size
  (headings render at body size under fallback). `kNotoSansFont` stays
  UI-chrome only.
