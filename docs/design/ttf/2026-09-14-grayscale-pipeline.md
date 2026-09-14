# TTF Grayscale Pipeline

ISO date: 2026-09-14
Status: Shipped (full-frame fallback + uniform quantizer)
Scope: text anti-aliasing on the native-TTF reader path

## 1. Why this exists

The engine rasterizes true 8-bit glyph coverage (`TtfFont` / `stb_truetype`).
CrossPoint panels are 1-bit controllers, but the reader already has a
dual-plane 4-level gray path for bitmap text (`GfxRenderer`'s
`GrayPlanes.h` + `displayGrayBuffer()`). The TTF path reaches the same 4-level
parity by painting the engine's coverage through that existing dual-plane
machinery — no engine `FrameFormat` change is required.

## 2. Transport split

`renderBookTtf()` chooses the transport from `grayscaleCapabilities()`:

- **Strip panels** (`stripUploads == true`, SSD1677-class): the existing
  per-band `beginStripTarget()` dual-plane walk.
- **Non-strip panels** (UC8279 X4-class): the same dual-plane bits painted
  into full-frame plane buffers — one `GRAYSCALE_DUAL` walk over a
  full-frame target (`beginStripTarget(lsb, 0, panelHeight, msb)`),
  submitted through `copyGrayscaleLsbBuffers()` /
  `copyGrayscaleMsbBuffers()` / `displayGrayBuffer()` /
  `cleanupGrayscaleWithFrameBuffer()`.

No capability flag is flipped: `stripUploads` stays false for the X4 driver,
and its scan geometry / waveform are untouched. Both transports share the
same base/cleanup contract (`ttfDisplayGrayBase()`).

Full-frame plane buffers are `poolMakeBytes` on the PSRAM-class builds that
compile the TTF path (`CROSSPOINT_TTF_READER` is absent from every C3/sticky
env, so no C3 allocation case exists), bounded by a 128KB/plane guard.
Allocation happens before any refresh-state mutation; pool exhaustion
(`nullptr`) falls back to a plain B/W display.

## 3. Uniform tone quantizer

`pagepaint::grayTone()` is the single quantizer for base and planes:

```cpp
tone = (3u * coverage + 127u) / 255u;
```

Boundaries:

| tone | coverage range | meaning |
|---|---|---|
| 0 | 0..42 | no plot, no plane bits |
| 1 | 43..127 | light gray: MSB |
| 2 | 128..212 | dark gray: LSB + MSB |
| 3 | 213..255 | solid ink: the BW base carries it |

- stb coverage is linear pixel coverage — no gamma 2.2.
- The old 48/96/144 `.cpfont`-converter banding and the older 1/8/12
  discussion thresholds are retired.
- `PagePaint::paintText()` (base, tone ≥ 1) and `PagePaint::paintPlanes()`
  (MSB/LSB flags) both call `grayTone()`. One quantizer, never a local
  threshold copy.

## 4. Tone-profile swap point

The quantizer is one function in `pagepaint`. A future calibrated profile
(e.g. a 256-byte reflectance LUT from measured panel response) replaces that
single function — it does **not** introduce per-callsite thresholds. The
compositing rule is to quantize the completed pixel, not each draw-op;
same-glyph outlines stay with the rasterizer.

## 5. Host tests

- `PagePaintQuantizer.*` — boundary and exhaustive monotonicity checks.
- `PagePaintConsistency.BaseAndPlanesAgreeWithSharedQuantizer` — base plots
  tone ≥ 1; MSB = tones 1–2; LSB = tone 2; tone 3 base-only.
- `PagePaintEquivalence.StripBandsMatchFullFrame` — band-aggregated plane
  bits ≡ full-frame plane bits.
- `GrayPlanesTest` converter-banding assertions use the uniform boundaries.

## 6. Open calibration work

Device validation remains owner/manual: four solid patches + a coverage ramp
through the exact `Uc8279X4Driver` / LUT-02 path, ghosting, and gray↔BW
transitions. The future path is a measured-panel LUT profile swapped into
`grayTone()` (not a local threshold change). FreeType streaming remains the
separate upgrade path behind the same `BookFont` interface for very large /
CJK faces, unrelated to the tone curve.

## Cross-links

- Architecture: `2026-09-10-native-ttf-architecture.md`
- Font architecture & UX: `2026-09-10-font-architecture-and-ux.md`
- Dictionary flow: `2026-09-14-dictionary-and-word-selection.md`
- SDK/miniz policy: `2026-09-14-sdk-submodules-and-miniz.md`
