# Frontlight Side-Swipe Gestures

## Summary

Two new touch gestures on the e-ink reader screen that control the frontlight directly while reading:

- **Left side, vertical slide → frontlight color temperature** (warmth). Sliding up warms the light (more warm channel), sliding down cools it (more cool channel).
- **Right side, vertical slide → frontlight brightness**. Sliding up increases brightness, sliding down decreases it. Sliding all the way down to 0 turns the frontlight off.

These gestures work independently of the `touchReaderControls` setting and are gated at compile time by `FREEINK_CAP_FRONTLIGHT`. They are disableable via a new Settings toggle.

## Design

### Gate: compile-time + runtime

- **Compile-time**: The entire feature is wrapped in `#if FREEINK_CAP_FRONTLIGHT`. The `FREEINK_CAP_FRONTLIGHT` macro is already defined in `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h` (line 201) and derived from `FREEINK_DEVICE_*` per env. Boards without a frontlight (X4, X3, X4C, OnePage) compile the code out entirely. No new board-features-pipeline entry needed — the existing macro suffices.
- **Runtime**: A new setting `frontlightSideGestures` (uint8_t, 0 = Off, 1 = On, default 1 on frontlight boards). When Off, the gestures do nothing even on frontlight-equipped devices. Persisted in `settings.json` as `"frontlightSideGestures"`.
- **Gated on active reader surface**: The handler also checks `overlay == Overlay::None && !endOfBookMenuActive()` so the gesture cannot fire while the toolbar menu, footnotes overlay, or end-of-book menu is shown. The edge swipe is still consumed (returns true) so it doesn't fall through to page-turn logic, but the frontlight value is not changed while an overlay owns input.

### Gate: independent of `touchReaderControls`

The feature must work regardless of `SETTINGS.touchReaderControls` (Off / Tap / Swipe / Inverted Tap). The existing `detectTouchPageTurn()` in `ReaderUtils.h` checks `touchReaderControls` and returns early when it's `TOUCH_READER_OFF`. The new side-swipe handler runs as a **separate check** after `detectTouchPageTurn()`, and does NOT read `touchReaderControls` at all. If a vertical swipe on the left/right edge is detected, the frontlight is adjusted and the page-turn path is skipped for that frame (the swipe is consumed).

### Gesture detection

The existing `wasSwipe()` / `decodeSwipe()` pipeline in `MappedInputManager` already returns logical-screen coordinates for a completed swipe (start + end). We add one new public method:

```cpp
// In MappedInputManager (MappedInputManager.h / .cpp):
// Returns true when `decodeSwipe()` identified a completed vertical swipe
// whose START point falls within the left or right edge band.
//   leftSide   – true if the swipe started on the left edge band
//   up         – true if the swipe moved upward (start.y > end.y)
//   distancePx – vertical travel in logical pixels (|start.y - end.y|),
//                used to compute the frontlight step size
bool wasSideSwipe(bool& leftSide, bool& up, int& distancePx) const;
```

**Detection logic** (in `wasSideSwipe()`, using `decodeSwipe()` output):
- The swipe must have a start point (`sx`, `sy`) from `decodeSwipe()`.
- Left side: `sx < screenWidth * SIDE_BAND` (SIDE_BAND = 0.20 = left 20%).
- Right side: `sx > screenWidth * (1.0 - SIDE_BAND)`.
- The swipe must be primarily vertical: `ady > adx` (vertical axis dominant), which also guarantees it won't conflict with the horizontal back-gesture (which requires `dx > 0` with `dx > dy`).
- The swipe must be a valid completed swipe — `decodeSwipe()` already enforces the SDK's minimum travel threshold (60px in physical touch units, applied to the raw gesture), so no additional threshold is needed.
- `distancePx = ady` (the raw vertical delta returned by `decodeSwipe()`).

