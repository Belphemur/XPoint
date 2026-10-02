# Design: Direct TTF Font Downloader for PSRAM Devices

## Goal
Replace the device-initiated font downloader so that PSRAM-enabled devices
(ESP32-S3 / X4 Pro, X4C) can download raw TTF files from a JSON manifest,
while preserving the existing `.cpfont` mechanism for non-PSRAM devices (C3).

## Gating
- `BOARD_HAS_PSRAM` is defined on all S3 envs (x4pro, x4c, sticky, papermono) in platformio.ini.
- `CROSSPOINT_TTF_READER=1` + `CROSSPOINT_FONT_BACKEND_FT=1` are also S3-only flags.
- The new TTF downloader compiles in **only** under `CROSSPOINT_TTF_READER`.
- C3 builds keep FontDownloadActivity.cpp (the .cpfont path) unchanged.

## Architecture

### New files
1. `src/activities/settings/TtfFontDownloadActivity.h` / `.cpp`
   - Mirrors FontDownloadActivity's state machine (WIFI_SELECTION → LOADING_MANIFEST → FAMILY_LIST → DOWNLOADING → COMPLETE/ERROR)
   - Same manifest parsing pattern (ArduinoJson + filter) but reads `kind: "ttf"` + `preview` + `files[].path`
   - Uses HttpDownloader::downloadToFile with CRC32 post-check
   - Writes to `/<root>/<family>/<filename>.ttf` via SdCardFontSystem registry root
   - After download: calls `sdFontSystem.markRegistryDirty()` + `sdFontSystem.ensureLoaded()` to pick up the new font
   - Shows preview image if available (decode PNG from URL or cache on SD)

2. `include/activity/TtfFontDownloadActivity.h` (or keep in src/activities/ like FontDownloadActivity)
   - Gate: `#if defined(CROSSPOINT_TTF_READER)` around the class declaration and registration in ActivityManager

### Modified files
1. `src/main.cpp` — Register TtfFontDownloadActivity only under `CROSSPOINT_TTF_READER`
2. `src/activities/settings/SettingsActivity.cpp` — Add "Download Fonts (TTF)" menu row gated on `CROSSPOINT_TTF_READER`
3. `src/activities/settings/FontDownloadActivity.cpp` — Wrap existing activity registration in `#if !defined(CROSSPOINT_TTF_READER)` so C3 keeps .cpfont and S3 gets TTF. Alternatively: S3 gets BOTH (cpfont for legacy + ttf), choose based on user preference / settings field `fontSourceMode` in CrossPointSettings.

Actually — simpler: keep FontDownloadActivity for C3 only, add TtfFontDownloadActivity for S3 only. The settings page shows the appropriate activity based on build.

### Manifest format (already defined in distribute-fonts.py)
```json
{
  "version": 1,
  "kind": "ttf",
  "baseUrl": "https://pub-...r2.dev/ttf/",
  "families": [{
    "name": "NV Libron",
    "type": "serif",
    "description": "...",
    "source": "...",
    "preview": "https://.../previews/preview_Libron.png",
    "styles": ["regular", "bold", ...],
    "files": [{
      "name": "NV_Libron-Regular.ttf",
      "path": "NV_Libron/NV_Libron-Regular.ttf",
      "style": "regular",
      "size": 154000,
      "crc32": 12345678
    }]
  }]
}
```

### Key implementation details
- **Download target**: `/<root>/<family>/<filename>` where `<root>` is FontInstaller's defaultWriteRoot (`/fonts` or `/.fonts`)
- **CRC32**: firmware already has `computeFileCrc32` pattern in FontDownloadActivity.cpp:145 — reuse `esp_rom_crc32_le`
- **Progress**: HttpDownloader::downloadToFile supports a ProgressCallback + cancelFlag
- **PSRAM buffer**: use `poolMakeBytes` (existing pattern at FontDownloadActivity.cpp:179) for manifest fetch
- **HAL compliance**: all SD I/O through HalStorage/Storage, never raw SdFat

### Files to read before implementing
- FontDownloadActivity.cpp lines 219-338 (manifest parsing) — REUSE almost verbatim
- FontDownloadActivity.cpp lines 338-470 (download loop, CRC32 check) — REUSE pattern
- FontInstaller.h (file validation, path building) — extends for .ttf validation
- SdCardFontSystem.h (registry, ensureLoaded) — call after download
- BookFontLoader.h (family discovery) — how fonts get picked up after download
- platformio.ini:386-411 (x4pro env) — confirm CROSSPOINT_TTF_READER gate
- src/main.cpp:40-115 (font object registration) — know how to register a new family
- fontIds.h (font ID constants)
- HttpDownloader.h (downloadToFile API)
- MappedInputManager.cpp:20-55 (button mapping for UI rows)

## Risk: C3 compat
C3 has ~380KB RAM, no PSRAM. Loading a TTF face into PSRAM and letting FreeType manage glyph atlases can exceed DRAM on C3. That's exactly why `CROSSPOINT_TTF_READER` is absent from the C3 build — the new activity inherits that gate automatically.

## Verification
1. `pio run -e default` (C3 build) — must compile without the new class
2. `pio run -e x4pro` (S3 build) — must compile with the new class
3. Host tests if TtfFontDownloadActivity is abstracted for HOST_TEST
