# Design Brief: Native TTF Font Support for CrossPoint X Reader

## Task

Design a complete plan for supporting native TTF font files (no conversion, no .cpfont) in the CrossPoint X reader firmware, with on-the-fly UI settings changes, PSRAM-aware caching, and replacement of the current EpdFont bitmap text engine with the FreeInkBook TTF engine.

## Skill location for reference

The `/crosspoint-reader-dev` Hermes skill lives at `skill: software-development/crosspoint-reader-dev`. Key reference files within it:
- `references/font-regen-gotchas.md` — bitmap font regeneration constraints (read this for the reproducibility gate and why TTF replaces it)
- `references/epub-engine-topology.md` — EPUB/text rendering pipeline topology
- `references/psram-allocator-patterns.md` — PSRAM allocation patterns (heap_caps_malloc, MALLOC_CAP_SPIRAM, arena sizing)
- `references/opencode-design-gate.md` — design doc review workflow

## Repository context

- **Design worktree**: `/home/balor/workspace/eink/crosspoint-x-reader-design-ttf` (branch `design/native-ttf-support`)
- **Primary repo**: `/home/balor/workspace/eink/crosspoint-x-reader`
- **FreeInkBook SDK** (submodule): `freeink-sdk/libs/book/FreeInkBook/` — NOT yet linked in platformio.ini lib_deps
- **FreeInkUI** (submodule): `freeink-sdk/libs/ui/FreeInkUI/`

## Key codebase files (read these to understand the architecture)

### Current reader (to be replaced)
- `lib/EpdFont/EpdFont.h` — bitmap font format
- `lib/EpdFont/SdCardFont.h` — .cpfont loader + on-demand glyph loading
- `lib/EpdFont/EpdFontFamily.h` — multi-style family container
- `lib/Epub/Epub/ParsedText.cpp` — word-level layout engine
- `lib/Epub/Epub/blocks/TextBlock.h/.cpp` — single line rendering
- `lib/Epub/Epub/Section.cpp` — section cache (font ID is part of cache key)
- `lib/GfxRenderer/GfxRenderer.{h,cpp}` — framebuffer text rendering (RETAIN for UI chrome)
- `src/SdCardFontSystem.cpp` — SD font discovery, loading, font ID resolution
- `src/activities/reader/EpubReaderActivity.cpp` — reader activity
- `src/activities/settings/TextSettingsActivity.cpp` — 4-tab settings UI (Font/Size/Layout/Style)
- `src/activities/settings/TextSettingsPreview.cpp` — live preview via reader engine
- `src/CrossPointSettings.h/.cpp` — settings: fontFamily, sdFontFamilyName, fontPointSize
- `src/fontIds.h` — font ID constants
- `src/ReaderFontSizes.h` — BUILTIN_READER_POINT_SIZES = {12, 14, 16, 18}

