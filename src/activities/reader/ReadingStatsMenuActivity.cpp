#include "ReadingStatsMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include "BookStatsActivity.h"
#include "FinishedBooksActivity.h"
#include "GlobalReadingStats.h"
#include "MappedInputManager.h"
#include "ReadingRhythmActivity.h"
#include "activities/settings/GlobalStatsActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

ReadingStatsMenuActivity::ReadingStatsMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::optional<BookReadingStats> initialBookStats,
                                                   std::string initialBookTitle, std::string initialBookCachePath,
                                                   std::string initialBookAuthor)
    : UiListActivity("ReadingStatsMenu", renderer, mappedInput),
      bookStats(std::move(initialBookStats)),
      bookTitle(std::move(initialBookTitle)),
      bookAuthor(std::move(initialBookAuthor)),
      bookCachePath(std::move(initialBookCachePath)) {
  rebuildRowItems();
}

void ReadingStatsMenuActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRowItems();
  requestUpdate();
}

void ReadingStatsMenuActivity::onExit() { Activity::onExit(); }

int ReadingStatsMenuActivity::listCount() const { return rowCount; }

void ReadingStatsMenuActivity::rebuildRowItems() {
  // Touch hits dispatch ListItem::actionValue as a row index, so actionValue
  // must be the visible row; rowEntries carries the target screen.
  size_t index = 0;
  const auto addRow = [this, &index](const char* label, const StatsEntry entry) {
    menuRowItems[index].label = label;
    menuRowItems[index].actionValue = static_cast<int16_t>(index);
    rowEntries[index] = entry;
    ++index;
  };
  if (bookStats) {
    addRow(tr(STR_STATS_THIS_BOOK), StatsEntry::ThisBook);
  }
  addRow(tr(STR_STATS_THIS_DEVICE_SCREEN), StatsEntry::ThisDevice);
  addRow(tr(STR_STATS_READING_RHYTHM), StatsEntry::ReadingRhythm);
  addRow(tr(STR_STATS_FINISHED_BOOKS), StatsEntry::FinishedBooks);
  rowCount = static_cast<int>(index);
}

void ReadingStatsMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = menuRowItems;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void ReadingStatsMenuActivity::activateIndex(const int index) {
  if (index < 0 || index >= rowCount) return;
  app.clearTapFlash();
  switch (rowEntries[index]) {
    case StatsEntry::ThisBook:
      openThisBook();
      break;
    case StatsEntry::ThisDevice:
      openThisDevice();
      break;
    case StatsEntry::ReadingRhythm:
      openReadingRhythm();
      break;
    case StatsEntry::FinishedBooks:
      openFinishedBooks();
      break;
  }
}

void ReadingStatsMenuActivity::openThisBook() {
  if (!bookStats) return;
  auto statsActivity =
      makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, bookTitle, bookAuthor, *bookStats, bookCachePath,
                                           bookCachePath.empty() ? GlobalReadingStats{} : GlobalReadingStats::load());
  if (!statsActivity) {
    LOG_ERR("RSM", "OOM: BookStatsActivity");
    return;
  }
  startActivityForResult(std::move(statsActivity), [this](const ActivityResult& result) {
    if (std::holds_alternative<ClearPaceResult>(result.data)) {
      ActivityResult clearPace;
      clearPace.data = ClearPaceResult{};
      setResult(std::move(clearPace));
      finish();
      return;
    }
    // BookStatsActivity::saveStats() persists date edits to disk; refresh the
    // menu's snapshot so reopening ThisBook shows the saved dates instead of
    // the stale constructor copy.
    if (bookStats && !bookCachePath.empty()) {
      *bookStats = BookReadingStats::load(bookCachePath);
    }
    requestUpdate();
  });
}

void ReadingStatsMenuActivity::openThisDevice() {
  auto activity = makeUniqueNoThrow<GlobalStatsActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("RSM", "OOM: GlobalStatsActivity");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
}

void ReadingStatsMenuActivity::openReadingRhythm() {
  auto activity = makeUniqueNoThrow<ReadingRhythmActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("RSM", "OOM: ReadingRhythmActivity");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
}

void ReadingStatsMenuActivity::openFinishedBooks() {
  auto activity = makeUniqueNoThrow<FinishedBooksActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("RSM", "OOM: FinishedBooksActivity");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
}
