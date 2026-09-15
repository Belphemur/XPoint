#include "UiAppHost.h"

#include <Logging.h>

#include "UiAppHelpers.h"

namespace fui = freeink::ui;

UiAppHost::UiAppHost(const GfxRenderer& renderer)
    : uiTarget(makeUiTarget(renderer)), app(uiTarget, uiTarget.deviceContext()) {}

void UiAppHost::resetUi() {
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  LOG_DBG("UIRT", "uiReady=false");
}

void UiAppHost::renderUi() {
  app.setDevice(uiTarget.deviceContext());
  app.render();
  uiReady = true;
  LOG_DBG("UIRT", "uiReady=true");
}

UiAppHost::TouchRoute UiAppHost::routeTouch(const MappedInputManager& input, const bool withLongPress,
                                            const bool routeHeld) {
  TouchRoute result;  // named apart from route() — cppcheck flags the shadow
  if (!uiReady) return result;
  result.snap = touchSnapshotFrom(input, withLongPress);
  if (!result.snap.touchPressed && !result.snap.touchReleased && !(routeHeld && result.snap.touchHeld)) {
    return result;
  }
  result.routed = true;
  result.event = app.route(result.snap);
  LOG_DBG("UIRT", "routed event=%d value=%d touch=(%d,%d)", static_cast<int>(result.event.action),
          static_cast<int>(result.event.value), static_cast<int>(result.snap.touchX),
          static_cast<int>(result.snap.touchY));
  return result;
}

fui::ActionEvent UiAppHost::route(const fui::InputSnapshot& snap) {
  if (!uiReady) return {};
  return app.route(snap);
}
