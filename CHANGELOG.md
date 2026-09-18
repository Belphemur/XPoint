# Changelog

All notable changes to XPoint are documented in this file.

## [Unreleased]

### Added

- Background FIBP chapter-index prefetch ("index-ahead", design addendum §7 of `docs/design/2026-09-18-freetype-backend-as-built.md`): on S3-class builds (PSRAM + FreeType) a 24KB-stack, priority-1 worker task pinned to core 0 lays out spine chapters ahead of the reader and commits their FIBP caches to SD, so chapter entry hits the cache instead of the blocking "Indexing" popup. The worker owns its own `TtfBookRuntime` and `FtFont` faces (FT face create/destroy stays on the main thread), replicates the loader fingerprint byte-for-byte so its caches land under the reader's generation names, skips spines whose cache exists, defers to the reader's sync rebuild via a single-writer handoff, and re-seeds on generation bumps. Inert on C3 / stb builds (synchronous fallback unchanged); the build driver is shared with the reader via the new `ChapterIndexEngine`.
- FreeType-backed native-TTF reader (design `docs/design/2026-09-17-freetype-font-backend.md`, #146): under `CROSSPOINT_FONT_BACKEND_FT=1` on PSRAM-class devices (x4pro/x4c/papermono), `BookFontLoader` builds the reading font chain over the SDK's `FtFont` instead of stb-backed `TtfFont` — true bold/italic from variable-font `wght`/`ital`/`slnt` axes with FT-side oblique/embolden synthesis, per-face glyph arena eliminated (FreeType owns the glyph slot, all FT heap PSRAM-preferring), and metrics-only `glyphBounds` band-culling parity. Requires freeink-sdk ≥ `6a8addc` (#26: `FtFont::glyphBounds`, CFF/OTTO module enablement, rasterize bitmap-lifetime fix). Stb path remains compilable for rollback (`CROSSPOINT_FONT_BACKEND_FT=0`).

### Fixed

- SD-card firmware update and other confirmation dialogs: pressing Confirm was treated as Cancel after the defensive pop-result guard introduced in #75. Confirm again starts the update (this also unblocks locked X4 Pro devices downgrading to upstream CrossPoint firmware from the SD card).
