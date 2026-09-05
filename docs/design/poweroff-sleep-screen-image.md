# Poweroff screen follows the Sleep Screen setting

Status: design (reviewed, pre-implementation)
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
   flips the whole shutdown screen — frame and caption included, exactly as
   sleep's full-screen art flips (white-on-black after the flip; sleep's own
   DARK logo screen inverts the same way, SleepActivity.cpp:615-617).
   (Decision 2026-09-05.)
6. Cover staging must stop paying for modes that will not use it (skip EPUB
   cover generation on every power off in `CUSTOM`/`BLANK`/logo modes).

## Non-goals

- No change to the normal sleep screen rendering.
- No new user-facing settings or strings.
- QUICK_RESUME and TRANSPARENT_CUSTOM do not get poweroff-specific art.

Polarity: the shutdown path keeps today's timer-wake behavior (cold boot,
always normal output polarity — `display.begin()` at main.cpp:760 precedes the
render at :767 and nothing sets inversion before it). On the manual path,
`renderShutdownScreen` additionally normalizes output polarity with
`display.setInverted(false)` at the top — the same one-liner
`SleepActivity::onEnter` uses (SleepActivity.cpp:503) — so under night mode a
manual power off renders `BLANK` as a white panel with a readable caption
rather than today's inverted render (which the new `BLANK` mode would make a
glaring solid-black panel). No other night-mode handling is added.

## Design

### Mode → image mapping

| `sleepScreen` mode | Poweroff screen |
|---|---|
| `DARK`, `LIGHT` | Light logo + caption (today's no-image path) |
| `QUICK_RESUME`, `TRANSPARENT_CUSTOM` | Light logo + caption (fallback) |
| `BLANK` | Blank screen + caption |
| `CUSTOM` | `/sleep.bmp`, else random valid BMP from `/.sleep`, else `/sleep` — drawn inside the frame, caption below |
| `COVER` | Staged cover BMP inside the frame; no/stale cover → logo + caption |
| `COVER_CUSTOM` | Sleeping from reader → staged cover BMP inside the frame; from home → `CUSTOM` image inside the frame; cover missing/stale → `CUSTOM` → logo; no sleep files → logo + caption |

`COVER_CUSTOM` mirrors sleep's from-reader branch (SleepActivity.cpp:546-551,
`APP_STATE.lastSleepFromReader`) instead of the earlier cover-first draft:
poweroff should agree with what the user just saw on the sleep screen. The
flag is fresh on the manual path (`enterPowerOff` sets
`APP_STATE.lastSleepFromReader = activityManager.isReaderActivity()` before
staging, mirroring `enterDeepSleep` main.cpp:414) and persisted for the
timer-wake path (saved at `enterDeepSleep` main.cpp:429, loaded at
main.cpp:666 before the shutdown render).

### Code touch points

