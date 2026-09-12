#include "ReadingRhythmActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "BookStatsView.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

ReadingRhythmActivity::ReadingRhythmActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ReadingRhythm", renderer, mappedInput) {}
void ReadingRhythmActivity::onEnter() {
  Activity::onEnter();
#ifdef READING_STATS_ENABLED
  globalStats = GlobalReadingStats::load();
#endif
  previousOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  requestUpdate();
}

void ReadingRhythmActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
}

void ReadingRhythmActivity::onExit() {
  renderer.setOrientation(previousOrientation);
  Activity::onExit();
}

void ReadingRhythmActivity::render(RenderLock&&) {
  BookStatsView::renderReadingRhythmPage(renderer, &mappedInput, globalStats, /*showButtonHints=*/true,
                                         /*showMoreButton=*/false);
  const char* title = tr(STR_STATS_READING_RHYTHM);
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title, true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
}
