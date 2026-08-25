#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "activities/reader/GlobalReadingStats.h"

// Read-only list of aggregate (all-books) reading statistics. Launched from
// the settings "Reading Stats" action. Rows are label/value pairs built once
// on enter.
class GlobalStatsActivity final : public UiListActivity {
  GlobalReadingStats globalStats;

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
