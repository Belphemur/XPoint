# Trim the legacy `.cpfont` SD-card font path on TTF boards (X4 Pro / S3)

Date: 2026-10-18 · Status: proposed · Author: Hermes Agent (for Belphemur/Belphemur)
Domain: firmware / font subsystem · Base: `develop` · Target branch: `trim/cpfont-x4pro`

## Context

The `.cpfont` (v4 binary bitmap font) system is the legacy SD-card font reader
(`SdCardFont` parser + `FontDecompressor` cpfont mode + `FontInstaller` cpfont
validation + `SdCardFontSystem::loadCpfontVariant` + `FontCacheManager` sd-cache
loops + `GfxRenderer::ensureSdCardFontReady` `MISS_CTX_SD_CARD` branch). These
are compiled **ungated for every env** today.

On `CROSSPOINT_TTF_READER` boards (x4pro/x4c/papermono — ESP32-S3 + PSRAM), the
book text path is the native TTF/FreeType reader (`BookFontLoader` -> `FtFont`)
and the UI script fallback is `TtfUiFont`/`TtfUiFont` (§14.1 of
`docs/design/2026-09-17-freetype-font-backend.md`). PR #187 (TTF distribution)
added `TtfFontDownloadActivity` + `distribute-fonts.py` as the X4 Pro SD-font
manage flow, while keeping the legacy `FontDownloadActivity` (.cpfont) for the
non-TTF (FREEINK_BOOK_SMALL) boards.

Traceability that the `.cpfont` *render* path is dead on TTF boards:
- `renderer.sdCardFonts_` (a `map<int, SdCardFont*>`) is populated only by
  `GfxRenderer::registerSdCardFont` (GfxRenderer.h:180), which is only reached
  from the `MISS_CTX_SD_CARD` glyph-miss fast path (GdxRenderer.cpp:112).
- That miss path is only taken for fonts whose `EpdFontData::missKind ==
  MISS_CTX_SD_CARD` (EpdFontData.h:254). TTF boards load baked bitmaps
  (`MISS_CTX_BUILTIN`) and `FtFont` (not `EpdFontData`) — never `MISS_CTX_SD_CARD`.
- The `FontVariantProvider` that *would* create an `SdCardFont` (`load` ->
  `SdCardFontSystem::loadVariant`) is wired only inside `SdCardFontSystem::setup`
  (SdCardFontSystem.cpp:42). It is never configured on the TTF reader path, so
  `loadVariant` is never invoked at runtime on a TTF board.
- Therefore `sdCardFonts_` stays empty on TTF boards and the whole `.cpfont`
  parser is unreachable — it is linked only because `ensureSdCardFontReady` +
  `FontCacheManager` reference `SdCardFont` symbols, defeating `--gc-sections`.

## Size (measured on `develop` x4pro build, `.pio/build/x4pro/firmware.map`)

Per-object `.text` (cpfont-ish objects, ~58 KB total). Bold = in scope for removal:

| Object | `.text` | Keep/remove on TTF |
|---|---|---|
| **lib/EpdFont/SdCardFont.cpp** (the `.cpfont` parser) | **20,543 B** | remove |
| **lib/EpdFont/SdCardFontManager.cpp** (cpfont face mgr) | **4,017 B** | remove |
| src/SdCardFontSystem.cpp (`loadCpfontVariant`/`setupUiFallbacks`/provider wire-up) | 6,294 B | gate cpfont render methods, keep management facade |
| **src/FontInstaller.cpp** (cpfont validation) | 3,471 B | keep TTF install, gate cpfont validation |
| lib/GfxRenderer/FontCacheManager.cpp (sd-cache loops) | 2,691 B | keep (bitmap prewarm path live); sd loops become no-ops |
| lib/GfxRenderer/GdxRenderer.cpp (`ensureSdCardFontReady` MISS_CTX_SD_CARD branch) | (shared) | gate the branch |
| lib/EpdFont/FontDecompressor.cpp | 4,050 B | **keep** (EpdBookFont + bitmap prewarm) |
| lib/EpdFont/EpdFont.cpp | 2,125 B | **keep** (baked-bitmap accessor) |
| src/adapters/EpdBookFont.cpp | 1,089 B | **keep** (TTF reader bitmap fallback) |
| src/TtfUiFallback.cpp | 7,352 B | **keep** (X4 Pro CJK UI fallback) |
| lib/EpdFont/SdCardFontRegistry.cpp | 7,576 B | **keep** (family listing for the download activity) |
| lib/EpdFont/EpdFontFamily.cpp | 287 B | **keep** |

Realizable TTF-board savings ≈ **30 KB of `.text`** (SdCardFont.cpp alone is
~20.5 KB and is the dominant lever). The exact delta is confirmed by a before/after
`firmware.map` `.text` diff + `arm-none-eabi-size` Flash/IRAM/DRAM in the verification.

## Decision

Gate the **legacy `.cpfont` *render* path** with `#if !defined(CROSSPOINT_TTF_READER)`
(zero new build flags; re-uses the existing TTF-reader capability flag). On TTF
boards the cpfont-only translation units become empty and `--gc-sections` drops
them; the non-cpfont render path is untouched.

### Gated out on TTF boards (compiled only when `!CROSSPOINT_TTF_READER`)
- the `MISS_CTX_SD_CARD` branch of `GdxRenderer::ensureSdCardFontReady` + its
  `registerSdCardFont`/`isSdCardFont` consumers of `SdCardFont*`;
- the `FontVariantProvider` wire-up (`setFontVariantProvider` call) in
  `SdCardFontSystem::setup`, plus `loadVariant`/`loadCpfontVariant`/`setupUiFallbacks`/
  `ensureLoaded`'s cpfont branch;
- the cpfont-only bodies of `SdCardFont.cpp` and `SdCardFontManager.cpp`
  (whole-file `#if` so they are empty TUs, hence GC'd, on TTF boards);
- the `.cpfont` install/validation in `FontInstaller` (`validateCpfontFile`,
  `isCpfontFilename`, `installCpfont`); keep the TTF install path (used by
  `TtfFontDownloadActivity`).

### Kept unconditionally (live on TTF boards)
`FontDecompressor`, `EpdFont`, `EpdBookFont`, `TtfUiFont`/`TtfUiFont`,
`BookFontLoader`/`FtFont`, `EpdFontFamily`, the baked bitmaps
(`fontIds.h`, `builtinFonts/`), `FontCacheManager` (bitmap prewarm), and the
`SdCardFontSystem` management facade + `SdCardFontRegistry` (family listing).

## Verification
1. `pio run -e x4pro` builds; before/after `.text`/Flash/DRAM delta from
   `firmware.map` + `arm-none-eabi-size`; `SdCardFont.cpp.o` absent from the new
   map.
2. `pio run -e default` (C3, `FREEINK_BOOK_SMALL`, `!CROSSPOINT_TTF_READER`) still
   builds with the cpfont path intact — no regression for legacy boards.
3. Host tests: `cmake -S test -B test/build-host && cmake --build test/build-host &&
   ctest --test-dir test/build-host --output-on-failure`.
4. `./bin/clang-format-fix -g`.

## Open question (pre-flight for the implementation job)
Do `FREEINK_BOOK_SMALL` (non-TTF) boards actually load `.cpfont` files from SD, or
also use the baked-bitmap path? If the latter, the cpfont parser is dead there too
and a global removal (not a gate) is possible — but this change intentionally
*keeps* cpfont on legacy boards (the conservative, safe choice), so the question
does not block this PR.
