# Design: Reading Progress on Cover Sleep/Shutdown Screens

## Goal

When the user selects the cover as the sleep screen (Cover mode), display
the current book's reading progress percentage at the bottom of the screen.
This applies to both the sleep screen (idle timeout) and the shutdown screen
(manual power-off).

## Background

The sleep screen is rendered by `SleepActivity`. When `sleepScreenCoverMode`
is `COVER`, `COVER_CUSTOM`, or `COVER_TRANSPARENT`, the activity resolves
the book cover bitmap from `APP_STATE.openEpubPath` and paints it as the
full-screen sleep image (or a framed image for the shutdown path).

Reading progress is tracked by `ProgressManager` and persisted per-book
in the reading-stats binary format. The percentage can be computed via
`loadBookProgress(bookPath)`, which returns an `int` in `0..100`.

`loadBookProgress` allocates an `Epub` (or `Xtc`) and re-reads metadata plus
`progress.bin` from SD, which is a cost sleep and power-off transitions must
not pay on every render. The readers instead seed a cache:
`CrossPointState::openBookProgressPercent` (`int8_t`, `-1` = unknown) is set
in `ReaderActivity::onExit()` from the live `getScreenshotInfo().progressPercent`
(all reader types), before that path's `APP_STATE.saveToFile()` — so the
value is also persisted for the timer-wake shutdown screen, which boots cold
and reads it from `state.json` like `autoPowerOffCoverBmpPath`. Every path
into the sleep/shutdown screens exits the reader first (`replaceActivity`
with `ReplaceForSleep` and `shutdown()` both run `onExit()`), so the cache
is fresh whenever a cover is rendered. The SD reload
(`loadBookProgress`) remains only as a fallback when the cache is still
`-1` (state written before this field existed, no reader exit since the
upgrade).

During shutdown and sleep transitions, `APP_STATE.openEpubPath` is still
populated (it is set by `ReaderActivity::onEnter`/`loadBook` and only
cleared by a subsequent reader `onEnter`, not by `onExit`). This means
the SleepActivity has access to the book path at render time.

## Design

### Setting

A new toggle setting `sleepScreenCoverShowProgress` (uint8_t, default `1` /
On) in the Display category, placed immediately after the existing
`sleepScreenCoverFilter` setting. This lets users who prefer a clean cover
opt out.

- **StrId**: `STR_SLEEP_COVER_PROGRESS`
- **Label**: "Show Reading Progress on Cover"
- **Type**: Toggle (On/Off)
- **Default**: 1 (On)
- **Category**: STR_CAT_DISPLAY

### Rendering

A helper function `drawReadingProgressPercent(const GfxRenderer& renderer,
const int progressPercent)` formats the percentage as `"%d%%"` and draws it
centered at the bottom of the screen using `drawCenteredText` with
`UI_10_FONT_ID` (matching the shutdown caption font), at
`pageHeight - 20`.

The progress text is drawn in black ink (`black = true`), the same polarity
as the existing shutdown caption. It is drawn **before** any `invertScreen()`
call so the inversion flips it together with the cover, exactly like the
shutdown caption: text drawn after the inversion would stay black ink over
the already-flipped cover and become unreadable.

#### Sleep screen

`renderCoverSleepScreen()` computes the progress from
`APP_STATE.openEpubPath` and passes it through to `renderBitmapSleepScreen()`
via a new `progressPercent` parameter (default `-1` = no progress drawn).

Inside `renderBitmapSleepScreen()`, the progress is drawn **after**
`drawBitmap()` but **before** the inversion block, before the
`overlayCover == &bitmap` transparent-overlay path and before
`displayImageWithGrayscale()`. This places the text in the B/W framebuffer
layer; the grayscale passes only clear and repaint the gray planes, so the
B/W progress text survives. For the transparent overlay path the same
applies — the overlay renderers preserve the B/W background (`preserveBackground = true`),
so the progress remains visible underneath.

When progress is drawn, `displayImageWithGrayscale()` runs with
`allowAbsolutePlanes = false`: the absolute-plane pipeline repaints the whole
panel from the plane content alone, which would erase the progress text
living in the base framebuffer. The OEM fold pipeline keeps base content
(the same choice `renderShutdownImageFramed()` makes for its frame and
caption). Without progress text the absolute-plane pipeline is kept
unchanged.

Call sites that pass `-1` (no progress):
- `renderCustomSleepScreen()` — custom images, not covers
- `renderSleepOverlayFile()` — overlay images

Call sites that pass the computed progress:
- `renderCoverSleepScreen()` — cover-based sleep screens

#### Shutdown screen

`renderShutdownScreen()` computes the progress when `useCover` is true
(same condition that selects the cover path) and passes it to
`renderShutdownImageFramed()` via a new `progressPercent` parameter
(default `-1`).

Inside `renderShutdownImageFramed()`, the progress is drawn **after**
`drawShutdownCaption()` and **before** `invertScreen()`. The Y position is
`frameY + frameH + 8` — a small gap below the image frame, above the
shutdown caption (which sits at `frameY + frameH + 24`).

### Files to change

| File | Change |
|------|--------|
| `src/CrossPointSettings.h` | Add `uint8_t sleepScreenCoverShowProgress = 1;` after `sleepScreenCoverFilter` |
| `src/SettingsList.cpp` | Add `SettingInfo::Toggle` for the new field |
| `lib/I18n/I18nKeys.h` | Add `STR_SLEEP_COVER_PROGRESS` after `STR_SLEEP_COVER_FILTER` |
| `lib/I18n/translations/english.yaml` | Add `"Show Reading Progress on Cover"` |
| `src/activities/boot_sleep/SleepActivity.h` | Add `drawReadingProgressPercent` friend/helper declaration; update `renderBitmapSleepScreen` / `renderShutdownImageFramed` signatures |
| `src/activities/boot_sleep/SleepActivity.cpp` | Add `#include "util/BookProgress.h"` + `<cstdio>`; implement `drawReadingProgressPercent`; update the two render functions |
| `src/CrossPointState.h` / `.cpp` | Add `int8_t openBookProgressPercent = -1` cached reading progress + JSON round-trip |
| `src/activities/reader/ReaderActivity.cpp` | Seed `openBookProgressPercent` from `getScreenshotInfo()` in `onExit()` before `saveToFile()` |

### Scope: non-cover modes

Progress is only shown for cover-based sleep/shutdown screens. The CUSTOM
sleep screen, TRANSPARENT_CUSTOM, DEFAULT, BLANK, and LIGHT modes do not
show progress — the progress is specific to the reading session's cover art.

### Edge cases

- **No book open** (`APP_STATE.openEpubPath` empty): progress is `-1`,
  nothing is drawn.
- **Book not in stats** (`loadBookProgress` returns 0): progress is `0%`,
  shown as a valid value.
- **Progress at 100%**: shown as `100%`.
