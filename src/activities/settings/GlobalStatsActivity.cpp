#include "GlobalStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"
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

std::string formatStreak(uint16_t streak) {
  if (streak == 0) {
    return tr(STR_STATS_NO_STREAK);
  }
  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_STATS_DAY_STREAK_FORMAT), static_cast<unsigned>(streak));
  return std::string(buf);
}

}  // namespace

GlobalStatsActivity::GlobalStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("GlobalStats", renderer, mappedInput, /*wantsTouchLongPress=*/false) {}

void GlobalStatsActivity::onEnter() {
  UiListActivity::onEnter();
#ifdef READING_STATS_ENABLED
  // Load even when tracking is disabled so the viewer still shows the
  // last-saved (frozen) values under the "Reading stats are disabled" banner
  // (design §7.1.1) instead of a freshly zeroed record.
  globalStats = GlobalReadingStats::load();
#endif
  rebuildRowItems();
}

void GlobalStatsActivity::rebuildRowItems() {
  valueCache.clear();
  rowItems.clear();
  clearPaceRow = -1;
  // n = 10: 8 metric rows + the clear action (+ 1 buffer); the bucket rows are
  // rendered as the chart band below the list (design §7.2/§7.3).
  const size_t n = 10;
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
  const ReadingStatsDate* today = getCurrentLocalReadingStatsDateTime(now) ? &now.date : nullptr;

  // Card-grid row order per design §7.2: Sessions / Reading Time / Pages
  // Turned / Reading Speed / Avg Session / Reading Streak / Longest Streak /
  // Books Read, then the clear action.
  addRow(tr(STR_STATS_SESSIONS_LBL), formatStatCell(globalStats.totalSessions));
  addRow(tr(STR_STATS_TIME_LBL), formatStatsDuration(globalStats.totalReadingSeconds));
  addRow(tr(STR_STATS_PAGES_LBL), formatStatCell(globalStats.totalPagesTurned));
  // Cross-book reading speed from the global WPM window. Same display rule as
  // BookStatsActivity: "{0} WPM" when the window has at least one sample,
  // "-" otherwise. A full window (WPM_WINDOW_SIZE samples) is required for
  // resolveReadingPaceSecondsPerPage to use this value as the primary
  // estimate for a fresh book; the partial-window state is still informative
  // and surfaced here.
  {
    char wpmBuf[24];
    if (globalStats.wpm.count > 0) {
      snprintf(wpmBuf, sizeof(wpmBuf), tr(STR_STATS_WPM_VALUE), static_cast<unsigned>(globalStats.wpm.avg));
    } else {
      snprintf(wpmBuf, sizeof(wpmBuf), "%s", tr(STR_STATS_WPM_UNAVAILABLE));
    }
    addRow(tr(STR_STATS_AVG_PAGE_PACE), wpmBuf);
  }
  const uint32_t avgSession =
      globalStats.totalSessions > 0 ? globalStats.totalReadingSeconds / globalStats.totalSessions : 0;
  addRow(tr(STR_STATS_AVG_SESSION_LBL), formatStatsDuration(avgSession));
  addRow(tr(STR_STATS_READING_STREAK_LBL), formatStreak(globalStats.currentReadingStreakDays(today)));
  addRow(tr(STR_STATS_LONGEST_STREAK_LBL), formatStreak(globalStats.longestReadingStreakDays()));
  addRow(tr(STR_STATS_COMPLETED_LBL), formatStatCell(globalStats.completedBooks));

  clearPaceRow = static_cast<int>(rowItems.size());
  addRow(tr(STR_CLEAR_GLOBAL_PACE), std::string());
}

void GlobalStatsActivity::activateIndex(const int index) {
  if (index == clearPaceRow) {
    globalStats.clearWpmStats();
    globalStats.save();
    rebuildRowItems();
    requestUpdate();
  }
}

bool GlobalStatsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }
  return false;
}

int GlobalStatsActivity::chartBandHeight() const {
  // Section-title line + the taller of the two bar charts (7 day-of-week
  // rows) + the heatmap (7 day rows at 5px pitch) + the gaps between them.
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  constexpr int BAR_ROW_H = 16;
  constexpr int HEATMAP_H = 7 * 5 - 1;  // drawHeatmapGrid: 4px cells + 1px gaps
  constexpr int GAP = 8;
  return lineH + READING_DAY_OF_WEEK_COUNT * BAR_ROW_H + GAP + lineH + GAP + HEATMAP_H + GAP;
}

