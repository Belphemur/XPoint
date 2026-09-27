#include "FrontlightControl.h"

#include <GfxRenderer.h>
#include <HalFrontlight.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"

namespace {
// Set by every mutator, cleared by persistIfDirty(). Single-threaded use: all
// callers run on the loop task (panel events, gesture dispatch, lifecycle).
bool settingsDirty = false;
}  // namespace

namespace frontlight {

void setBrightness(const uint8_t percent) {
  const uint8_t clamped = std::clamp(percent, FRONTLIGHT_MIN_BRIGHTNESS, static_cast<uint8_t>(100));
  if (SETTINGS.frontlightBrightness == clamped && SETTINGS.frontlightOn && Frontlight.isOn()) return;
  SETTINGS.frontlightBrightness = clamped;
  if (!SETTINGS.frontlightOn || !Frontlight.isOn()) {
    // setOn(true) restores lastBrightness; apply the new level after it.
    SETTINGS.frontlightOn = 1;
    Frontlight.setOn(true);
  }
  Frontlight.setBrightness(clamped);
  settingsDirty = true;
}

void setWarmth(const uint8_t percent) {
  if (!Frontlight.hasColorTemperature()) return;
  const uint8_t clamped = std::min(percent, static_cast<uint8_t>(100));
  if (SETTINGS.frontlightWarmth == clamped) return;
  SETTINGS.frontlightWarmth = clamped;
  Frontlight.setWarmth(clamped);
  settingsDirty = true;
}

void setOn(const bool on) {
  if (static_cast<bool>(SETTINGS.frontlightOn) == on && Frontlight.isOn() == on) return;
  // Off deliberately keeps frontlightBrightness/lastBrightness, so toggling
  // back on restores the level the slider shows.
  SETTINGS.frontlightOn = on ? 1 : 0;
  Frontlight.setOn(on);
  settingsDirty = true;
}

void adjustBrightness(const int delta) {
  const int next = static_cast<int>(SETTINGS.frontlightBrightness) + delta;
  if (next <= 0) {
    setOn(false);
    return;
  }
  setBrightness(static_cast<uint8_t>(next));
}

void adjustWarmth(const int delta) {
  const int next = std::clamp(static_cast<int>(SETTINGS.frontlightWarmth) + delta, 0, 100);
  setWarmth(static_cast<uint8_t>(next));
}

void persistIfDirty() {
  if (!settingsDirty) return;
  settingsDirty = false;
  SETTINGS.saveToFile();
}

bool SwipeGesture::update(MappedInputManager& input, GfxRenderer& renderer, const bool policy) {
  // 3px of vertical travel per 1% keeps night-time tuning precise while a full
  // sweep still fits on the panel height (see the original reader handler).
  static constexpr int PIXELS_PER_PERCENT = 3;
  static constexpr float SIDE_BAND = 0.08f;  // 8% of width from each edge
  const int screenW = renderer.getScreenWidth();
  const int leftBand = static_cast<int>(screenW * SIDE_BAND);
  const int rightBand = screenW - static_cast<int>(screenW * SIDE_BAND);

  if (!active_) {
    if (!policy) return false;
    int tx = 0;
    int ty = 0;
    if (!input.wasScreenTouchDown(tx, ty)) return false;
    if (tx < leftBand) {
      active_ = true;
      leftSide_ = true;
      lastY_ = ty;
    } else if (tx >= rightBand) {
      active_ = true;
      leftSide_ = false;
      lastY_ = ty;
    }
    return active_;  // true only when a drag started on an edge band
  }

  int cx = 0;
  int cy = 0;
  if (input.isScreenTouchHeld(cx, cy)) {
    const int deltaY = cy - lastY_;
    const int step = std::abs(deltaY) / PIXELS_PER_PERCENT;
    lastY_ = cy;  // reset the baseline for the next frame
    if (step == 0) return true;
    if (leftSide_) {
      adjustWarmth(deltaY < 0 ? step : -step);  // up = warmer
    } else {
      adjustBrightness(deltaY < 0 ? step : -step);  // up = brighter
    }
    return true;
  }

  if (input.wasScreenTouchReleased()) active_ = false;
  return true;  // a live drag stays consumed through its release frame
}

}  // namespace frontlight