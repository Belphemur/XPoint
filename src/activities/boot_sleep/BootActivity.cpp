#include "BootActivity.h"

#include <GfxRenderer.h>

#include "fontIds.h"
#include "images/Logo120Draw.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  // Inverted transparent blit of the logo — the shared helper and the sleep
  // screen use the same treatment so boot and sleep match.
  drawLogo120Inverted(renderer, (pageWidth - 120) / 2, (pageHeight - 120) / 2);
  // Fork brand text — hardcoded on purpose (not translated).
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, "XPOINT", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "Booting ...");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
