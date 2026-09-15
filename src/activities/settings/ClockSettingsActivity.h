#pragma once
#include "activities/UiListActivity.h"

// Clock configuration under System settings: 12/24-hour format, header-clock
// display, the auto-detected time zone (read-only), and manual NTP sync. The
// zone itself is detected automatically on sync — there is no manual picker.
// Only reachable when halClock.isAvailable() — SettingsActivity gates the entry.
class ClockSettingsActivity final : public UiListActivity {
 public:
  explicit ClockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int ITEM_COUNT = 4;

  void onEnter() override;

 private:
  int listCount() const override { return ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  // Row values are flash/translation strings assigned directly into rowItems_;
  // two formatted values (the zone display and the sync row's live time) live
  // here so their pointers stay valid across the frame.
  char syncTime_[9] = {0};
  char zoneValue_[64] = {0};
  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
};
