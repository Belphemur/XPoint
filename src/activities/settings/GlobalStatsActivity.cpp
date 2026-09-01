#include "GlobalStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

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
  const size_t n = 19;
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

  addRow(tr(STR_STATS_SESSIONS_LBL), std::to_string(globalStats.totalSessions));
  addRow(tr(STR_STATS_TIME_LBL), formatStatsDuration(globalStats.totalReadingSeconds));
  addRow(tr(STR_STATS_PAGES_LBL), std::to_string(globalStats.totalPagesTurned));
  addRow(tr(STR_STATS_COMPLETED_LBL), std::to_string(globalStats.completedBooks));
  const uint32_t avgSession =
      globalStats.totalSessions > 0 ? globalStats.totalReadingSeconds / globalStats.totalSessions : 0;
  addRow(tr(STR_STATS_AVG_SESSION_LBL), formatStatsDuration(avgSession));
  addRow(tr(STR_STATS_READING_STREAK_LBL), formatStreak(globalStats.currentReadingStreak(today)));
  addRow(tr(STR_STATS_LONGEST_STREAK_LBL), formatStreak(globalStats.displayLongestReadingStreak()));
  addRow(tr(STR_STATS_MORNING), formatStatsDuration(globalStats.timeOfDaySeconds[0]));
  addRow(tr(STR_STATS_AFTERNOON), formatStatsDuration(globalStats.timeOfDaySeconds[1]));
  addRow(tr(STR_STATS_EVENING), formatStatsDuration(globalStats.timeOfDaySeconds[2]));
  addRow(tr(STR_STATS_NIGHT), formatStatsDuration(globalStats.timeOfDaySeconds[3]));
  addRow(tr(STR_STATS_MON), formatStatsDuration(globalStats.dayOfWeekSeconds[0]));
  addRow(tr(STR_STATS_TUE), formatStatsDuration(globalStats.dayOfWeekSeconds[1]));
  addRow(tr(STR_STATS_WED), formatStatsDuration(globalStats.dayOfWeekSeconds[2]));
  addRow(tr(STR_STATS_THU), formatStatsDuration(globalStats.dayOfWeekSeconds[3]));
  addRow(tr(STR_STATS_FRI), formatStatsDuration(globalStats.dayOfWeekSeconds[4]));
  addRow(tr(STR_STATS_SAT), formatStatsDuration(globalStats.dayOfWeekSeconds[5]));
  addRow(tr(STR_STATS_SUN), formatStatsDuration(globalStats.dayOfWeekSeconds[6]));

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

void GlobalStatsActivity::buildScreen(UiScreen& screen) {
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

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
