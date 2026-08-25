#include "BookStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "ReadingStatsUtils.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

std::string formatStatsDuration(uint32_t seconds) {
  char buf[32];
  BookReadingStats::formatDuration(seconds, buf, sizeof(buf));
  return std::string(buf);
}

std::string formatStatsDate(const ReadingStatsDate& date) {
  if (!date.isValid()) return std::string("--");
  char buf[32];
  formatReadingStatsShortDate(date, buf, sizeof(buf));
  return std::string(buf);
}

}  // namespace

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                     const BookReadingStats& stats)
    : UiListActivity("BookStats", renderer, mappedInput, /*wantsTouchLongPress=*/false),
      bookTitle(std::move(title)),
      stats(stats) {}

void BookStatsActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRowItems();
}

void BookStatsActivity::rebuildRowItems() {
  valueCache.clear();
  rowItems.clear();
  const size_t n = 20;
  valueCache.reserve(n);
  rowItems.reserve(n);

  const auto addRow = [&](const char* label, std::string value) {
    valueCache.push_back(std::move(value));
    fui::ListItem item;
    item.label = label;
    item.value = valueCache.back().c_str();
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  };

  addRow(tr(STR_STATS_SESSIONS_LBL), std::to_string(stats.sessionCount));
  addRow(tr(STR_STATS_TIME_LBL), formatStatsDuration(stats.totalReadingSeconds));
  addRow(tr(STR_STATS_PAGES_LBL), std::to_string(stats.totalPagesTurned));
  const uint32_t avgSession = stats.sessionCount > 0 ? stats.totalReadingSeconds / stats.sessionCount : 0;
  addRow(tr(STR_STATS_AVG_SESSION_LBL), formatStatsDuration(avgSession));
  addRow(tr(STR_STATS_AVG_PAGE_PACE), formatStatsDuration(stats.avgSecondsPerForwardPage));
  addRow(tr(STR_STATS_EST_TIME_LEFT), formatStatsDuration(stats.estimatedTimeLeftSeconds));
  addRow(tr(STR_STATS_COMPLETED), stats.isCompleted ? tr(STR_YES) : tr(STR_NO));
  addRow(tr(STR_STATS_STARTED), formatStatsDate(stats.startDate));
  addRow(tr(STR_STATS_FINISHED), formatStatsDate(stats.finishedDate));
  addRow(tr(STR_STATS_MORNING), formatStatsDuration(stats.timeOfDaySeconds[0]));
  addRow(tr(STR_STATS_AFTERNOON), formatStatsDuration(stats.timeOfDaySeconds[1]));
  addRow(tr(STR_STATS_EVENING), formatStatsDuration(stats.timeOfDaySeconds[2]));
  addRow(tr(STR_STATS_NIGHT), formatStatsDuration(stats.timeOfDaySeconds[3]));
  addRow(tr(STR_STATS_MON), formatStatsDuration(stats.dayOfWeekSeconds[0]));
  addRow(tr(STR_STATS_TUE), formatStatsDuration(stats.dayOfWeekSeconds[1]));
  addRow(tr(STR_STATS_WED), formatStatsDuration(stats.dayOfWeekSeconds[2]));
  addRow(tr(STR_STATS_THU), formatStatsDuration(stats.dayOfWeekSeconds[3]));
  addRow(tr(STR_STATS_FRI), formatStatsDuration(stats.dayOfWeekSeconds[4]));
  addRow(tr(STR_STATS_SAT), formatStatsDuration(stats.dayOfWeekSeconds[5]));
  addRow(tr(STR_STATS_SUN), formatStatsDuration(stats.dayOfWeekSeconds[6]));
}

void BookStatsActivity::activateIndex(const int index) { (void)index; }

bool BookStatsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }
  return false;
}

void BookStatsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props, /*hasSubtitle=*/false);
  screen.list(props);
}

void BookStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int contentY = isPortraitInverted ? 50 : 0;

  const char* title = bookTitle.empty() ? tr(STR_READING_STATS) : bookTitle.c_str();
  const int titleX = contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, title, true, EpdFontFamily::BOLD);

  renderUi();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
