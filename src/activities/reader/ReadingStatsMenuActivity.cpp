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
                                                   std::string initialBookTitle, std::string initialBookCachePath)
    : UiListActivity("ReadingStatsMenu", renderer, mappedInput),
      bookStats(std::move(initialBookStats)),
      bookTitle(std::move(initialBookTitle)),
      bookCachePath(std::move(initialBookCachePath)) {
  rebuildRowItems();
}

void ReadingStatsMenuActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRowItems();
  requestUpdate();
}

void ReadingStatsMenuActivity::onExit() { Activity::onExit(); }

int ReadingStatsMenuActivity::listCount() const { return bookStats ? 4 : 3; }

void ReadingStatsMenuActivity::rebuildRowItems() {
  const size_t count = bookStats ? 4 : 3;
  size_t index = 0;
  if (bookStats) {
    menuRowItems[index].label = tr(STR_STATS_THIS_BOOK);
    menuRowItems[index].actionValue = static_cast<int16_t>(static_cast<uint8_t>(StatsEntry::ThisBook));
    ++index;
  }
  menuRowItems[index].label = tr(STR_STATS_THIS_DEVICE_SCREEN);
  menuRowItems[index].actionValue = static_cast<int16_t>(static_cast<uint8_t>(StatsEntry::ThisDevice));
  ++index;
  menuRowItems[index].label = tr(STR_STATS_READING_RHYTHM);
  menuRowItems[index].actionValue = static_cast<int16_t>(static_cast<uint8_t>(StatsEntry::ReadingRhythm));
  ++index;
  menuRowItems[index].label = tr(STR_STATS_FINISHED_BOOKS);
  menuRowItems[index].actionValue = static_cast<int16_t>(static_cast<uint8_t>(StatsEntry::FinishedBooks));
}

void ReadingStatsMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
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
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  const auto entry = static_cast<StatsEntry>(menuRowItems[index].actionValue);
  switch (entry) {
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
  auto statsActivity = makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, bookTitle, *bookStats, bookCachePath,
                                                            bookCachePath.empty() ? GlobalReadingStats{}
                                                                                  : GlobalReadingStats::load());
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
