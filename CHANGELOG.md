# Changelog

All notable changes to XPoint are documented in this file.

## [Unreleased]

### Added

- FreeType-backed native-TTF reader (design `docs/design/2026-09-17-freetype-font-backend.md`, #146): under `CROSSPOINT_FONT_BACKEND_FT=1` on PSRAM-class devices (x4pro/x4c/papermono), `BookFontLoader` builds the reading font chain over the SDK's `FtFont` instead of stb-backed `TtfFont` — true bold/italic from variable-font `wght`/`ital`/`slnt` axes with FT-side oblique/embolden synthesis, per-face glyph arena eliminated (FreeType owns the glyph slot, all FT heap PSRAM-preferring), and metrics-only `glyphBounds` band-culling parity. Requires freeink-sdk ≥ `6a8addc` (#26: `FtFont::glyphBounds`, CFF/OTTO module enablement, rasterize bitmap-lifetime fix). Stb path remains compilable for rollback (`CROSSPOINT_FONT_BACKEND_FT=0`).

### Fixed

- SD-card firmware update and other confirmation dialogs: pressing Confirm was treated as Cancel after the defensive pop-result guard introduced in #75. Confirm again starts the update (this also unblocks locked X4 Pro devices downgrading to upstream CrossPoint firmware from the SD card).
