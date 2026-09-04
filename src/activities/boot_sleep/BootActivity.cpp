#include "BootActivity.h"

#include <GfxRenderer.h>

#include "fontIds.h"
#include "images/Logo120.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  // Transparent blit: bit=1 pixels leave the framebuffer untouched, bit=0 paints ink.
  // The Logo120.h bytes are pre-rotated 90° CCW (see the header in Logo120.h) so the
  // icon is upright on the device's native LandscapeCounterClockwise orientation.
  renderer.drawImageTransparent(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  // Fork brand text — hardcoded on purpose (not translated).
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, "XPOINT", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "Booting ...");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
