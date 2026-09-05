# Poweroff screen follows the Sleep Screen setting

Status: design (pre-implementation)
Related: `docs/design/auto-power-off.md`, `docs/design/shutdown-reason-marker.md`

## Problem

The poweroff screen (manual power off and the auto-power-off timer wake) always
renders the current book's cover, framed, with the "press power to turn on"
caption — regardless of what the user chose for the **Sleep Screen** setting.
A user who configured custom sleep art (`/sleep.bmp`, `/sleep/` folder) or the
blank screen still gets a book cover at power off. The two screens should
agree: poweroff keeps its frame and explanatory caption, but the *picture*
follows `SETTINGS.sleepScreen`.

## Requirements

1. The poweroff image is selected by the existing `sleepScreen` setting
   (`CrossPointSettings::SLEEP_SCREEN_MODE`). No new settings, no new i18n
   strings.
2. The frame (~2/3 panel border) and the `STR_SHUTDOWN_PRESS_POWER` caption are
   kept exactly as today whenever an image is shown.
3. Modes with no standalone image at poweroff fall back to today's light logo
   screen + caption: `DARK`, `LIGHT`, `QUICK_RESUME`, `TRANSPARENT_CUSTOM`.
   (Decision 2026-09-05: keep it light and simple — the caption must stay
   readable ink-on-white; quick-resume needs the prior panel frame and overlays
   need a base screen, neither of which exists at a cold power-off.)
4. `BLANK` shows a blank screen + caption only — no logo. (Decision
   2026-09-05: faithful to the setting.)
5. The cover filter setting (`sleepScreenCoverFilter`) applies like sleep does:
   `NO_FILTER` keeps grayscale for images that carry it, `BLACK_AND_WHITE`
   (contrast) forces a BW render, `INVERTED_BLACK_AND_WHITE` forces BW and
   flips the whole screen. (Decision 2026-09-05.)
6. Cover staging must stop paying for modes that will not use it (skip EPUB
   cover generation on every power off in `CUSTOM`/`BLANK`/logo modes).

## Non-goals

- No change to the normal sleep screen rendering.
- No new user-facing settings or strings.
- No night-mode polarity handling in the shutdown path (unchanged today).
- QUICK_RESUME and TRANSPARENT_CUSTOM do not get poweroff-specific art.

## Design

### Mode → image mapping

| `sleepScreen` mode | Poweroff screen |
|---|---|
| `DARK`, `LIGHT` | Light logo + caption (today's no-image path) |
| `QUICK_RESUME`, `TRANSPARENT_CUSTOM` | Light logo + caption (fallback) |
| `BLANK` | Blank screen + caption |
| `CUSTOM` | `/sleep.bmp`, else random valid BMP from `/.sleep`, else `/sleep` — drawn inside the frame, caption below |
| `COVER` | Staged cover BMP inside the frame; no/stale cover → logo + caption |
| `COVER_CUSTOM` | Staged cover BMP inside the frame; no/stale cover → `CUSTOM` image inside the frame; neither → logo + caption |

### Code touch points

- `src/activities/boot_sleep/SleepActivity.h` / `.cpp`
  - Make the sleep render helpers static member functions taking
    `GfxRenderer&` (`renderCustomSleepScreen`, `renderCoverSleepScreen`,
    `renderBitmapSleepScreen`, `renderDefaultSleepScreen`). They already touch
    no instance state — only the member `renderer`, which becomes the
    parameter — so `renderShutdownScreen` (static) can reuse them instead of
    duplicating the selection logic.
  - Extract a file-local "render BMP inside the shutdown frame" helper:
    clear → draw bitmap at `calculateBitmapPlacementInBounds` (2/3 frame,
    inset 6 — constants preserved) → `drawRect` frame → caption (same
    two-line split at `". "`) → `displayBuffer(HALF_REFRESH)`. With
    `NO_FILTER` and a grayscale bitmap, the gray passes run over the framed
    base exactly like `renderBitmapSleepScreen` (base displayed via
    `displayGrayscaleBase(HALF)`, LSB/MSB planes carry only the image rect —
    frame and caption stay in the base; same compositing the overlay path
    already relies on).
  - `renderShutdownScreen(renderer)`: keep signature and the
    `autoPowerOffCoverBmpPath` clear-after-paint. Select the branch from
    `SETTINGS.sleepScreen` per the table above. CUSTOM images render with
    `Bitmap(file, /*dithering=*/true)` (same as sleep); covers keep the
    undithered `Bitmap(file)`.
  - Filter handling mirrors `renderBitmapSleepScreen`:
    `hasGreyscale && filter == NO_FILTER` → gray passes; filter ==
    `INVERTED_BLACK_AND_WHITE` → `invertScreen()` before the base display,
    gray passes skipped.
- `src/main.cpp` — `stageAutoPowerOffCover()`
  - Stage only when `SETTINGS.sleepScreen` is `COVER` or `COVER_CUSTOM`.
    Call sites and their `requireTimerEnabled` semantics (deep-sleep entry
    requires the auto-power-off timer; manual power off unconditionally)
    are unchanged.
  - Safety: `APP_STATE.loadFromFile()` (main.cpp:666) runs before the
    timer-wake shutdown render (main.cpp:766-777), so a cover staged at
    deep-sleep entry still reaches the wake render.
- `src/CrossPointState.h/.cpp` — unchanged; `autoPowerOffCoverBmpPath` keeps
  its persisted key and clear-after-paint lifecycle.
- `USER_GUIDE.md` — one sentence in the power-off section: the poweroff
  screen shows the picture configured by the Sleep Screen setting.

### Cost / memory

- Net negative RAM/heap: no new buffers beyond what the existing paths
  allocate (`Bitmap` row streaming, gray work planes reused as today).
- Net negative CPU/SD: in non-cover modes the EPUB cover is no longer parsed
  and the cover BMP no longer generated at power off.
- Flash: a few hundred bytes for the mapping switch; no new strings.

### Verification

- `pio run -e x4pro` green; `./bin/clang-format-fix` clean.
- Manual matrix (device, human tester): modes {DARK, LIGHT, CUSTOM with
  `/sleep.bmp`, CUSTOM with `/sleep/` folder, COVER in book, COVER from home,
  COVER_CUSTOM in book, COVER_CUSTOM from home, COVER_CUSTOM with no sleep
  files, BLANK, QUICK_RESUME, TRANSPARENT_CUSTOM} × {manual power off, auto
  power off timer}, plus one pass with filter = Inverted.

## Alternatives considered

- **Full parity incl. DARK inversion** — rejected (user decision): the caption
  must stay readable; a dark inverted shutdown screen reads worse and needs a
  light-colored caption.
- **QUICK_RESUME/TRANSPARENT treated as CUSTOM** — rejected (user decision):
  quick-resume art is the retained panel frame and overlays composite over an
  existing screen; neither exists after a cold power cut.
- **Duplicate selection logic in the shutdown path** — rejected: the sleep
  helpers are already renderer-only; a static refactor is smaller and keeps
  one source of truth for `/sleep.bmp` → `/.sleep` → `/sleep` resolution.
