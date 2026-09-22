#include "MappedInputManager.h"

#include <BoardConfig.h>
#include <FreeInkUICore.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

void MappedInputManager::update(const bool deferHomeButtonAction) const {
  // Frame boundary for async input sampling (docs/design/2026-09-20-async-
  // Consume what the poll task produced (soak-fix7 consume-on-check):
  // gpio.update() is a no-op in async mode (edges move only through the
  // per-read pop); on sync builds update() keeps the historical one-shot
  // path and consumeTouchFrame() is a no-op. Then take the per-tick edge
  // snapshot — each physical button's edges consumed exactly once here,
  // served to every activity read this tick (multi-read safe).
  gpio.update();
  gpio.consumeTouchFrame();
  framePressedEdges = 0;
  frameReleasedEdges = 0;
  for (uint8_t physical = HalGPIO::BTN_BACK; physical <= HalGPIO::BTN_POWER; ++physical) {
    if (gpio.wasPressed(physical)) framePressedEdges |= static_cast<uint8_t>(1u << physical);
    if (gpio.wasReleased(physical)) frameReleasedEdges |= static_cast<uint8_t>(1u << physical);
  }
  resolvePowerDoubleClickWindow();
  homeAction = HomeButtonAction::Ignore;
  homeGesture = HomeButtonGesture::None;
  if (gpio.hasHomeKey()) {
    homeAction = homeButtonInput.update(millis(), gpio.wasHomeKeyTapped(), gpio.wasHomeKeyLongPressed(),
                                        wasSwipe() != SwipeDir::None, gpio.wasHomeKeyPressed(),
                                        static_cast<HomeButtonAction>(SETTINGS.homeButtonTapAction),
                                        static_cast<HomeButtonAction>(SETTINGS.homeButtonDoubleTapAction),
                                        static_cast<HomeButtonAction>(SETTINGS.homeButtonLongPressAction));
    homeGesture = homeButtonInput.lastGesture();
  }
  if (deferHomeButtonAction) {
    // Keep the first action observed during a synchronous transfer. Home must
    // still be visible now so the transfer can cancel and unwind promptly.
    if (homeAction != HomeButtonAction::Ignore && deferredHomeAction == HomeButtonAction::Ignore) {
      deferredHomeAction = homeAction;
      deferredHomeGesture = homeGesture;
    }
  } else if (deferredHomeAction != HomeButtonAction::Ignore) {
    // A fresh gesture is newer than anything captured during a blocking
    // transfer; otherwise replaying stale input could dispatch actions out of
    // order. Keep only the fresh action and drop the stale latch.
    if (homeAction != HomeButtonAction::Ignore) {
      deferredHomeAction = HomeButtonAction::Ignore;
      deferredHomeGesture = HomeButtonGesture::None;
    } else {
      homeAction = deferredHomeAction;
      homeGesture = deferredHomeGesture;
      deferredHomeAction = HomeButtonAction::Ignore;
      deferredHomeGesture = HomeButtonGesture::None;
    }
  }
  for (uint8_t value = 0; value <= static_cast<uint8_t>(Button::ScreenDown); ++value) {
    if (!isPressed(static_cast<Button>(value))) longPressFiredButtons &= ~(1u << value);
  }
}

bool MappedInputManager::isNavDirectionSwapped() const {
  // Touch boards always follow the rendered orientation; button-only boards keep the user toggle.
  // Home and settings render in portrait, so neither path swaps them.
  const auto orientation = renderer.getOrientation();
  return (gpio.hasTouch() || SETTINGS.frontButtonFollowOrientation) &&
         (orientation == GfxRenderer::PortraitInverted || orientation == GfxRenderer::LandscapeCounterClockwise);
}

MappedInputManager::Button MappedInputManager::mapScreenDirection(const Button button) const {
  // Rows follow GfxRenderer::Orientation's declared order.
  static constexpr Button directions[][4] = {
      {Button::Left, Button::Right, Button::Up, Button::Down},
      {Button::Down, Button::Up, Button::Left, Button::Right},
      {Button::Right, Button::Left, Button::Down, Button::Up},
      {Button::Up, Button::Down, Button::Right, Button::Left},
  };

  uint8_t direction = 0;
  switch (button) {
    case Button::ScreenLeft:
      direction = 0;
      break;
    case Button::ScreenRight:
      direction = 1;
      break;
    case Button::ScreenUp:
      direction = 2;
      break;
    case Button::ScreenDown:
      direction = 3;
      break;
    default:
      return button;
  }

  const uint8_t orientation =
      SETTINGS.frontButtonFollowOrientation ? static_cast<uint8_t>(renderer.getOrientation()) : 0;
  return directions[orientation][direction];
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  return mapButtonWith(button, [this, fn](const uint8_t physical) { return (gpio.*fn)(physical); });
}

