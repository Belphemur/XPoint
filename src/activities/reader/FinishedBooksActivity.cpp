#include "FinishedBooksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "BookStatsView.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

FinishedBooksActivity::FinishedBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FinishedBooks", renderer, mappedInput) {}

void FinishedBooksActivity::onEnter() {
  Activity::onEnter();
#ifdef READING_STATS_ENABLED
  finishedBooks = FinishedBooksIndex::load();
  globalStats = GlobalReadingStats::load();
#endif
  previousOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  requestUpdate();
}

void FinishedBooksActivity::loop() {
  const size_t pageCount = std::max<size_t>(
      1, (finishedBooks.size() + FINISHED_BOOKS_ENTRIES_PER_PAGE - 1) / FINISHED_BOOKS_ENTRIES_PER_PAGE);

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && finishedBooksPage > 0) {
    --finishedBooksPage;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) && finishedBooksPage + 1 < pageCount) {
    ++finishedBooksPage;
    requestUpdate();
  }
}

void FinishedBooksActivity::onExit() {
  renderer.setOrientation(previousOrientation);
  Activity::onExit();
}

void FinishedBooksActivity::render(RenderLock&&) {
  const size_t pageCount = std::max<size_t>(
      1, (finishedBooks.size() + FINISHED_BOOKS_ENTRIES_PER_PAGE - 1) / FINISHED_BOOKS_ENTRIES_PER_PAGE);
  const bool hasPreviousPage = finishedBooksPage > 0;
  const bool hasNextPage = finishedBooksPage + 1 < pageCount;

  BookStatsView::renderFinishedBooksPage(renderer, &mappedInput, finishedBooks, globalStats, finishedBooksPage,
                                         /*showButtonHints=*/true, hasPreviousPage, hasNextPage,
                                         /*showMoreButton=*/false);
  const char* title = tr(STR_STATS_FINISHED_BOOKS);
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title, true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
}
