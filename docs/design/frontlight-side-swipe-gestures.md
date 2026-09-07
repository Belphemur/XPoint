# Frontlight Side-Swipe Gestures

## Summary

Two new touch gestures on the e-ink reader screen that control the frontlight directly while reading:

- **Left side, vertical slide → frontlight color temperature** (warmth). Sliding up warms the light (more warm channel), sliding down cools it (more cool channel). Only effective on dual-channel boards (X4 Pro).
- **Right side, vertical slide → frontlight brightness**. Sliding up increases brightness, sliding down decreases it. Sliding all the way down turns the light off; swiping up from the minimum restores it. The brightness swipe behaves identically to the frontlight panel's slider: it clamps to 1% minimum (FRONTLIGHT_MIN_BRIGHTNESS). The left-edge warmth gesture is accepted on single-channel boards but has no visual effect.

These gestures work independently of the `touchReaderControls` setting and are gated at compile time by `FREEINK_CAP_FRONTLIGHT`. They are disableable via a new Settings toggle.

## Design

### Gate: compile-time + runtime

- **Compile-time**: The entire feature is wrapped in `#if FREEINK_CAP_FRONTLIGHT`. The `FREEINK_CAP_FRONTLIGHT` macro is already defined in `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h` (line 201) and derived from `FREEINK_DEVICE_*` per env. Boards without a frontlight (X4, X3, X4C, OnePage) compile the code out entirely. No new board-features-pipeline entry needed — the existing macro suffices.
- **Runtime**: A new setting `frontlightSideGestures` (uint8_t, 0 = Off, 1 = On, default 1 on frontlight boards). When Off, the gestures do nothing even on frontlight-equipped devices. Persisted in `settings.json` as `"frontlightSideGestures"`.
- **Gated on active reader surface**: The handler also checks `overlay == Overlay::None && !endOfBookMenuActive()` so the gesture cannot fire while the toolbar menu, footnotes overlay, or end-of-book menu is shown.

### Gate: independent of `touchReaderControls`

The feature must work regardless of `SETTINGS.touchReaderControls` (Off / Tap / Swipe / Inverted Tap). The existing `detectTouchPageTurn()` in `ReaderUtils.h` checks `touchReaderControls` and returns early when it's `TOUCH_READER_OFF`. The new side-swipe handler runs as a **separate check** after `detectTouchPageTurn()`, and does NOT read `touchReaderControls` at all.

### Why continuous drag, not completed swipe?

The SDK's `wasSwipe`/`decodeSwipe` pipeline requires 60px minimum travel before it fires as a "swipe". On a 1448px screen, 60px maps to ~4% — too coarse for night-time fine adjustment where 1% precision matters. Instead of using `wasSideSwipe()` (which calls `decodeSwipe`), the handler uses continuous touch tracking across `loop()` frames:

- `wasScreenTouchDown()` fires once on touch-down; if the touch starts within the left or right 20% edge band, a frontlight drag begins.
- `isScreenTouchHeld()` reports the live touch position each frame; the vertical delta from the previous frame is mapped directly to a frontlight step (1px = 1%).
- `wasScreenTouchReleased()` ends the drag.

The vertical delta is accumulated frame-to-frame (baseline resets each loop), so a slow small stroke produces small 1% increments and a fast long stroke produces larger steps — exactly the proportional control the user needs at night.

### Gesture detection and step sizing

- Touch starts in the left 20% of screen width → **warmth** (leftSide = true).
- Touch starts in the right 20% of screen width → **brightness** (leftSide = false).
- Touch starts in the middle 60% → not a frontlight gesture (falls through to page turn / tap).
- Each `loop()` frame: `deltaY = currentY - previousY`. `step = abs(deltaY)`. `up = (deltaY < 0)` (finger moved up).
- **Warmth**: `next = SETTINGS.frontlightWarmth + (up ? step : -step)`, clamped to `[0, 100]`. Calls `Frontlight.setWarmth()`. No re-render.
- **Brightness**: `next = SETTINGS.frontlightBrightness + (up ? step : -step)`.
  - If `next <= 0`: turns the light off via `Frontlight.setOn(false)` (preserves `lastBrightness` so the panel slider and swipe restore to the same value). `SETTINGS.frontlightOn = 0`.
  - If `next > 0`: clamps to `[FRONTLIGHT_MIN_BRIGHTNESS, 100]`. If the light was off, restores it via `Frontlight.setOn(true)`. Calls `Frontlight.setBrightness()`.

`FRONTLIGHT_MIN_BRIGHTNESS` is defined in `HalFrontlight.h` as a shared constant, and `FrontlightPanelActivity.cpp` uses it too — so both controls agree on the 1% floor.

