#pragma once

#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "activities/UiListActivity.h"

// Read-only list of a single book's reading statistics. Launched from the
// reader menu (READING_STATS). Rows are label/value pairs built once on enter.
class BookStatsActivity final : public UiListActivity {
  std::string bookTitle;
  BookReadingStats stats;

  std::vector<freeink::ui::ListItem> rowItems;
  std::vector<std::string> valueCache;
  void rebuildRowItems();

 public:
  explicit BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                             const BookReadingStats& stats);
  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  int listCount() const override { return static_cast<int>(rowItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleButtons() override;
};