**Why a completed swipe, not continuous drag?** E-ink refresh is slow (1–2s per frame). A continuous drag would queue frame updates that outpace the display. A completed vertical swipe that maps its travel distance to a percentage change is the established pattern (matches how `wasSwipe()` already drives page turns). The magnitude of the vertical travel as a fraction of screen height determines the step size:

- `step = clamp(round(ady / screenHeight * 100), 1, 100)` — a full-height swipe = ±100% step.
- Warmth: `up ? +step : -step` applied to `SETTINGS.frontlightWarmth` (clamped 0–100).
- Brightness: `up ? +step : -step` applied to `SETTINGS.frontlightBrightness` (clamped 0–100). If the result is 0, the light is turned off via `setOn(false)`.

### Where the handler lives

The handler runs in `EpubReaderActivity::loop()`, at the same call site as `detectTouchPageTurn()` (line 629 of `EpubReaderActivity.cpp`). It runs **after** `detectTouchPageTurn()` but **before** the overlay check and page-turn logic, so a vertical side-swipe that changes the frontlight does not also trigger a page turn:

```cpp
// In EpubReaderActivity::loop(), after line 629:
#if FREEINK_CAP_FRONTLIGHT
if (SETTINGS.frontlightSideGestures && Frontlight.present() &&
    overlay == Overlay::None && !endOfBookMenuActive()) {
  if (handleSideSwipeFrontlight()) {
    return;
  }
}
#endif
```

`handleSideSwipeFrontlight()` calls `mappedInput.wasSideSwipe(leftSide, up, distancePx)` and, if true, applies the brightness/warmth change and calls `Frontlight.setBrightness()` / `Frontlight.setWarmth()` directly, updates the in-memory `SETTINGS` fields, and returns (skipping the page-turn for this frame).

### Setting declaration

In `SettingsList.h`, add a new `Toggle` entry in the Display category, gated by `FREEINK_CAP_FRONTLIGHT`:

```cpp
#if FREEINK_CAP_FRONTLIGHT
SettingInfo::Toggle(StrId::STR_FRONTLIGHT_SIDE_GESTURES, &CrossPointSettings::frontlightSideGestures,
                    "frontlightSideGestures", StrId::STR_CAT_DISPLAY),
#endif
```

In `CrossPointSettings.h`, add the field after the existing frontlight fields:

```cpp
uint8_t frontlightSideGestures = 1;
```

In `english.yaml`, add:

```yaml
STR_FRONTLIGHT_SIDE_GESTURES: "Frontlight Side Gestures"
```

### i18n

New string key `STR_FRONTLIGHT_SIDE_GESTURES` added to `english.yaml`. Other languages auto-fill from English via `gen_i18n.py`. The key is only surfaced on boards with `FREEINK_CAP_FRONTLIGHT` (gated in `SettingsList.h`), so non-frontlight boards never see an untranslated label.

## Files to change

| File | Change |
|------|--------|
| `lib/I18n/translations/english.yaml` | Add `STR_FRONTLIGHT_SIDE_GESTURES` |
| `src/CrossPointSettings.h` | Add `frontlightSideGestures` field (generic JSON loop handles persistence) |
| `src/SettingsList.h` | Add the Settings row, gated by `FREEINK_CAP_FRONTLIGHT` |
| `src/MappedInputManager.h` | Add `wasSideSwipe(bool& leftSide, bool& up, int& distancePx) const` declaration |
| `src/MappedInputManager.cpp` | Implement `wasSideSwipe()` using `decodeSwipe()` + edge math |
| `src/activities/reader/EpubReaderActivity.h` | Add `handleSideSwipeFrontlight()` private method |
| `src/activities/reader/EpubReaderActivity.cpp` | Add `handleSideSwipeFrontlight()` implementation + call in `loop()` |
| `README.md` | Document the feature under fork features |
| `USER_GUIDE.md` | Document the gesture in the reading-gestures section |

## Persistence

