#pragma once

#include <I18n.h>
#include <activities/UiListActivity.h>
#include <components/OptionPopup.h>

#include <string>

#include "PowerSettings.h"

// Device-side "Power" submenu (System tab): the endurance governor's profile and
// telemetry, the clocks the ladder resolved, the heavy-job clock hold, and the
// sleep/auto-power-off intervals. Mirrors HomeButtonSettingsActivity's structure;
// the settings stay in the shared SettingsList so the web API is unaffected.
class PowerSettingsActivity final : public UiListActivity {
 public:
  PowerSettingsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("PowerSettings", renderer, input) {}

 private:
  static constexpr int ROW_COUNT = power_menu::ROW_COUNT;

  freeink::ui::ListItem rowItems_[ROW_COUNT]{};
  std::string rowValues_[ROW_COUNT];
  OptionPopup<> optionPopup;

  int listCount() const override { return ROW_COUNT; }
  const char* headerTitle() const override { return tr(STR_POWER); }
  void onEnter() override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void render(RenderLock&&) override;

  void openSleepTimeoutPicker();
  void openAutoPowerOffPicker();
};