- `src/activities/boot_sleep/SleepActivity.cpp` (header unchanged)
  - The framed shutdown path cannot reuse the sleep render helpers
    themselves — they render full-screen, and covers come from the staged
    path rather than live resolution — so the shutdown branches live in
    `renderShutdownScreen` plus file-local helpers. The shared surface is:
    `selectRandomSleepFile` (extended with a `commitRecent` flag, sleep
    callers unchanged), `calculateBitmapPlacementInBounds` (unchanged), and
    a new file-local `flushGrayscaleImage` extracted from
    `renderBitmapSleepScreen`'s three-pass grayscale tail (bullet below).
    The shutdown path still never constructs a `SleepActivity`.
  - Single-source the three-pass grayscale flush: extract the
    `displayGrayscaleBase(HALF)` → per-plane `bitmap.rewindToData()` →
    `clearScreen(0x00)` → `setRenderMode(LSB/MSB)` → `drawBitmap` →
    `copyGrayscale*Buffers()` → `displayGrayBuffer()` → `setRenderMode(BW)`
    tail into one shared helper taking the placement bounds, used by both
    `renderBitmapSleepScreen` (full-screen placement, sleep) and the framed
    shutdown helper (inset placement). The final display call is
    branch-dependent: grayscale branch ends in `displayGrayBuffer()`, BW
    branch in `displayBuffer(HALF)`.
  - Base survival outside the image rect is guaranteed by both controller
    families: on SSD1677 (X4 / X4 Classic) the gray nudge LUT's `(BW=0,
    RED=0)` selector group is an all-zero hold waveform — pixels unmarked in
    both planes are not driven (freeink-sdk `Ssd1677Luts.h`); on UC8279 (X4
    Pro) `displayStart` snapshots the base framebuffer into `_grayBase` and
    `copyGrayscale*` folds it into the absolute planes (`plane0 = base |
    lsbMask`, `plane1 = plane0 ^ msbMask`, Uc8279X4Driver.cpp:391-394, :537,
    :568). This is the same compositing the transparent-overlay path
    (SleepActivity.cpp:349-373) and reader glyph AA already rely on.
  - Framed shutdown helper: clear → draw bitmap at
    `calculateBitmapPlacementInBounds` (2/3 frame, inset 6 — constants
    preserved) → `drawRect` frame → caption (same two-line split at `". "`)
    → `invertScreen()` when the filter is `INVERTED_BLACK_AND_WHITE` (after
    frame + caption so the whole screen flips) → shared flush tail.
    Grayscale passes only when the bitmap carries grayscale AND the filter is
    `NO_FILTER` — same predicate as sleep's non-preserved path.
  - CUSTOM images render with `Bitmap(file, /*dithering=*/true)` (same as
    sleep); covers keep the undithered `Bitmap(file)` (Bitmap.h:67 default).
  - Shutdown `CUSTOM` selection must not mutate sleep state: extend
    `selectRandomSleepFile` with a `commitRecent` flag (default `true`, sleep
    unchanged). The shutdown path passes `false` — no `pushRecentSleepIndex`
    (the next sleep's exclusion window must not be skewed by art shown at
    shutdown) and no `APP_STATE.saveToFile()` (the timer-wake path otherwise
    never persists; the write would directly delay the rail cut).
  - `renderShutdownScreen(renderer)`: keep signature; normalize polarity
    (`display.setInverted(false)`, see Polarity above); select the branch
    from `SETTINGS.sleepScreen` per the table; clear
    `APP_STATE.autoPowerOffCoverBmpPath` after paint (unchanged lifecycle —
    and still the only consumer of that field besides persistence).
- `src/main.cpp`
  - `stageAutoPowerOffCover()`: stage only when the mode can use a cover —
    `COVER`, or `COVER_CUSTOM` with `lastSleepFromReader` (the flag is set at
    main.cpp:414 before staging at :427). The clear stays unconditional so a
    stale path from a previous setting never leaks into a later render.
    `requireTimerEnabled` semantics at both call sites (`enterDeepSleep`
    :427 requires the auto-power-off timer; `enterPowerOff` :467 does not)
    are unchanged.
  - `enterPowerOff()`: set `APP_STATE.lastSleepFromReader =
    activityManager.isReaderActivity()` before staging (fresh flag for the
    `COVER_CUSTOM` branch on the manual path; persisted by the existing save
    at :475).
  - Safety: `APP_STATE.loadFromFile()` (main.cpp:666) and
    `SETTINGS.loadFromFile()` (:682) both run before the timer-wake shutdown
    render (:766-777), so the staged cover path and the `sleepScreen` switch
    are valid on the wake path too.
- `src/CrossPointState.h/.cpp` — unchanged; `autoPowerOffCoverBmpPath` and
  `lastSleepFromReader` keep their persisted keys and lifecycles.
- `USER_GUIDE.md` — one sentence in the power-off section: the poweroff
  screen shows the picture configured by the Sleep Screen setting.

### Cost / memory

- Net negative RAM/heap: no new buffers beyond what the existing paths
  allocate (`Bitmap` row streaming, gray work planes reused as today; the
  gray flush tail is extracted, not duplicated).
- Net negative CPU/SD: in non-cover modes the EPUB cover is no longer parsed
  and the cover BMP no longer generated at power off; the shutdown `CUSTOM`
  path drops the `APP_STATE.saveToFile()` SD write entirely.
- `CUSTOM`/`COVER_CUSTOM`-from-home pays the sleep path's existing two-pass
  folder scan (O(N) BMP header parses + `delay(100)`) before
  `display.deepSleep()`/rail cut; identical cost class to today's sleep
  render, but at poweroff it directly delays shutdown while the panel shows
  the previous frame. Accepted — poweroff is not latency-critical and the
  scan only runs when no staged cover exists.
- Flash: the mapping switch plus the framed helper (frame + caption + branch)
  — the gray flush sequence itself is shared, not copied.

## Design review pass applied (OpenCode kimi-k3, 2026-09-05)

All seven code claims verified against sources; no blockers. Findings folded
in: F1 superseded — the static-conversion shape was replaced after
re-derivation by file-local shutdown helpers sharing `selectRandomSleepFile`
and the extracted gray flush tail (smaller surface, no header churn),
F2 grayscale compositing mechanism documented per controller family,
F4 `SETTINGS.loadFromFile()` ordering note, F7 manual-path polarity
normalization via `setInverted(false)`, F9 `commitRecent` split (no
recent-window push, no SD write from the shutdown path), F10 latency
acceptance, F11 shared gray flush tail + `x4c` verification, F12
`COVER_CUSTOM` from-reader parity with fresh/persisted `lastSleepFromReader`.

## Verification

- `pio run -e x4pro` and `pio run -e x4c` green (grayscale compositing
  differs: SSD1677 hold-LUT vs UC8279 absolute-plane fold);
  `./bin/clang-format-fix -g` clean.
- Manual matrix (device, human tester): modes {DARK, LIGHT, CUSTOM with
  `/sleep.bmp`, CUSTOM with `/sleep/` folder, COVER in book, COVER from home,
  COVER_CUSTOM in book, COVER_CUSTOM from home, COVER_CUSTOM with no sleep
  files, BLANK, QUICK_RESUME, TRANSPARENT_CUSTOM} × {manual power off, auto
  power off timer}, plus one pass with filter = Inverted. The framed
  grayscale case (CUSTOM grayscale BMP or grayscale cover with `NO_FILTER`)
  must be eyeballed on both an X4 Pro and an X4 Classic.

## Alternatives considered

- **Full parity incl. DARK inversion** — rejected (user decision): the caption
  must stay readable; a dark inverted shutdown screen reads worse and needs a
  light-colored caption. (The `INVERTED_BLACK_AND_WHITE` filter still flips
  the screen — that is the user's explicit filter choice, applied exactly as
  sleep applies it.)
- **QUICK_RESUME/TRANSPARENT treated as CUSTOM** — rejected (user decision):
  quick-resume art is the retained panel frame and overlays composite over an
  existing screen; neither exists after a cold power cut.
- **Duplicate selection logic in the shutdown path** — rejected: the shutdown
  branches share `selectRandomSleepFile` and the extracted gray flush tail
  with sleep, keeping one source of truth for `/sleep.bmp` → `/.sleep` →
  `/sleep` resolution and the grayscale sequence.
- **Duplicate the gray flush sequence in the framed helper** — rejected
  (review F11): the three-pass sequence exists exactly once; the framed case
  differs only in placement bounds.
- **Cover-first `COVER_CUSTOM` regardless of context** — rejected (review
  F12): disagreed with sleep's from-reader branch; poweroff now mirrors what
  the sleep screen actually showed.
