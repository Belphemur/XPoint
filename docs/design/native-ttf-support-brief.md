# Native TTF Font Support — Executive Summary

## Goal

Add native `.ttf`/`.otf` font support to the CrossPoint X reader: users can
drop a font file in `/fonts/` on the SD card and use it to render book text,
with on-the-fly settings changes (family, size, line spacing, anti-aliasing),
replacing the current bitmap-only `EpdFont` text pipeline.

**Full design:** see `DESIGN_NATIVE_TTF_SUPPORT.md` in this repository.

## Architecture Change

The reader text path moves from the local `Epub` library's bitmap pipeline
(`ParsedText` → `TextBlock` → `Section .bin` cache → `GfxRenderer::drawText`
with `EpdFont` glyphs) to the **FreeInkBook** engine (`freeink-sdk/libs/book/`):

```
Before:  EpubReaderActivity → Section.bin → ParsedText → TextBlock → GfxRenderer/EpdFont  (1-bit bitmap)
After:   EpubReaderActivity → FIBP cache → ChapterLayout → PageRenderer → TtfFont/stb_truetype  (TTF)
```

`GfxRenderer` and `EpdFont` are **retained** for UI chrome (menus, headers,
popups). Only the book-text rendering path switches to FreeInkBook.

## Two-Phase Rollout

- **Phases 0–1**: Wire up the FreeInkBook engine as a submodule library, add
  `BookFontLoader` (TTF discovery + loading) and `SdCardBookSource`/
  `SdCardCacheStorage`/`FrameTargetFactory` adapters. No reader changes.
- **Phase 2**: Integrate into `EpubReaderActivity` behind
  `-DCROSSPOINT_TTF_READER=1` (kill switch). FIBP page cache replaces the
  Section `.bin` cache.
- **Phase 3**: Settings UI — TTF family section in the Font tab, continuous
  size picker, anti-aliasing row stays (maps to engine `FrameFormat`).
- **Phase 4**: Remove the legacy bitmap reader path (`ParsedText`, `blocks/`,
  `Section`); keep `GfxRenderer`/`EpdFont` for UI, `SdCardFontSystem` for
  UI CJK fallback.

## Memory Strategy

The engine is free-standing (no stdlib/malloc); all buffers are caller-owned
`Arena`s. Two tiers are probed at runtime via
`heap_caps_get_total_size(MALLOC_CAP_SPIRAM)` (not `get_free_size` — total
capacity, not free, is the PSRAM-presence discriminator):

| Tier | Hardware | Strategy |
|------|----------|----------|
| `DramC3` | ESP32-C3 (PSRAM-less) | All arenas in DRAM; `-DFREEINK_BOOK_SMALL=1`; 256KB font-size gate |
| `PsramS3` | ESP32-S3 + PSRAM (X4 Pro, Paper Mono) | Arenas in PSRAM via `heap_caps_malloc`; ~1MB per-face budget |

## Cache

FIBP (FreeInkBook page cache, `PageCacheWriter`/`PageCacheReader`) replaces the
Section `.bin` cache. Cache validity is keyed on
`layoutGenerationHash(params, fontFingerprint())` — any settings change
(family, size, line spacing, alignment, orientation) produces a different
hash, causing only affected spines to rebuild.

## Anti-Aliasing

The engine rasterizes true 8-bit glyph coverage via `stb_truetype`. CrossPoint
panels are 1-bit controllers but support 4-level text AA via dual LSB/MSB plane
RAM + the panel AA waveform (the current bitmap path already uses this when
`SETTINGS.textAntiAliasing` is ON). TTF v1 ships with the engine's native
dithered 1bpp AA (`FrameFormat::Mono1Dithered`); a no-engine-change 4-level
parity path via tiled Gray8 quantization is tracked in the design doc
(§11 Q7).

## Files

- `DESIGN_NATIVE_TTF_SUPPORT.md` — full design document
- `src/BookFontLoader.{h,cpp}` — TTF discovery, loading, chain management (new)
- `src/adapters/` — `SdCardBookSource`, `SdCardCacheStorage`,
  `FrameTargetFactory`, `PagePaint`, `SdCardBookFontAdapter` (CJK fallback)
- `src/activities/reader/EpubReaderActivity.{h,cpp}` — reader integration
- `src/activities/settings/TextSettingsActivity.{h,cpp}` — UI extensions
- `src/CrossPointSettings.{h,cpp}` — new settings keys + migration

## Prerequisites

Initialize git submodules to inspect engine references:

```bash
git submodule update --init
```
