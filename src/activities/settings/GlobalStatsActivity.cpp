#include "GlobalStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/reader/BookStatsView.h"
#include "components/UITheme.h"
#include "fontIds.h"

GlobalStatsActivity::GlobalStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("GlobalStats", renderer, mappedInput) {}

void GlobalStatsActivity::onEnter() {
  Activity::onEnter();
#ifdef READING_STATS_ENABLED
  // Load even when tracking is disabled so the viewer still shows the
  // last-saved (frozen) values instead of a freshly zeroed record.
  globalStats = GlobalReadingStats::load();
#endif
  // The card grid is laid out for portrait; it does not fit landscape heights.
  previousOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  // First paint is ours to request: no other notification is pending at this
  // point (push consumed the previous screen's), and without this the render
  // task idles until the next unrelated notify (header-clock minute tick or a
  // keypress) — the screen looks frozen for up to a minute.
  requestUpdate();
}

void GlobalStatsActivity::onExit() {
  renderer.setOrientation(previousOrientation);
  Activity::onExit();
}

void GlobalStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
#ifdef READING_STATS_ENABLED
    // When the flag is off, onEnter() leaves `globalStats` default-constructed
    // (no on-disk load). Calling clearWpmStats + save here would overwrite any
    // previously-persisted global_stats.bin with a zeroed record. Gate the
    // action so the flag-off build is purely read-only.
    globalStats.clearWpmStats();
    globalStats.save();
#endif
    requestUpdate();
  }
}

void GlobalStatsActivity::render(RenderLock&&) {
  BookStatsView::renderGlobalStatsPage(renderer, &mappedInput, tr(STR_STATS_ALL_TIME), globalStats,
                                       /*showButtonHints=*/true);

  const char* title = tr(STR_READING_STATS);
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title, true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CLEAR_GLOBAL_PACE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
