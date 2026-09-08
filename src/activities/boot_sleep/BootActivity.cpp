#include "BootActivity.h"

#include <GfxRenderer.h>
#include <Memory.h>

#include "fontIds.h"
#include "images/Logo120.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  // Invert the logo's black/white: XOR every byte with 0xFF so bit=0 (black)
  // becomes bit=1 (transparent) and vice-versa. The Logo120.h bytes are
  // pre-rotated 90° CCW (see the header in Logo120.h) so the icon is upright
  // on the device's native LandscapeCounterClockwise orientation.
  constexpr size_t LOGO120_BYTES = 120 * 120 / 8;  // 1800 bytes
  auto invertedLogo = makeUniqueNoThrow<uint8_t[]>(LOGO120_BYTES);
  if (invertedLogo) {
    for (size_t i = 0; i < LOGO120_BYTES; ++i) {
      invertedLogo[i] = static_cast<uint8_t>(Logo120[i] ^ 0xFF);
    }
    renderer.drawImageTransparent(invertedLogo.get(), (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  } else {
    LOG_ERR("BOOT", "OOM (%d bytes) — drawing original logo", LOGO120_BYTES);
    renderer.drawImageTransparent(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  }
  // Fork brand text — hardcoded on purpose (not translated).
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, "XPOINT", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "Booting ...");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
