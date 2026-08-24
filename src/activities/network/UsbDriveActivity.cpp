#include "UsbDriveActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"

void UsbDriveActivity::onEnter() {
  Activity::onEnter();
  state = State::Unsupported;
  preparing = true;
  startFailed = false;
  restartRequested = false;
  forcedDisconnectRequested = false;
  hostWaitStartedAt = 0;
  startFailureStartedAt = 0;
  forcedDisconnectRequestedAt = 0;

  // Show the safety instructions before giving the raw SD card to the USB host.
  requestUpdateAndWait();
  if (!Storage.beginUsbDrive()) {
    LOG_ERR("USB", "Unable to start USB Drive");
    preparing = false;
    startFailed = true;
    state = State::IoError;
    startFailureStartedAt = millis();
    requestUpdate();
    return;
  }

  preparing = false;
  state = State::WaitingForHost;
  hostWaitStartedAt = millis();
  requestUpdate();
}

void UsbDriveActivity::onExit() {
  if (!restartRequested) Storage.endUsbDrive();
  Activity::onExit();
}

void UsbDriveActivity::loop() {
  if (!startFailed) {
    const State nextState = Storage.usbDriveState();
    if (nextState != state) {
      const bool messageChanged = state != State::Connected || nextState != State::Accessed;
      state = nextState;
      if (messageChanged) requestUpdate();
    }
  }

  if (state == State::WaitingForHost && millis() - hostWaitStartedAt >= HOST_WAIT_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive host wait timed out");
    restartToHome();
    return;
  }

  if (startFailed && millis() - startFailureStartedAt >= START_FAILURE_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive startup failure timed out");
    restartToHome();
    return;
  }

  if (!startFailed && state == State::IoError) {
    if (!forcedDisconnectRequested) {
      forcedDisconnectRequested = true;
      forcedDisconnectRequestedAt = millis();
      LOG_ERR("USB", "USB Drive I/O error; disconnecting host");
      if (!Storage.disconnectUsbDriveHost()) {
        LOG_ERR("USB", "Unable to request USB Drive host disconnect");
      }
    } else if (millis() - forcedDisconnectRequestedAt >= FORCED_DISCONNECT_TIMEOUT_MS) {
      LOG_ERR("USB", "USB Drive host disconnect timed out; forcing restart");
      restartToHome();
    }
    return;
  }

  const bool canExitWithInput = state == State::WaitingForHost || startFailed;
  if (canExitWithInput && (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Power) || mappedInput.wasHomeGesture())) {
    restartToHome();
    return;
  }

  if (state == State::Ejected || state == State::Disconnected || state == State::Unsupported) {
    restartToHome();
  }
}

void UsbDriveActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_USB_DRIVE));

  if (preparing) {
    renderMessage(tr(STR_USB_DRIVE_PREPARING), tr(STR_USB_DRIVE_EJECT_HINT));
  } else {
    switch (state) {
      case State::WaitingForHost:
        renderMessage(tr(STR_USB_DRIVE_WAITING));
        break;
      case State::Connected:
      case State::Accessed:
        renderMessage(tr(STR_USB_DRIVE_CONNECTED), tr(STR_USB_DRIVE_EJECT_HINT));
        break;
      case State::IoError:
        renderMessage(startFailed ? tr(STR_USB_DRIVE_START_ERROR) : tr(STR_USB_DRIVE_ERROR));
        break;
      case State::Ejected:
      case State::Disconnected:
      case State::Unsupported:
        break;
    }
  }

  if (state == State::WaitingForHost || startFailed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

void UsbDriveActivity::renderMessage(const char* message, const char* detail) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int messageHeight = lineHeight * 2;
  const int detailHeight = detail ? lineHeight * 3 : 0;
  const int totalHeight = messageHeight + (detail ? metrics.verticalSpacing + detailHeight : 0);
  const int top = (renderer.getScreenHeight() - totalHeight) / 2;

  UITheme::drawCenteredWrappedText(renderer, Rect{metrics.contentSidePadding, top, width, messageHeight}, UI_10_FONT_ID,
                                   message, 2, true, EpdFontFamily::BOLD);
  if (detail) {
    UITheme::drawCenteredWrappedText(
        renderer, Rect{metrics.contentSidePadding, top + messageHeight + metrics.verticalSpacing, width, detailHeight},
        UI_10_FONT_ID, detail, 3);
  }
}

void UsbDriveActivity::restartToHome() {
  if (restartRequested) return;
  restartRequested = true;
  Storage.endUsbDrive();
  delay(20);
  restartToHomeAfterStorageHandoff();
}
