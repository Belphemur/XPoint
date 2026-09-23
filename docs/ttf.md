# TTF support: XPoint vs upstream CrossPoint

Both readers ship TrueType font support, but they built it in different places.
This page has two sections: a plain-language summary first, then the full
technical comparison for contributors.

## ELI5: How XPoint's TTF support differs from upstream CrossPoint

Both readers draw your `.ttf`/`.otf` files with the **same font engine**
(FreeType, via the FreeInk SDK). The difference is what wraps around it.

Upstream CrossPoint **teaches its old bitmap-font pipeline to draw TTF glyphs**:
it loads a TTF file, rasterizes glyphs, and pushes them through the same
renderer that was originally designed for pre-baked bitmap fonts. On PSRAM
boards it can even stream huge fonts straight from the SD card so they never
fully fit in RAM.

XPoint instead **rebuilds the whole book-rendering pipeline around the engine**:
fractional glyph metrics, real kerning, ligatures, anti-aliased grayscale
rendering, and sections that are laid out once and cached on the SD card. The
trade-off: the two readers are not drop-in compatible at the file level — XPoint
does not read upstream's `.ttf` pipeline files, and upstream does not read
XPoint's cached sections — but the fonts themselves (any `.ttf`/`.otf`/`.ttc`
file) work on both.

What that means for you as a reader:

- **Typography quality**: XPoint renders kerned, hinted, anti-aliased text; the
  bitmap-pipeline port draws square-grid glyphs without those refinements.
