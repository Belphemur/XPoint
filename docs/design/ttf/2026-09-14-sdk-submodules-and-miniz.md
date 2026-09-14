# SDK Submodules, FreeInkBook, and miniz Policy

ISO date: 2026-09-14
Status: Current policy
Scope: SDK pinning, nested submodules, single-miniz rule, inflate ownership

## 1. SDK relationship

The FreeInk SDK is consumed as the `freeink-sdk` git submodule:

```bash
git submodule update --init --recursive
```

- `freeink-sdk` is pinned to SDK `main` and advanced deliberately
  (`git submodule update --remote` in a dedicated pin-bump commit).
- Firmware links the SDK libraries as `symlink://` deps in `platformio.ini`
  (`BatteryMonitor`, `InputManager`, `EInkDisplay`, `SDCardManager`,
  `UsbMassStorage`, `BoardConfig`, `PowerManager`, `FrontlightManager`,
  `Rtc`, `Imu`, `SecureNet`, `FreeInkUI`, `FreeInkBook`, `Icons`).
- No engine source is modified in-tree; engine changes land upstream and
  the submodule pointer moves.

## 2. Nested miniz submodule

FreeInkBook vendors its own deflate/inflate dependency as a nested
submodule:

- `freeink-sdk/libs/book/FreeInkBook/third_party/miniz`
- pinned to the hardened esp_full_miniz fork, v1.15 fork @ `a6bf8fc`
- consumed header-only through
  `freeink-sdk/libs/book/FreeInkBook/third_party/miniz/include`
- `FreeInkBook/src/vendor/miniz.h` is the provenance wrapper that keeps the
  engine away from ESP-IDF's ROM miniz and supplies the build-tunable
  `include/full_miniz.h`.

The firmware build includes only the fork's headers; none of the fork's
wrapper/archive objects are built directly (they would collide with
FreeInkBook's own vendored miniz). `platformio.ini` adds
`-I freeink-sdk/libs/book/FreeInkBook/third_party/miniz/include` and relies
on the linker to bind the tinfl/tdefl calls to the engine's own objects.

## 3. Single-miniz-in-firmware policy

There is exactly one miniz in the firmware: FreeInkBook's. Consequences:

- **FreeInkBook is the sole inflate consumer** for EPUB content (chapter
  XML, images, any deflated ZIP member).
- No other subsystem may link its own miniz, zlib, or ROM tinfl/tdefl.
- Image decoding (pngle/tjpgd), XML parsing, and EPUB container access all
  funnel through the engine's `BookSource`/`Book`/`ChapterLayoutSession`
  substrate instead of spawning a second inflate path.

This keeps the symbol surface unambiguous, avoids ROM/vendor collisions,
and makes memory-profile tuning (arena sizing, inflate window ownership) a
single-owner decision.

## 4. Pin discipline

- Bumps of `freeink-sdk` are their own commit (`[P4] chore: advance
  freeink-sdk pin to ...`), never mixed with feature changes.
- A pin bump must state the SDK commit, any nested submodule pins, and the
  host test status (currently 416/416).
- If SDK `main` moves, re-pin only after the affected upstream PRs are
  verified against the firmware's lifetime and arena contracts — e.g.
  SDK PR #23 fixed owned `ZipEntry` / `LayoutParams` across resumable
  sessions, which this firmware depends on.
- The nested `esp_full_miniz` pin (`a6bf8fc`) is held unless the SDK pin
  bump explicitly changes it; do not advance nested pins incidentally.

## 5. Engine relationship

FreeInkBook is consumed as a submodule symlink:

```
freeink-sdk/libs/book/FreeInkBook
```

It is C++17, freestanding (no exceptions / RTTI), and depends on
caller-provided `Arena` buffers. The engine's XML layer (`XmlSax`) uses the
firmware's hardened expat via `-DFREEINK_BOOK_EXTERNAL_EXPAT=1`
(`-DXML_GE=0 -DXML_CONTEXT_BYTES=1024`), not a second vendored copy.

## Cross-links

- Architecture: `2026-09-10-native-ttf-architecture.md`
- Font architecture & UX: `2026-09-10-font-architecture-and-ux.md`
- Grayscale pipeline: `2026-09-14-grayscale-pipeline.md`
- Dictionary flow: `2026-09-14-dictionary-and-word-selection.md`
- Implementation plan (historical): `2026-09-10-implementation-plan.md`
