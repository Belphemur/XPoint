#include "FinishReadingPromptActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_YES = 1;
constexpr fui::ActionId ACTION_KEEP = 2;
// Absorbs taps on the dialog body (needs no handler); the activity treats an
// unrouted release with real coordinates as tap-outside = dismiss.
constexpr fui::ActionId ACTION_CHROME = 5;
}  // namespace

FinishReadingPromptActivity::FinishReadingPromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const uint8_t percentRemaining)
    : Activity("FinishReadingPrompt", renderer, mappedInput),
      UiAppHost(renderer),
      percentRemaining(percentRemaining) {}

void FinishReadingPromptActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_YES, &FinishReadingPromptActivity::onYesEvent, this);
  app.on(ACTION_KEEP, &FinishReadingPromptActivity::onKeepEvent, this);
  app.setScreen(&FinishReadingPromptActivity::promptScreen, this);
  requestUpdate();
}

void FinishReadingPromptActivity::confirm() {
  ActivityResult result;
  result.isCancelled = false;
  setResult(std::move(result));
  finish();
}

void FinishReadingPromptActivity::dismiss() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void FinishReadingPromptActivity::onYesEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<FinishReadingPromptActivity*>(user);
  self->app.clearTapFlash();  // the tap leaves this screen
  self->confirm();
}

void FinishReadingPromptActivity::onKeepEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<FinishReadingPromptActivity*>(user);
  self->app.clearTapFlash();
  self->dismiss();
}

void FinishReadingPromptActivity::loop() {
  // Touch goes through the FreeInkApp: render() registered the Yes/Keep pills and
  // the chrome guard. Runs before the Back handler so a tap inside the dialog is
  // consumed here and never reaches the reader's page-turn gesture handlers.
  const auto route = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (route.routed && app.invalidated()) requestUpdate();
  if (route) return;

  // Tap released outside the dialog (inside-taps are absorbed by the chrome
  // guard): dismiss. Swipe-end releases arrive with -1,-1 coords and fall
  // through — same rule as OptionPopup.
  if (route.routed && route.snap.touchReleased && route.snap.touchX >= 0) {
    dismiss();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    dismiss();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirm();
    return;
  }

  // Button boards: front buttons = Keep reading, side buttons = Yes, finished.
  // Both directions of each pair answer the prompt so a hand resting on either
  // edge always gets the same semantic.
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    dismiss();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) || mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    confirm();
    return;
  }
}

void FinishReadingPromptActivity::render(RenderLock&&) {
  // The dialog is drawn over whatever page the reader already had on screen:
  // no clear here, the app paints the popup card on top. Everything renders
  // through the app so the interaction table stays complete.
  renderUi();
  renderer.displayBuffer();
}

void FinishReadingPromptActivity::promptScreen(UiScreen& screen, void* user) {
  static_cast<FinishReadingPromptActivity*>(user)->buildPromptScreen(screen);
}