The frontlight brightness, warmth, and on/off state are already persisted via `SETTINGS.frontlightBrightness`, `SETTINGS.frontlightWarmth`, `SETTINGS.frontlightOn`. The side-swipe handler writes to these same in-memory `SETTINGS` fields and applies the hardware change immediately via `Frontlight.setBrightness()` / `Frontlight.setWarmth()` / `Frontlight.setOn()`. It does **not** call `SETTINGS.saveToFile()` on every swipe — doing so would cause SD-card write stalls on the e-ink render path. Persistence is deferred to the next natural settings-save point (sleep, Home, Settings screen), matching the `FrontlightPanelActivity::persistLightSettings()` pattern where live adjustments are only flushed to disk on `onExit()`.

## Orientation handling

`decodeSwipe()` already maps normalized touch coordinates through `renderer.tapToLogical()`, which applies the current orientation transform. So left/right and up/down are correct in all four orientations (Portrait, Inverted, Landscape CW, Landscape CCW).

## Conflict avoidance with existing gestures

- **Top-edge down-swipe** → opens FrontlightPanelActivity (consumed by `ActivityManager::loop()` before the reader's `loop()`). Not affected — the new side swipes start on the left/right edges, not the top edge.
- **Left-edge swipe (back gesture)** → consumed by `ActivityManager::loop()` via `wasBackGesture()`. The back gesture requires `dx > 0` (horizontal, moving right) with `dx > dy`. A vertical side-swipe has `ady > adx`, so `wasSwipe()` returns `Left`/`Right` only for horizontal swipes. The new `wasSideSwipe()` checks `ady > adx` exclusively, so it won't fire on a back-gesture.
- **Horizontal swipe in the reader (page turn)** → `detectTouchPageTurn()` consumes `SwipeDir::Left`/`Right`. The new vertical side-swipe handler only fires for vertical swipes (`up`/`down`), so there is no conflict. If a vertical side-swipe is detected, the handler returns early and skips the page-turn path entirely (the swipe is consumed).

## Brightness "all the way down disables it"

When a downward swipe on the right edge would reduce brightness to 0, the light is turned off via `Frontlight.setOn(false)` rather than setting brightness to 0 while staying "on". This matches the existing `toggleFrontlightByShortcut()` pattern and the `FrontlightPanelActivity`'s lamp button behavior. When a subsequent upward brightness swipe is detected while the light is off, the handler calls `Frontlight.setOn(true)` to wake the hardware (matching `FrontlightPanelActivity::onBrightnessEvent()`).

## Open questions

1. **Step size granularity**: Full-height swipe = ±100 step. A small swipe = ±1 step. This maps naturally to the brightness percentage. Alternative: fixed step (±5) regardless of swipe distance. The distance-proportional approach gives fine control for small gestures and fast full-range adjustment for large ones, matching the slider mental model.

2. **Warmth on single-channel boards**: `Frontlight.hasColorTemperature()` is false on single-channel frontlights (e.g., de-link, LilyGo). On those boards, `setWarmth()` is a no-op (the `FrontlightManager::setColorTemperature` body is empty without `FREEINK_CAP_WARMLIGHT`). The left-side gesture still consumes the touch and updates the saved `frontlightWarmth` setting (so it's correct when the device is moved to a warm/cool board), but the hardware doesn't change. This is acceptable — the `FRONT_LIGHT_ON` build flag already handles single-channel vs. warm/cool at the hardware level.

## Verification

- `pio run -e x4pro` builds (X4 Pro has warm/cool frontlight + touch). ✓
- `pio run -e default` builds (X4 — no frontlight, code compiles out). ✓
- `pio run -e papermono` builds (Paper Mono has frontlight but no warm channel). ✓
- `pio run -e x4c` builds (X4C — no frontlight, no touch).
- On x4pro: verify left-side up-swipe warms the light, down-swipe cools it; right-side up-swipe brightens, down-swipe dims; brightness at 0 turns the light off; re-swiping up restores brightness; gesture works with `touchReaderControls` set to Off; gesture does not fire when overlay menu or end-of-book menu is open.
- clang-format: no changes. ✓
- cppcheck: no new warnings. ✓
