#include "ClockSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <memory>

#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
enum MenuItem {
  ITEM_FORMAT = 0,
  ITEM_HEADER,
  ITEM_TIME_ZONE,
  ITEM_SYNC,
};

const StrId menuNames[ClockSettingsActivity::ITEM_COUNT] = {StrId::STR_CLOCK_FORMAT, StrId::STR_HEADER_CLOCK,
                                                            StrId::STR_TIMEZONE, StrId::STR_CLOCK_SYNC_NOW};
}  // namespace

ClockSettingsActivity::ClockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ClockSettings", renderer, mappedInput) {}

void ClockSettingsActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

const char* ClockSettingsActivity::headerTitle() const { return tr(STR_CLOCK); }

void ClockSettingsActivity::activateIndex(const int index) {
  nav.selected = index;
  app.clearTapFlash();
  switch (index) {
    case ITEM_FORMAT:
      SETTINGS.clockFormat = (SETTINGS.clockFormat + 1) % 2;
      break;
    case ITEM_HEADER:
      SETTINGS.headerClock = SETTINGS.headerClock ? 0 : 1;
      break;
    case ITEM_TIME_ZONE:
      // Read-only: the zone is auto-detected on sync. "Sync clock now" re-detects it.
      return;
    case ITEM_SYNC:
      if (auto activity = makeUniqueNoThrow<ClockSyncActivity>(renderer, mappedInput)) {
        startActivityForResult(std::move(activity), nullptr);
      } else {
        LOG_ERR("CLKSET", "OOM: ClockSyncActivity");
      }
      return;
    default:
      return;
  }
  SETTINGS.saveToFile();
  requestUpdate();
}

void ClockSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Every value is a flash/translation string or a member buffer, so the
  // render pass allocates nothing.
  rowItems_[ITEM_FORMAT].value = SETTINGS.clockFormat == 1 ? tr(STR_CLOCK_FORMAT_12H) : tr(STR_CLOCK_FORMAT_24H);
  rowItems_[ITEM_HEADER].value = SETTINGS.headerClock ? tr(STR_SHOW) : tr(STR_HIDE);
  // Read-only: the zone is auto-detected on sync, so the row has no action.
  // Show e.g. "America/Toronto (UTC-4)" with a DST badge when in effect.
  if (SETTINGS.clockTimeZoneId[0] == '\0') {
    rowItems_[ITEM_TIME_ZONE].value = tr(STR_NOT_SET);
  } else {
    const int off = SETTINGS.clockEffectiveOffsetMin();
    const bool neg = off < 0;
    const int absOff = neg ? -off : off;
    const char* dstBadge = SETTINGS.clockTzIsDst ? tr(STR_DST) : "";
    snprintf(zoneValue_, sizeof(zoneValue_), "%s (%s%c%d:%02d%s)", SETTINGS.clockTimeZoneId, tr(STR_UTC),
             neg ? '-' : '+', absOff / 60, absOff % 60, dstBadge);
    rowItems_[ITEM_TIME_ZONE].value = zoneValue_;
  }
  // The sync row's value is the current time itself: it confirms the sync,
  // previews format changes, and reads "Not Set" until the first sync.
  rowItems_[ITEM_SYNC].value =
      SETTINGS.clockHasBeenSynced && halClock.formatTime(syncTime_, sizeof(syncTime_),
                                                         SETTINGS.clockEffectiveOffsetMin(), SETTINGS.clockFormat == 1)
          ? syncTime_
          : tr(STR_NOT_SET);

  fui::ListProps props;
  props.items = rowItems_;
  props.count = ITEM_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
