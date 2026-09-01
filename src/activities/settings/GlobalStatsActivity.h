#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "activities/reader/GlobalReadingStats.h"

// List of aggregate (all-books) reading statistics. Launched from the settings
// "Reading Stats" action. Rows are label/value pairs built once on enter; the
// final row is a "clear global reading speed" action that clears the WPM
// window on the record this activity loaded and saves it.
class GlobalStatsActivity final : public UiListActivity {
  GlobalReadingStats globalStats;
  int clearPaceRow = -1;

  std::vector<freeink::ui::ListItem> rowItems;
  std::vector<std::string> valueCache;
  void rebuildRowItems();

 public:
  explicit GlobalStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  int listCount() const override { return static_cast<int>(rowItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleButtons() override;
};
