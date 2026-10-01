#include "PowerSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "PowerSettings.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr int POWER_PROFILE_ITEMS = 3;
const StrId powerProfileNames[POWER_PROFILE_ITEMS] = {StrId::STR_PROFILE_ENDURANCE, StrId::STR_PROFILE_BALANCED,
                                                      StrId::STR_PROFILE_PERFORMANCE};

constexpr int POWER_STATS_ITEMS = CrossPointSettings::POWER_STATS_MODE_COUNT;
const StrId powerStatsNames[POWER_STATS_ITEMS] = {StrId::STR_STATE_OFF, StrId::STR_POWER_STATS_COMPACT,
                                                  StrId::STR_POWER_STATS_FULL};

constexpr int TOGGLE_ITEMS = 2;
const StrId toggleNames[TOGGLE_ITEMS] = {StrId::STR_STATE_OFF, StrId::STR_STATE_ON};
}  // namespace

void PowerSettingsActivity::onEnter() {
  UiListActivity::onEnter();

  // Corrupt/migrated bytes: clamp so popups and read sites below agree on a
  // mode that exists (same read-site clamping as StatusBarSettingsActivity).
  if (SETTINGS.powerStatsMode >= POWER_STATS_ITEMS) {
    SETTINGS.powerStatsMode = CrossPointSettings::POWER_STATS_MODE::POWER_STATS_OFF;
  }
  if (SETTINGS.powerProfile >= POWER_PROFILE_ITEMS) {
    SETTINGS.powerProfile = 0;  // Endurance
  }
}

bool PowerSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void PowerSettingsActivity::activateIndex(const int index) {
  if (index < 0 || index >= ROW_COUNT || optionPopup.isActive()) return;
  app.clearTapFlash();
  nav.selected = index;

  switch (power_menu::ROW_LABELS[index]) {
    case StrId::STR_POWER_PROFILE:
      optionPopup.show(StrId::STR_POWER_PROFILE, powerProfileNames, POWER_PROFILE_ITEMS, SETTINGS.powerProfile,
                       [this](int idx) {
                         SETTINGS.powerProfile = idx;
                         SETTINGS.saveToFile();
                         requestUpdate();
                       });
      return;
    case StrId::STR_POWER_STATS:
      optionPopup.show(StrId::STR_POWER_STATS, powerStatsNames, POWER_STATS_ITEMS, SETTINGS.powerStatsMode,
                       [this](int idx) {
                         SETTINGS.powerStatsMode = idx;
                         SETTINGS.saveToFile();
                         requestUpdate();
                       });
      return;
    // Heavy-job boost is a plain On/Off choice, not an interval.
    case StrId::STR_HEAVY_JOB_BOOST:
      optionPopup.show(StrId::STR_HEAVY_JOB_BOOST, toggleNames, TOGGLE_ITEMS, SETTINGS.powerHeavyJobBoost ? 1 : 0,
                       [this](int idx) {
                         SETTINGS.powerHeavyJobBoost = idx ? 1 : 0;
                         SETTINGS.saveToFile();
                         requestUpdate();
                       });
      return;
    // Idle Clock and Page Render Clock are read-only: they report the rung the
    // endurance governor resolved at runtime, so there is nothing to pick, and a
    // row that falls through to `default` is inert by construction.
    case StrId::STR_TIME_TO_SLEEP:
      openSleepTimeoutPicker();
      return;
    case StrId::STR_AUTO_POWER_OFF:
      openAutoPowerOffPicker();
      return;
    default:
      return;
  }
}

void PowerSettingsActivity::openSleepTimeoutPicker() {
  // Firmware builds compile with -fno-exceptions, so a throwing make_unique
  // aborts on OOM instead of returning control. Same no-throw + log shape as
  // SettingsActivity's own child launches.
  auto picker = makeUniqueNoThrow<IntervalSelectionActivity>(
      renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
      CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
      StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, StrId::STR_SLEEP_NEVER);
  if (!picker) {
    LOG_ERR("SET", "OOM: sleep timeout picker");
    return;
  }
  startActivityForResult(std::move(picker), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
      SETTINGS.saveToFile();
    }
    requestUpdate();
  });
}

void PowerSettingsActivity::openAutoPowerOffPicker() {
  auto picker = makeUniqueNoThrow<IntervalSelectionActivity>(
      renderer, mappedInput, "AutoPowerOffInterval", StrId::STR_AUTO_POWER_OFF, SETTINGS.autoPowerOffHours,
      CrossPointSettings::AUTO_POWER_OFF_MIN_HOURS, CrossPointSettings::AUTO_POWER_OFF_MAX_HOURS,
      CrossPointSettings::AUTO_POWER_OFF_STEP_HOURS, CrossPointSettings::AUTO_POWER_OFF_STEP_HOURS,
      StrId::STR_AUTO_POWER_OFF_HOURS_FORMAT, false, StrId::STR_STATE_OFF);
  if (!picker) {
    LOG_ERR("SET", "OOM: auto power-off picker");
    return;
  }
  startActivityForResult(std::move(picker), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      SETTINGS.autoPowerOffHours = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
      SETTINGS.saveToFile();
    }
    requestUpdate();
  });
}

void PowerSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void PowerSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (int i = 0; i < ROW_COUNT; ++i) {
    rowItems_[i].label = I18N.get(power_menu::ROW_LABELS[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
    switch (power_menu::ROW_LABELS[i]) {
      case StrId::STR_POWER_PROFILE:
        rowValues_[i] =
            I18N.get(powerProfileNames[SETTINGS.powerProfile < POWER_PROFILE_ITEMS ? SETTINGS.powerProfile : 0]);
        break;
      case StrId::STR_POWER_STATS:
        rowValues_[i] =
            I18N.get(powerStatsNames[SETTINGS.powerStatsMode < POWER_STATS_ITEMS ? SETTINGS.powerStatsMode : 0]);
        break;
      case StrId::STR_HEAVY_JOB_BOOST:
        rowValues_[i] = I18N.get(SETTINGS.powerHeavyJobBoost ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
        break;
      case StrId::STR_IDLE_CLOCK: {
        // Resolved live rather than stored: this is the ladder's current idle
        // target, which already folds in any crash-strike promotion.
        char valueBuffer[32];
        snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_PWR_MHZ_VALUE), powerManager.endurance().idleClockMHz());
        rowValues_[i] = valueBuffer;
        break;
      }
      case StrId::STR_PAGE_RENDER_CLOCK: {
        char valueBuffer[32];
        snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_PWR_MHZ_VALUE), powerManager.endurance().renderClockMHz());
        rowValues_[i] = valueBuffer;
        break;
      }
      case StrId::STR_TIME_TO_SLEEP:
        if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
          rowValues_[i] = tr(STR_SLEEP_NEVER);
        } else {
          char valueBuffer[32];
          snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                   static_cast<unsigned int>(SETTINGS.sleepTimeoutMinutes));
          rowValues_[i] = valueBuffer;
        }
        break;
      case StrId::STR_AUTO_POWER_OFF:
        if (SETTINGS.autoPowerOffHours >= CrossPointSettings::AUTO_POWER_OFF_MAX_HOURS) {
          rowValues_[i] = tr(STR_STATE_OFF);
        } else {
          char valueBuffer[32];
          snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_AUTO_POWER_OFF_HOURS_FORMAT),
                   static_cast<unsigned int>(SETTINGS.autoPowerOffHours));
          rowValues_[i] = valueBuffer;
        }
        break;
      default:
        rowValues_[i].clear();
        break;
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = ROW_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  // Keep the setting name and its current value at the same visual weight
  // (see HomeButtonSettingsActivity).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
