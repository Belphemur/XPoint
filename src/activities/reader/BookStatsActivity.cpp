#include "BookStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "BookStatsView.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                     const BookReadingStats& stats)
    : Activity("BookStats", renderer, mappedInput), bookTitle(std::move(title)), stats(stats) {}

void BookStatsActivity::onEnter() {
  Activity::onEnter();
  // The card grid is laid out for portrait; it does not fit landscape heights.
  previousOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
}

void BookStatsActivity::onExit() {
  renderer.setOrientation(previousOrientation);
  Activity::onExit();
}

void BookStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(ActivityResult{ClearPaceResult{}});
    finish();
  }
}

void BookStatsActivity::render(RenderLock&&) {
  const float progressPercent = stats.lastBookProgressPercent == UNKNOWN_BOOK_PROGRESS_PERCENT
                                    ? -1.0f
                                    : static_cast<float>(stats.lastBookProgressPercent);
  BookStatsView::renderPerBookStatsPage(renderer, &mappedInput, bookTitle, stats, progressPercent,
                                        /*hasEstimatedTimeLeft=*/false, stats.estimatedTimeLeftSeconds,
                                        /*showButtonHints=*/true);

  const char* title = bookTitle.empty() ? tr(STR_READING_STATS) : bookTitle.c_str();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title, true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CLEAR_BOOK_PACE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