void GlobalStatsActivity::drawCharts() {
  if (!chartsVisible || chartBandRect.empty()) {
    return;
  }
  const Rect band{chartBandRect.x, chartBandRect.y, chartBandRect.width, chartBandRect.height};
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  constexpr int GAP = 8;
  const int heatmapH = 7 * 5 - 1;
  const int barsH = band.height - lineH - GAP - lineH - GAP - heatmapH - GAP;
  if (barsH <= 0) {
    return;
  }

  const int halfW = band.width / 2;
  const char* todLabels[READING_TIME_BUCKET_COUNT] = {tr(STR_STATS_MORNING), tr(STR_STATS_AFTERNOON),
                                                      tr(STR_STATS_EVENING), tr(STR_STATS_NIGHT)};
  const char* dowLabels[READING_DAY_OF_WEEK_COUNT] = {tr(STR_STATS_MON), tr(STR_STATS_TUE), tr(STR_STATS_WED),
                                                      tr(STR_STATS_THU), tr(STR_STATS_FRI), tr(STR_STATS_SAT),
                                                      tr(STR_STATS_SUN)};

  const int titleY = band.y;
  const int barsY = titleY + lineH + GAP;
  const int titleHalfW = halfW - GAP;
  const int todTitleX =
      band.x + std::max(0, (titleHalfW - renderer.getTextWidth(SMALL_FONT_ID, tr(STR_STATS_TIME_OF_DAY)))) / 2;
  const int dowTitleX =
      band.x + halfW +
      std::max(0, (band.width - halfW - renderer.getTextWidth(SMALL_FONT_ID, tr(STR_STATS_DAY_OF_WEEK)))) / 2;
  renderer.drawText(SMALL_FONT_ID, todTitleX, titleY, tr(STR_STATS_TIME_OF_DAY), true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, dowTitleX, titleY, tr(STR_STATS_DAY_OF_WEEK), true, EpdFontFamily::BOLD);

  // Time-of-day on the left, day-of-week on the right: each chart divides the
  // shared bar-band height by its own row count, so the 4 time-of-day rows
  // render taller than the 7 day-of-week rows.
  const Rect todRect{static_cast<int16_t>(band.x), static_cast<int16_t>(barsY), static_cast<int16_t>(titleHalfW),
                     static_cast<int16_t>(barsH)};
  const Rect dowRect{static_cast<int16_t>(band.x + halfW), static_cast<int16_t>(barsY),
                     static_cast<int16_t>(band.width - halfW), static_cast<int16_t>(barsH)};
  BaseTheme::drawBarChart(renderer, todRect, todLabels, globalStats.timeOfDaySeconds.data(), READING_TIME_BUCKET_COUNT);
  BaseTheme::drawBarChart(renderer, dowRect, dowLabels, globalStats.dayOfWeekSeconds.data(), READING_DAY_OF_WEEK_COUNT);

  // Full-width reading-history heatmap below the charts; the newest day
  // anchors the rightmost week.
  const Rect heatRect{static_cast<int16_t>(band.x), static_cast<int16_t>(barsY + barsH + GAP),
                      static_cast<int16_t>(band.width), static_cast<int16_t>(heatmapH)};
  BaseTheme::drawHeatmapGrid(renderer, heatRect, globalStats.readingHistoryAnchorDay, globalStats.readingHistoryBits);
}

void GlobalStatsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  // Reserve the chart band below the list when the RTC provides a date (the
  // heatmap and bucket charts are meaningless without one). The list lays out
  // in the remaining body above it.
  ReadingStatsDateTime now;
  chartsVisible = getCurrentLocalReadingStatsDateTime(now);
  chartBandRect = fui::Rect{};
  if (chartsVisible) {
    chartBandRect = screen.takeBottom(static_cast<int16_t>(chartBandHeight()), screen.theme().spaceLg);
  }
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

void GlobalStatsActivity::render(RenderLock&&) {
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

  const char* title = tr(STR_READING_STATS);
  const int titleX = contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, title, true, EpdFontFamily::BOLD);

  renderUi();
  drawCharts();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