### FreeInkBook engine (to replace current, NOT yet linked)
- `freeink-sdk/docs/freeink-book.md` — engine design doc (authoritative reference)
- `freeink-sdk/libs/book/FreeInkBook/include/BookFont.h` — RenderFont interface (advance, lineHeight, ascent, kerning, ligature, rasterize)
- `freeink-sdk/libs/book/FreeInkBook/include/render/TtfFont.h` — TtfFont (stb_truetype wrapper) + FontChain (per-codepoint fallback, up to 8 faces)
- `freeink-sdk/libs/book/FreeInkBook/src/render/TtfFont.cpp` — stb_truetype init, glyph cache (direct-mapped, arena-bounded, flush-and-rebuild)
- `freeink-sdk/libs/book/FreeInkBook/include/layout/ChapterLayout.h` — SAX → block flow → UAX#14 line breaking → PageSink
- `freeink-sdk/libs/book/FreeInkBook/src/render/PageRenderer.cpp` — Page → 1bpp framebuffer (Bayer dither, Floyd-Steinberg for images)
- `freeink-sdk/libs/book/FreeInkBook/include/cache/PageCache.h` — FIBP v3 binary cache, layoutGenerationHash, charStart anchors
- `freeink-sdk/libs/book/FreeInkBook/include/BookArena.h` — bump allocator (caller-sized, no free(), high-water mark)
- `freeink-sdk/libs/book/FreeInkBook/include/BookStorage.h` — BookSource + CacheStorage adapter interfaces
- `freeink-sdk/libs/book/FreeInkBook/include/BookProfile.h` — Small/Standard/Large tiers (compile-time)
- `freeink-sdk/libs/book/FreeInkBook/include/BookTypes.h` — BookStatus, TocEntry, ManifestItem
- `freeink-sdk/libs/ui/FreeInkUI/include/FreeInkUIBookFont.h` — BitmapBookFont (bitmap→RenderFont adapter) + TtfGlyphSource (TTF→UI fallback)
- `freeink-sdk/libs/ui/FreeInkUI/include/FreeInkUIDisplayTarget.h` — native DisplayTarget (1-bit framebuffer)
- `freeink-sdk/libs/ui/FreeInkUI/include/FreeInkUIGfxRenderer.h` — FreeInkUI→GfxRenderer adapter

### TTF research (user-provided)
- `/home/balor/.hermes/webui/attachments/1d8cb3219c1a/TTF_Font_Rendering_on_ESP32-C3.md` — LVGL+TinyTTF, FreeType, stb_truetype direct, PSRAM malloc strategies, memory layout
- `/home/balor/.hermes/webui/attachments/1d8cb3219c1a/TTF_Font_Rendering_on_ESP32-C3-1.md` — same content, second copy

### Hardware context
- **Target**: ESP32-C3 (default_env in platformio.ini), with ESP32-S3 variants
- **PSRAM**: 8 MB external PSRAM (ESP32-C3 with PSRAM add-on)
- **Display**: 1-bit e-ink panel (800×480 typical)
- **Key finding from research**: `malloc()` does NOT reliably use PSRAM on ESP32 — must use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` explicitly
- **Important**: The `platformio.ini` targets `esp32-c3-devkitm-1` as default. ESP32-C3 has no SDMMC controller — only SPI mode SD.

## Existing design document

A design brief has already been written at `/home/balor/workspace/eink/crosspoint-x-reader-design-ttf/DESIGN_NATIVE_TTF_SUPPORT.md`. Please:
1. Review it thoroughly
2. Check it against the codebase (the files listed above)
3. Refine and expand it where it's missing analysis — especially:
   - More detail on how the existing `GfxRenderer` → `EpdFont` text path maps to `PageRenderer` → `TtfFont`
   - The exact PSRAM arena sizing for ESP32-C3 (400 KB internal SRAM, how much free)
   - The font settings flow: how `SETTINGS.getReaderFontId()` would change
   - Whether free-fonts.json manifest approach is right vs. SD card scanning
   - The UI wireframe details based on the screenshots (see /home/balor/.hermes/webui/attachments/1d8cb3219c1a/)
4. Add any missing sections (error handling, testing strategy, rollback plan)

## Deliverable

A refined, detailed design document written to `/home/balor/workspace/eink/crosspoint-x-reader-design-ttf/DESIGN_NATIVE_TTF_SUPPORT.md` (overwriting the existing draft). The document should be comprehensive enough that an engineer could start implementing from it.

## Format

Use the existing 10-section structure as the skeleton. Expand each section with concrete details, code snippets where helpful, and precise file references. Add any new sections needed (e.g., Testing Strategy, Rollback Plan).

## Verification

After writing the design doc, verify:
1. The doc is syntactically valid markdown
2. All referenced file paths in the doc actually exist in the worktree
3. All function names mentioned exist in the actual headers (e.g., TtfFont::init signature, PageRenderer::render signature, layoutGenerationHash signature, etc.)
4. Report any discrepancies found between the design doc and the actual codebase