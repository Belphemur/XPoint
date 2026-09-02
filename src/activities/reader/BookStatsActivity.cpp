#include "BookStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
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
  if (!date.isValid()) return std::string(tr(STR_STATS_VALUE_UNAVAILABLE));
  char buf[32];
  formatReadingStatsShortDate(date, buf, sizeof(buf));
  return std::string(buf);
}

std::string formatDayCount(uint16_t days) {
  char buf[40];
  snprintf(buf, sizeof(buf), tr(STR_STATS_DAY_COUNT_FMT), static_cast<unsigned>(days),
           days == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS));
  return std::string(buf);
}

// Projects the finish date from the remaining-time estimate and the book's
// observed daily pace: days left = ceil(timeLeft / dailySeconds), added to
// the current date.
std::string formatEstimatedFinishDate(const BookReadingStats& stats, const ReadingStatsDateTime& now) {
  if (!stats.startDate.isValid() || stats.estimatedTimeLeftSeconds == 0 || stats.totalReadingSeconds == 0) {
    return std::string(tr(STR_STATS_VALUE_UNAVAILABLE));
  }
  const uint16_t daysElapsed = std::max<uint16_t>(1, readingSpanDaysElapsed(stats.startDate, now.date));
  const uint32_t dailySeconds = stats.totalReadingSeconds / daysElapsed;
  if (dailySeconds == 0) {
    return std::string(tr(STR_STATS_VALUE_UNAVAILABLE));
  }
  constexpr uint32_t maxDaysLeft = 3650;  // clamp far-out projections (also bounds the seconds product)
  const uint32_t roundedDaysLeft =
      stats.estimatedTimeLeftSeconds / dailySeconds + (stats.estimatedTimeLeftSeconds % dailySeconds != 0 ? 1U : 0U);
  const uint32_t daysLeft = std::min(maxDaysLeft, roundedDaysLeft);
  ReadingStatsDateTime finish = now;
  addSecondsToReadingStatsDateTime(finish, daysLeft * 86400U);
  return formatStatsDate(finish.date);
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
  startedRowLabel.clear();
  clearPaceRow = -1;
  // n = 22: 8 card cells + 2 RTC rows + 11 bucket rows + the clear action
  // (+ 1 buffer).
  const size_t n = 22;
  valueCache.reserve(n + 1);
  rowItems.reserve(n + 1);

  // Tracking off: this header banner sits at the very top (design §7.1.1) and
  // clarifies the rows below are frozen at their last-saved values. isHeader
  // makes it underlined and non-selectable/non-interactive automatically.
  if (!SETTINGS.shouldTrackReadingStats()) {
    fui::ListItem banner;
    banner.isHeader = true;
    banner.label = tr(STR_STATS_DISABLED);
    rowItems.push_back(banner);
  }

  const auto addRow = [&](const char* label, std::string value) {
    valueCache.push_back(std::move(value));
    fui::ListItem item;
    item.label = label;
    item.value = valueCache.back().c_str();
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  };

  ReadingStatsDateTime now;
  const bool hasRtc = getCurrentLocalReadingStatsDateTime(now);

  // Card-grid row order per design §7.1: Sessions / Reading Time / Progress /
  // Avg Session / Time Left / Reading Speed / Pages Turned / Completed, then
  // the RTC-gated Started and Est. Finish rows, the bucket rows, and the
  // clear action.
  addRow(tr(STR_STATS_SESSIONS_LBL), formatStatCell(stats.sessionCount));
  addRow(tr(STR_STATS_TIME_LBL), formatStatsDuration(stats.totalReadingSeconds));
  const bool progressKnown = stats.lastBookProgressPercent != UNKNOWN_BOOK_PROGRESS_PERCENT;
  if (progressKnown) {
    char progressBuf[8];
    snprintf(progressBuf, sizeof(progressBuf), tr(STR_STATS_PROGRESS_FMT),
             static_cast<unsigned>(stats.lastBookProgressPercent));
    addRow(tr(STR_STATS_PROGRESS_LBL), std::string(progressBuf));
  } else {
    addRow(tr(STR_STATS_PROGRESS_LBL), tr(STR_STATS_VALUE_UNAVAILABLE));
  }
  const uint32_t avgSession = stats.sessionCount > 0 ? stats.totalReadingSeconds / stats.sessionCount : 0;
  addRow(tr(STR_STATS_AVG_SESSION_LBL), formatStatsDuration(avgSession));
  // Time Left shows the persisted estimate from the last session commit
  // (EpubReaderActivity runs the cached -> pace-based chain before saving);
  // "--" once the book is completed or before the first estimate exists.
  if (!stats.isCompleted && stats.estimatedTimeLeftSeconds > 0) {
    char compact[16];
    formatCompactReadingDuration(stats.estimatedTimeLeftSeconds, compact, sizeof(compact));
    addRow(tr(STR_TIME_LEFT), std::string(compact));
  } else {
    addRow(tr(STR_TIME_LEFT), tr(STR_STATS_VALUE_UNAVAILABLE));
  }
  // Reading speed is sourced from the WPM window only; the legacy
  // seconds-per-page average was dropped during the v5 -> v6 migration. Show
  // the WPM value directly so the row stays informative. Use the localized
  // "{0} WPM" / "-" strings instead of building the suffix inline.
  char wpmBuf[24];
  if (stats.wpm.count > 0) {
    snprintf(wpmBuf, sizeof(wpmBuf), tr(STR_STATS_WPM_VALUE), static_cast<unsigned>(stats.wpm.avg));
  } else {
    snprintf(wpmBuf, sizeof(wpmBuf), "%s", tr(STR_STATS_WPM_UNAVAILABLE));
  }
  addRow(tr(STR_STATS_AVG_PAGE_PACE), wpmBuf);
  addRow(tr(STR_STATS_PAGES_LBL), formatStatCell(stats.totalPagesTurned));
  addRow(tr(STR_STATS_COMPLETED), stats.isCompleted ? tr(STR_YES) : tr(STR_NO));
  if (hasRtc) {
    // Started: the label carries the start date; the value is how long ago
    // that was. The composed label outlives the list (member string), since
    // ListItem borrows the char pointer.
    if (stats.startDate.isValid()) {
      char startedBuf[64];
      snprintf(startedBuf, sizeof(startedBuf), tr(STR_STATS_STARTED_DATE_FMT), tr(STR_STATS_STARTED),
               formatStatsDate(stats.startDate).c_str());
      startedRowLabel = std::string(startedBuf);
      addRow(startedRowLabel.c_str(), formatDayCount(readingSpanDaysElapsed(stats.startDate, now.date)));
    }
    if (stats.isCompleted) {
      addRow(tr(STR_STATS_FINISHED_DATE), formatStatsDate(stats.finishedDate));
    } else {
      addRow(tr(STR_STATS_EST_FINISH_DATE), formatEstimatedFinishDate(stats, now));
    }
  }
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

  clearPaceRow = static_cast<int>(rowItems.size());
  addRow(tr(STR_CLEAR_BOOK_PACE), std::string());
}

void BookStatsActivity::activateIndex(const int index) {
  if (index == clearPaceRow) {
    setResult(ActivityResult{ClearPaceResult{}});
    finish();
  }
}

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