void FinishReadingPromptActivity::buildPromptScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  const bool touch = mappedInput.hasTouch();

  // The readout is the dialog's hero: remap the title slot to the large sans
  // so it reads at a glance.
  uiTarget.setFont(fui::GfxRendererTarget::FONT_TITLE, NOTOSANS_18_FONT_ID);

  const int16_t capLh = screen.target().lineHeight(theme.smallText.font);
  const int16_t bodyLh = screen.target().lineHeight(theme.bodyText.font);
  // "left to read" suffix line + the body question line.
  const int16_t questionH = static_cast<int16_t>(capLh + theme.spaceSm + bodyLh);
  const int16_t hintsH = touch ? 0 : static_cast<int16_t>(capLh * 2 + theme.spaceSm + theme.spaceMd);

  char readout[16];
  snprintf(readout, sizeof(readout), "%u%%", percentRemaining);

  // Dialog chrome and slots, styled exactly like UiSliderDialog/OptionPopup:
  // bordered popup panel per theme frame metrics, body-font caption, then the
  // Yes/Keep pills as one bottom row on touch boards. Static because the props
  // embed StyleSets, well past the 256-byte local stack budget; every field is
  // reassigned each build.
  fui::DialogOption options[2];
  static fui::OptionDialogProps props;
  props.title = I18N.get(StrId::STR_ALMOST_THERE);
  props.headline = readout;
  if (touch) {
    options[0].label = I18N.get(StrId::STR_YES_FINISHED);
    options[0].action = ACTION_YES;
    options[1].label = I18N.get(StrId::STR_KEEP_READING);
    options[1].action = ACTION_KEEP;
    props.options = options;
    props.optionCount = 2;
  } else {
    props.options = nullptr;
    props.optionCount = 0;
  }
  props.inputMask = fui::InputTouch;
  props.titleText = theme.smallText;
  props.titleText.bold = true;
  props.titleText.align = fui::TextAlign::Center;
  props.headlineText = theme.titleText;
  props.headlineText.bold = true;
  props.headlineText.align = fui::TextAlign::Center;
  props.buttonText = theme.bodyText;
  const int16_t innerPadding = static_cast<int16_t>(metrics.optionPopupInnerPadding);
  props.padding = fui::Insets{innerPadding, innerPadding, innerPadding, innerPadding};
  props.gap = static_cast<int16_t>(metrics.optionPopupItemSpacing);
  // defaultPopupStyles() has no border, so opt in using the per-theme frame metrics.
  props.styles = fui::defaultPopupStyles();
  props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  props.styles.normal.borderWidth = static_cast<uint8_t>(metrics.popupFrameThickness);
  props.styles.normal.radius = static_cast<uint8_t>(metrics.popupCornerRadius);
  props.styles.selected = props.styles.normal;
  props.styles.focused = props.styles.normal;
  props.styles.active = props.styles.normal;
  props.styles.disabled = props.styles.normal;
  props.buttonHeight =
      fui::clampI16(screen.target().lineHeight(theme.bodyText.font) + metrics.optionPopupSelectionVPadding * 2);

  // Reserve the suffix/question band (and hint lines) as the dialog's content
  // band; optionDialog returns its rect. Same width rule as OptionPopup.
  props.contentHeight = static_cast<int16_t>(questionH + hintsH);
  const fui::Rect screenRect = screen.frame().screen();
  const int16_t width = fui::clampI16(
      std::min<int>(screenRect.width * 3 / 4, screenRect.width - metrics.optionPopupDialogSideMargin * 2));
  // Measured height minus one spaceMd: texts anchor to the top and the buttons
  // to the bottom, so the trim comes out of the air between the readout and
  // the content band.
  const int16_t height =
      fui::clampI16(fui::optionDialogHeight(screen.target(), props, width) - theme.spaceMd, 0, screenRect.height);
  const fui::Rect dialog = fui::centeredRect(screenRect, fui::Size{width, height});
  // Chrome guard first, controls after: routing scans newest-first, so the
  // pills win inside the dialog and the guard absorbs the rest — same pattern
  // as OptionPopup.
  screen.frame().hit(dialog, ACTION_CHROME, 0, fui::InputTouch);
  const fui::Rect band = fui::optionDialog(screen.frame(), dialog, props);

  // Content band, top to bottom: the "left to read" suffix, then the body
  // question; button boards append the two centered hint lines below.
  const char* suffix = I18N.get(StrId::STR_LEFT_TO_READ);
  fui::TextStyle suffixText = theme.smallText;
  suffixText.align = fui::TextAlign::Center;
  screen.target().text(fui::Rect{band.x, band.y, band.width, capLh}, suffix, suffixText);

  const char* question = I18N.get(StrId::STR_DID_YOU_FINISH);
  fui::TextStyle questionText = theme.bodyText;
  questionText.align = fui::TextAlign::Center;
  const int16_t questionY = static_cast<int16_t>(band.y + capLh + theme.spaceSm);
  screen.target().text(fui::Rect{band.x, questionY, band.width, bodyLh}, question, questionText);

  if (!touch) {
    // Front buttons = Keep reading, side buttons = Yes, finished — built from
    // separate label + answer strings so the layout doesn't depend on a
    // separator hidden in translated text.
    char hintFront[96];
    snprintf(hintFront, sizeof(hintFront), "%s %s", I18N.get(StrId::STR_STEP_HINT_FRONT), I18N.get(StrId::STR_KEEP_READING));
    char hintSide[96];
    snprintf(hintSide, sizeof(hintSide), "%s %s", I18N.get(StrId::STR_STEP_HINT_SIDE), I18N.get(StrId::STR_YES_FINISHED));

    const int16_t hintY = static_cast<int16_t>(questionY + bodyLh + theme.spaceMd);
    screen.target().text(fui::Rect{band.x, hintY, band.width, capLh}, hintFront, suffixText);
    screen.target().text(fui::Rect{band.x, static_cast<int16_t>(hintY + capLh + theme.spaceSm), band.width, capLh},
                         hintSide, suffixText);
  }
}