bool MappedInputManager::edgeSnapshot(const Button button, const bool pressed) const {
  const uint8_t edges = pressed ? framePressedEdges : frameReleasedEdges;
  return mapButtonWith(button, [edges](const uint8_t physical) { return (edges & (1u << physical)) != 0; });
}

template <typename Probe>
bool MappedInputManager::mapButtonWith(const Button button, Probe&& probe) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return probe(SETTINGS.frontButtonBack);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return probe(SETTINGS.frontButtonConfirm);
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return probe(SETTINGS.frontButtonLeft);
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return probe(SETTINGS.frontButtonRight);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return probe(HalGPIO::BTN_UP);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return probe(HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return probe(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return probe(isNavDirectionSwapped() ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP);
        case CrossPointSettings::NEXT_PREV:
          return probe(isNavDirectionSwapped() ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return probe(isNavDirectionSwapped() ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN);
        case CrossPointSettings::NEXT_PREV:
          return probe(isNavDirectionSwapped() ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::NavNext:
      // Logical "next item" navigation: side Down + front Right, with the control axis flipped in
      // INVERTED / LANDSCAPE_CCW under the live orientation policy, matching the rotated hint labels.
      return isNavDirectionSwapped() ? (mapButtonWith(Button::Up, probe) || mapButtonWith(Button::Left, probe))
                                     : (mapButtonWith(Button::Down, probe) || mapButtonWith(Button::Right, probe));
    case Button::NavPrevious:
      // Logical "previous item" navigation: side Up + front Left, axis-flipped in the same orientations.
      return isNavDirectionSwapped() ? (mapButtonWith(Button::Down, probe) || mapButtonWith(Button::Right, probe))
                                     : (mapButtonWith(Button::Up, probe) || mapButtonWith(Button::Left, probe));
    case Button::ScreenLeft:
    case Button::ScreenRight:
    case Button::ScreenUp:
    case Button::ScreenDown:
      return mapButtonWith(mapScreenDirection(button), probe);
  }

  return false;
}

namespace {
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
}  // namespace

bool MappedInputManager::hasTouch() const { return gpio.hasTouch(); }

bool MappedInputManager::rawInputPriority() {
  // Snapshot masks, not gpio.wasAny*: in async mode the SDK's pending
  // counts are empty once update()'s snapshot consumed the edges (report-
  // only leftovers), so the button component must read this tick's masks.
  return framePressedEdges != 0 || frameReleasedEdges != 0 || gpio.wasTouchActivity() || gpio.isTouchContactActive() ||
         gpio.rawInputActive();
}

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenLongPress(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchLongPress(nx, ny)) return false;
  // Consuming the long-press implies acting on it: suppress the rest of the
  // contact so the finger lift can't also tap whatever the action opened.
  gpio.suppressTouchContact();
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  // Live contact position while the finger is down (no tap-slop gate) — drag tracking.
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchReleased() const { return gpio.wasTouchReleased(); }

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c >= colCount) return false;
    if (colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  switch (fui::swipeDirection(sx, sy, ex, ey)) {
    case fui::SwipeDir::Left:
      return SwipeDir::Left;
    case fui::SwipeDir::Right:
      return SwipeDir::Right;
    case fui::SwipeDir::Up:
      return SwipeDir::Up;
    case fui::SwipeDir::Down:
      return SwipeDir::Down;
    default:
      return SwipeDir::None;
  }
}

// Edge classification (which swipe counts as an edge gesture) lives in the
// SDK; only the MEANING of each edge — back, menu, home, light panel, and the
// home-key remap — is decided here.
bool MappedInputManager::wasEdgeSwipe(const freeink::ui::ScreenEdge edge) const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = fui::edgeSwipe(edge, sx, sy, ex, ey, renderer.getScreenWidth(), renderer.getScreenHeight());
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasBackGesture() const {
  // Back = left-to-right swipe starting near the left edge. Edge-anchored so that
  // mid-screen horizontal swipes stay available to activities that consume
  // SwipeDir::Left/Right (e.g. percent selection, image viewer).
  return wasEdgeSwipe(fui::ScreenEdge::Left);
}

bool MappedInputManager::wasTopEdgeDownSwipe() const { return wasEdgeSwipe(fui::ScreenEdge::Top); }

bool MappedInputManager::wasBottomEdgeUpSwipe() const { return wasEdgeSwipe(fui::ScreenEdge::Bottom); }

bool MappedInputManager::wasMenuGesture() const { return wasTopEdgeDownSwipe(); }

bool MappedInputManager::wasReaderMenuSwipeUp() const { return gpio.hasHomeKey() && wasBottomEdgeUpSwipe(); }

bool MappedInputManager::wasHomeGesture() const {
  return gpio.hasHomeKey() ? homeAction == HomeButtonAction::Home : wasBottomEdgeUpSwipe();
}

bool MappedInputManager::wasLightPanelGesture() const {
  // On lightless boards the same edge remains available to the reader menu.
  return Frontlight.present() && wasTopEdgeDownSwipe();
}

void MappedInputManager::resolvePowerDoubleClickWindow() const {
#if FREEINK_CAP_TOUCH
  if (!BoardConfig::isX4Pro() || !SETTINGS.doubleClickPwrLight) return;
  const unsigned long now = millis();
  // Per-tick verdict reset: powerConfirmClickFrame is an edge (one Confirm
  // per resolved window), not a level — without this the first expiry
  // latches it and PWR_CONFIRM replays Confirm every frame forever (the
  // replay regression this PR fixes, resurfaced on the expiry path; the
  // per-tick clear used to live in main.cpp, which the window migration
  // absorbed).
  powerConfirmClickFrame = false;
  const bool physicalRelease = (frameReleasedEdges & (1u << HalGPIO::BTN_POWER)) != 0;
  const bool comboRelease = physicalRelease && (frameReleasedEdges & (1u << HalGPIO::BTN_DOWN)) != 0;
  // PWR_CONFIRM carve-out threshold; 0 disables the carve-out.
  const uint32_t confirmHoldMs =
      SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM ? SETTINGS.getPowerButtonDuration() : 0;
  const auto result = PowerClickWindow::tick(powerReleaseWindowStart, physicalRelease, comboRelease, now,
                                             gpio.getPowerButtonHeldTime(), confirmHoldMs);
  if (result.serveRelease && result.holdRelease) {
    // Expiry + a new short click: the deferred release resolves but the NEW
    // physical release re-arms its own window — one mask bit cannot serve
    // and hold at once, so the bit stays held and delivers at the new
    // window's resolution (kody review, S11).
    frameReleasedEdges &= static_cast<uint8_t>(~(1u << HalGPIO::BTN_POWER));
  } else if (result.serveRelease) {
    frameReleasedEdges |= static_cast<uint8_t>(1u << HalGPIO::BTN_POWER);
  } else if (physicalRelease) {
    // Held (armed) or consumed by a double-click: the release stays out of
    // the served mask.
    frameReleasedEdges &= static_cast<uint8_t>(~(1u << HalGPIO::BTN_POWER));
  }
  if (result.doubleClick) powerDoubleClickFrame = true;
  if (result.confirmEdge) powerConfirmClickFrame = true;
#endif
}

bool MappedInputManager::isPowerClickHoldCandidate() const {
  // A power press in progress still inside the click window could resolve as
  // a double-click candidate on release — suppress button-down power-off.
  return BoardConfig::isX4Pro() && SETTINGS.doubleClickPwrLight &&
         gpio.getPowerButtonHeldTime() <= kPowerClickMaxHoldMs;
}

bool MappedInputManager::consumePowerDoubleClick() {
  if (!powerDoubleClickFrame) return false;
  powerDoubleClickFrame = false;
  return true;
}

#if FREEINK_CAP_TOUCH
bool MappedInputManager::wasPowerConfirmClick() const {
  if (!gpio.hasTouch() || SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM) return false;
  // Wait out the X4 Pro's frontlight double-click window before treating its
  // first release as Confirm. With the shortcut disabled, and on other touch
  // boards, the release counts directly — from the SNAPSHOT (soak-fix7): a
  // direct gpio.wasReleased would find the edge already consumed by the
  // snapshot loop (double-read loses it) and always report false.
  if (BoardConfig::isX4Pro() && SETTINGS.doubleClickPwrLight) return powerConfirmClickFrame;
  return edgeSnapshot(Button::Power, /*pressed=*/false) &&
         gpio.getPowerButtonHeldTime() <= SETTINGS.getPowerButtonDuration();
}
#endif

bool MappedInputManager::wasPressed(const Button button) const {
  if (button == Button::Confirm && homeAction == HomeButtonAction::Confirm) return true;
  if (button == Button::Back && wasBackGesture()) return true;
#if FREEINK_CAP_TOUCH
  if (button == Button::Confirm && wasPowerConfirmClick()) return true;
#endif
  return edgeSnapshot(button, true);
}

bool MappedInputManager::wasReleased(const Button button) const {
  if (button == Button::Confirm && homeAction == HomeButtonAction::Confirm) return true;
  if (button == Button::Back && wasBackGesture()) return true;
#if FREEINK_CAP_TOUCH
  if (button == Button::Confirm && wasPowerConfirmClick()) return true;
#endif
  return edgeSnapshot(button, false);
}

bool MappedInputManager::wasLongPressed(const Button button, const unsigned long thresholdMs) const {
  if (!isPressed(button)) return false;
  const uint16_t bit = 1u << static_cast<uint8_t>(button);
  if ((longPressFiredButtons & bit) != 0 || getHeldTime() < thresholdMs) return false;
  longPressFiredButtons |= bit;
  suppressNextRelease(button);
  return true;
}

void MappedInputManager::suppressNextRelease(const Button button) const {
  suppressedReleaseButtons |= 1u << static_cast<uint8_t>(button);
}

bool MappedInputManager::consumeSuppressedRelease() const {
  uint16_t released = 0;
  for (uint8_t value = 0; value <= static_cast<uint8_t>(Button::ScreenDown); ++value) {
    const uint16_t bit = 1u << value;
    if ((suppressedReleaseButtons & bit) != 0 && edgeSnapshot(static_cast<Button>(value), false)) {
      released |= bit;
      // Exactly-once (audit F1): also clear the PHYSICAL edge bits the
      // logical button maps to, so the suppressed release cannot also fire
      // wasReleased()/wasAnyReleased() later this tick. Composite logical
      // buttons (Nav*/Page*) map to several physical buttons — clear each.
      mapButtonWith(static_cast<Button>(value), [this](const uint8_t physical) {
        frameReleasedEdges &= static_cast<uint8_t>(~(1u << physical));
        return false;
      });
    }
  }
  suppressedReleaseButtons &= ~released;
  return released != 0;
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return framePressedEdges != 0; }

bool MappedInputManager::wasAnyReleased() const { return frameReleasedEdges != 0; }

unsigned long MappedInputManager::getHeldTime() const {
  // A mapped action has its own meaning, independent of the contact duration.
  if (homeAction != HomeButtonAction::Ignore) return 0;
  if (framePressedEdges == 0 && frameReleasedEdges == 0 && touchHeldOverrideValid &&
      millis() - touchHeldOverrideAt <= TOUCH_HELD_OVERRIDE_WINDOW_MS) {
    return touchHeldOverrideMs;
  }
  touchHeldOverrideValid = false;
  return gpio.getHeldTime();
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Swap previous/next labels to match the page turn direction swap in INVERTED and LANDSCAPE_CCW.
  const bool swapLabels = isNavDirectionSwapped();
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

  return mapFrontLabels(back, confirm, leftLabel, rightLabel);
}

MappedInputManager::Labels MappedInputManager::mapDirectionalLabels(const char* back, const char* confirm,
                                                                    const char* left, const char* right, const char* up,
                                                                    const char* down) const {
  const auto labelForButton = [&](const Button rawButton) {
    if (mapScreenDirection(Button::ScreenLeft) == rawButton) return left;
    if (mapScreenDirection(Button::ScreenRight) == rawButton) return right;
    if (mapScreenDirection(Button::ScreenUp) == rawButton) return up;
    if (mapScreenDirection(Button::ScreenDown) == rawButton) return down;
    return "";
  };
  return mapFrontLabels(back, confirm, labelForButton(Button::Left), labelForButton(Button::Right));
}

MappedInputManager::Labels MappedInputManager::mapFrontLabels(const char* back, const char* confirm, const char* left,
                                                              const char* right) const {
  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return left;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return right;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  // Reads the per-tick snapshot (multi-read safe under consume-on-check).
  if ((framePressedEdges & (1u << HalGPIO::BTN_BACK)) != 0) {
    return HalGPIO::BTN_BACK;
  }
  if ((framePressedEdges & (1u << HalGPIO::BTN_CONFIRM)) != 0) {
    return HalGPIO::BTN_CONFIRM;
  }
  if ((framePressedEdges & (1u << HalGPIO::BTN_LEFT)) != 0) {
    return HalGPIO::BTN_LEFT;
  }
  if ((framePressedEdges & (1u << HalGPIO::BTN_RIGHT)) != 0) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