### No re-render

`handleSideSwipeFrontlight()` does NOT call `requestUpdate()`. The `Frontlight.setBrightness/setWarmth/setOn` calls push new PWM duty values to the hardware immediately (confirmed by `[FrontlightMgr] apply` logs). A full e-ink page re-render is triggered by `requestUpdate()`, but the page content is unchanged — only the frontlight PWM changed. Triggering a re-render adds ~1.1s of display refresh for zero visual benefit.

### No per-swipe settings save

`handleSideSwipeFrontlight()` does NOT call `SETTINGS.saveToFile()`. The SD write on every swipe causes render-path stalls (see `PersistableStore.h`). The in-memory `SETTINGS` fields are already correct and the frontlight hardware is updated immediately. Persistence happens naturally at the next settings-save point (sleep, Home, Settings screen), matching `FrontlightPanelActivity`'s live-on-exit pattern.

### Where the handler lives

The handler runs in `EpubReaderActivity::loop()`, after `detectTouchPageTurn()` but before the overlay check and page-turn logic:

```cpp
// In EpubReaderActivity::loop():
#if FREEINK_CAP_FRONTLIGHT
if (SETTINGS.frontlightSideGestures && Frontlight.present() &&
    overlay == Overlay::None && !endOfBookMenuActive()) {
  if (handleSideSwipeFrontlight()) {
    return;
  }
}
#endif
```

`handleSideSwipeFrontlight()` manages its own drag state (`frontlightDrag` struct, gated by `#if FREEINK_CAP_FRONTLIGHT`). On touch-down it checks the edge band; on each held frame it applies the delta; on release it resets. Returns true when the gesture is consumed (skips page-turn for that frame).

### Files changed

1. `lib/hal/HalFrontlight.h` — add `FRONTLIGHT_MIN_BRIGHTNESS` shared constant.
2. `src/CrossPointSettings.h` — add `frontlightSideGestures` field.
3. `src/SettingsList.h` — add `Toggle` entry, gated by `FREEINK_CAP_FRONTLIGHT`, in Display category.
4. `src/MappedInputManager.h` — remove `wasSideSwipe` (replaced by continuous tracking).
5. `src/MappedInputManager.cpp` — remove `wasSideSwipe` implementation.
6. `src/activities/reader/EpubReaderActivity.h` — add `handleSideSwipeFrontlight()` declaration, add `FrontlightDragState` member.
7. `src/activities/reader/EpubReaderActivity.cpp` — add `handleSideSwipeFrontlight()` with continuous drag tracking + call site in `loop()`.
8. `src/activities/util/FrontlightPanelActivity.cpp` — use shared `FRONTLIGHT_MIN_BRIGHTNESS` constant (was local `MIN_BRIGHTNESS`).
9. `lib/I18n/translations/english.yaml` — add `STR_FRONTLIGHT_SIDE_GESTURES`.
10. `README.md` — add feature #10 + touch gestures subsection.
11. `USER_GUIDE.md` — add frontlight side-gesture controls subsection + Display Settings entry.

### OpenCode design review (2026-09-07)

**Reviewer**: OpenCode glm 5.3 flash
**Result**: Reviewed (manual codebase investigation; OpenCode subagent was spawned but did not complete).

Findings:
- **Sound**: `FREEINK_CAP_FRONTLIGHT` is the correct compile-time gate; `Frontlight.present()` handles runtime inertness on virtual boards.
- **Sound**: Placement after `detectTouchPageTurn()` but before the overlay check is correct — vertical edge swipes don't trigger page turns.
- **Sound**: `HalFrontlight` API calls (`setOn`/`setBrightness`/`setWarmth`) match existing codebase patterns.
- **Sound**: Conflict avoidance with back-gesture (horizontal) and light-panel gesture (top edge) is architecturally sound.
- **Sound**: `setOn(false)` at brightness=0 matches `FrontlightPanelActivity::toggleLight()` pattern.
- **Note**: The step calculation maps swipe distance to 1–100; the SDK's `TOUCH_SWIPE_MIN_PX` (60px) was the minimum detectable swipe, which caused the ~4% floor issue — addressed by switching to continuous drag tracking.

### Verification

- `pio run -e x4pro` (ESP32-S3, frontlight + touch): **SUCCESS**
- `pio run -e default` (ESP32-C3, no frontlight): **SUCCESS**
- `pio run -e papermono` (Paper Mono, brightness-only frontlight): **SUCCESS**
- `pio run -e sticky` (ESP32-S3, no frontlight): **SUCCESS**
- `clang-format-fix -g`: no changes needed
- Host unit tests: pass
- CodeRabbit review: pass (no actionable findings)
- Copilot review: pass
