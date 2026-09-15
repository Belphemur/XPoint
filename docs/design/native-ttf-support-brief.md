# Native TTF Font Support — Executive Summary

## Goal

Add native `.ttf`/`.otf` font support to the CrossPoint X reader: users can
drop a font file in `/fonts/` on the SD card and use it to render book text,
with on-the-fly settings changes (family, size, line spacing, anti-aliasing),
replacing the bitmap-only `EpdFont` text pipeline on PSRAM-class builds.

**Full design:** see `docs/design/ttf/` in this repository.

## Current state

Shipped on PSRAM-class builds (`x4pro`, `x4c`, `papermono`):

- FreeInkBook reader path live (`EpubReaderActivity` → `TtfBookRuntime`).
- FIBP page caches live (`catalog.fibc` + `s<spine>-<hash8>.fibp`).
- TTF family loading, continuous size, and chain-tail Atkinson fallback live.
- Reader quick font sheet live (page-only relayout + FAST refresh).
- Dictionary / footnotes / anchors live on the TTF path.
- Ruby, focus reading, links, and synthetic bold live.
- Full-frame dual-plane gray fallback + uniform tone quantizer live.

The legacy bitmap reader path remains in the tree as the rollback path;
PSRAM-less builds (C3 / sticky) do not link the TTF stack.

## Architecture change

```
Before:  EpubReaderActivity → Section.bin → ParsedText → TextBlock → GfxRenderer/EpdFont  (1-bit bitmap)
After:   EpubReaderActivity → TtfBookRuntime → FIBP cache → ChapterLayout → PagePaint/gray planes  (TTF)
```

`GfxRenderer` and `EpdFont` remain for UI chrome (menus, headers, popups).
Only the book-text rendering path switched to FreeInkBook on the TTF device
class.

## Memory strategy

All runtime arenas are `PoolBytes` (PSRAM-backed on `BOARD_HAS_PSRAM`), with
no static BSS. Font bytes are PSRAM-first with a bounded loader fallback.
The TTF stack is compile-gated to PSRAM-class builds only; PSRAM-less
binaries keep the legacy reader and do not allocate TTF arenas.

## Caches

FIBP replaces the Section `.bin` cache. Validity is keyed on
`layoutGenerationHash(params, fontFingerprint())` — any settings change
(family, size, line spacing, alignment, orientation) produces a different
hash, so only affected spines rebuild.

## Anti-aliasing

The engine rasterizes true 8-bit glyph coverage. The reader reaches 4-level
gray parity through its existing dual-plane LSB/MSB machinery, with a
uniform tone quantizer `(3*coverage + 127)/255`. Full-frame fallback covers
non-strip panels; strip panels keep the existing per-band walk. Details in
`docs/design/ttf/2026-09-14-grayscale-pipeline.md`.

## Design corpus

| Document | Scope |
|---|---|
| `docs/design/ttf/2026-09-10-native-ttf-architecture.md` | runtime, caches, font loading, build gating |
| `docs/design/ttf/2026-09-10-font-architecture-and-ux.md` | device-class split, picker, quick sheet |
| `docs/design/ttf/2026-09-14-grayscale-pipeline.md` | dual-plane gray transport + quantizer |
| `docs/design/ttf/2026-09-10-implementation-plan.md` | historical plan, decisions, risks, rollback |
| `docs/design/ttf/2026-09-14-dictionary-and-word-selection.md` | dictionary flow on the TTF path |
| `docs/design/ttf/2026-09-14-sdk-submodules-and-miniz.md` | SDK pinning, nested miniz, inflate ownership |

## Files

- `src/BookFontLoader.{h,cpp}` — TTF discovery, loading, chain management
- `src/adapters/` — `SdCardBookSource`, `SdCardCacheStorage`,
  `FrameTargetFactory`, `PagePaint`, `EpdBookFont`
- `src/activities/reader/EpubReaderActivity.{h,cpp}` — reader integration
- `src/activities/reader/TtfBookRuntime.{h,cpp}` — chapter pipeline
- `src/activities/reader/TtfWordSelect.{h,cpp}` — word selection payload
- `src/activities/settings/TextSettingsActivity.{h,cpp}` — UI extensions
- `src/CrossPointSettings.{h,cpp}` — TTF settings keys + migration

## Reader quick font sheet

Reader size/family adjustment uses a compact two-row bottom sheet over the
still-visible page: `Size 28 [ - ] [ + ]` and `Family [ - ] [ + ]`. A step
performs a transient page-only ChapterLayout pass through the real engine,
paints the page, and FAST-refreshes; it does not rebuild the page cache. The
sheet closes with one settings save and a full reflow. `TextSettingsActivity`
remains the full advanced picker. Details in
`docs/design/ttf/2026-09-10-font-architecture-and-ux.md`.

## Prerequisites

Initialize git submodules recursively to inspect engine references;
FreeInkBook vendors `esp_full_miniz` as a nested submodule:

```bash
git submodule update --init --recursive
```