- **Big fonts**: both can stream oversized fonts from the SD card now (XPoint
  adopted upstream's streaming design). XPoint streams with a PSRAM prefix
  cache; give up on nothing else — small fonts still load fully into PSRAM.
- **CJK in menus**: both now fall back to your TTF font when a menu string
  needs Chinese/Japanese/Korean (or Greek, Cyrillic, …) glyphs the built-in
  bitmap UI fonts lack. XPoint shares the reader's already-loaded font, so the
  fallback costs almost no extra memory.
- **`.cpfont` vs `.ttf`**: `.cpfont` is XPoint's pre-rasterized bitmap family
  format (fast, no engine needed, best for PSRAM-less boards). `.ttf`/`.otf`/
  `.ttc` use the native engine. Either can be selected in Settings; native TTF
  requires a PSRAM board (X4 Pro / X4 Classic).

## Full technical comparison

### Architecture

| Aspect | Upstream CrossPoint (#3646) | XPoint fork |
| --- | --- | --- |
| Adapter layer | `TtfEpdFont` + `VectorFontSupport.h`: an `EpdFont`-compatible view over `freeink::font::FtFont`, drawn through the legacy bitmap pipeline | Native `FontChain` (FreeInkBook) owns the TTF path end-to-end: `BookFontLoader` discovers families, `TtfBookRuntime` lays out, `EpdBookFont` adapts the *other* direction (bitmap → reader engine) for fallback tails |
| Device gating | `CROSSPOINT_VECTOR_FONTS` (PSRAM boards) | `CROSSPOINT_TTF_READER` (PSRAM boards) + `CROSSPOINT_FONT_BACKEND_FT` |
| Reader pipeline | Legacy EpdFont draw path per glyph | Fractional-metrics layout, kerning/ligatures, gray dual-plane AA, FIBP section caches |
| Glyph delivery | Per-page glyph set materialized by `TtfEpdFont` (`build()`/`addCoverage()`), served via `EpdFontData` extension hooks | FreeType faults glyphs on demand through `FtFont::rasterize26_6`; P1 glyph bitmap cache per face |
| Style resolution | `SdCardFontRegistry::refineVectorStyles`: OS/2 weight + italic via `inspectStream`, deterministic nearest-400/700 pick | Ported (TASK 1): `BookFontLoader::refineStyles` with the same deterministic pick; filename inference demoted to the unreadable-face fallback; role→slot map folds into the font fingerprint so caches invalidate |
| Collections (.ttc) | Supported (face index 0 or first Unicode cmap face) | Ported (TASK 2): SDK `FtFont` gained `faceIndex` on init/initStream/inspectStream; registry accepts `.ttc` |
| Streaming oversized fonts | `openTtfSource`/`prefixRead`: >2MB faces stream from SD with a ~1MB PSRAM prefix; GPOS kerning disabled when streamed | Ported (TASK 3): same design through the native stack (`tryLoadStreamedFace`, 24MB cap, 1MB prefix, HalFile ReadFn thunk); small faces stay fully resident in PSRAM |
| CJK UI fallback | `setupTtfUiFallbacks`: `TtfEpdFont` per UI size (16KB cache, 384 glyphs), FNV id + `\x01ui` salt | Ported (TASK 4): `TtfUiFont` adapter per UI size borrowing the loader's resident bytes (no copies); glyphs fault through the existing `EpdFontData::glyphMissHandler` seam; degrade funnel on refused render options; heap-gated at 256KB PSRAM largest-block |
| Cache invalidation | Fingerprint over loaded bytes | Content FNV over face bytes + per-slot path hashes + face indices + render-options tag + role-map tag |
| Memory resilience | TtfEpdFont arenas registered as the `gfx.ttfGlyphArenas` eviction sink | `WordStore` bump arena + OOM latching + evict-and-retry (see below); **no** TTF sink — the loader's release path frees face bytes that `TtfUiFont` UI faces borrow, so evicting mid-layout would dangle FreeType faces |

### What XPoint ported from upstream #3646

1. **Face-metadata style resolution** — real OS/2 weights replace filename
   guessing; deterministic role assignment; fingerprint-safe.
2. **`.ttc` support** — SDK `FtFont` face-index plumbing (fork SDK PR), registry
   acceptance, sfnt validation of the embedded directory.
3. **Streaming oversized fonts** — the `openTtfSource`/`prefixRead` design,
   adapted to `HalFile` mutex discipline and the native loader.
4. **TTF CJK UI fallback** — the UI sizes route scripts the built-ins lack to
   the active TTF family via the existing `GfxRenderer::setFallbackFont`
   plumbing.
5. **lib/Epub memory hardening** — `WordStore` bump arena, `ParsedText` handle
   storage, OOM latching (`hadDroppedWords` → `layoutOom` → `ParseStatus::Error`
   → `Section::finalizeBuild` abandon), `TextBlock` evict-and-retry, packed
   advance-table handoff (`buildAdvanceTablePacked`), and the
   `gfx.renderGlyphCache`/`gfx.sdFontMini` eviction sinks.

### What XPoint deliberately did NOT adopt

- **`TtfEpdFont` / `VectorFontSupport.h` / `CROSSPOINT_VECTOR_FONTS`** — that
  pipeline duplicates the same `freeink::font::FtFont` engine through the legacy
  bitmap draw path. Adopting it would resurrect the dual-reader problem the
  device-class split exists to prevent.
- **The `gfx.ttfGlyphArenas` eviction sink** — upstream's sink drops
  `TtfEpdFont` arenas, which are safe because its font BYTES stay resident. In
  XPoint the loader's release path frees the borrowed bytes themselves (and the
  TTF UI-fallback faces borrow them), so an equivalent sink would dangle. The
  loader's `releaseResidentCaches()` remains the coordinated release point.

### Directive amendments (owner decisions, 2026-09-23)

- **Streaming approved**: the original "skip oversized faces" rule was replaced
  by upstream's streaming pattern; skipping is now only the fallback when PSRAM
  cannot fund the 1MB prefix cache. Streamed faces give up GPOS kerning
  (`setGposByteBudget(0)`) — advances come from the (unchanged) glyph metrics,
  so layout stays stable.
- **CJK UI fallback approved**: the "UI chrome stays bitmap" rule is amended for
  scripts the built-in bitmap UI fonts cannot render. Latin UI text stays on the
  built-in bitmap fonts; heap-gating skips the fallback entirely when PSRAM
  cannot fund it.

### When to use which format

- **`.ttf` / `.otf` / `.ttc`** under `/fonts/` (or `/.fonts/`): full native
  rendering, any size, PSRAM boards only.
- **`.cpfont`** (pre-rasterized bitmap family): works on every board, faster to
  load, but fixed to the rasterized sizes and no kerning/ligatures.

### Cross-links

- Font architecture & UX: `docs/design/ttf/2026-09-10-font-architecture-and-ux.md`
- Native TTF architecture: `docs/design/ttf/2026-09-10-native-ttf-architecture.md`
- Grayscale pipeline: `docs/design/ttf/2026-09-14-grayscale-pipeline.md`