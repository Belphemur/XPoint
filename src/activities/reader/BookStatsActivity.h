#pragma once

#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "activities/UiListActivity.h"

// List of a single book's reading statistics. Launched from the reader menu
// (READING_STATS). Rows are label/value pairs built once on enter; the final
// row is a "clear reading speed" action that reports ClearPaceResult back to
// the reader (which owns the authoritative record and re-saves it).
class BookStatsActivity final : public UiListActivity {
  std::string bookTitle;
  BookReadingStats stats;
  int clearPaceRow = -1;

  std::vector<freeink::ui::ListItem> rowItems;
  std::vector<std::string> valueCache;
  // Backing storage for the composite "Started <date>" row label; ListItem
  // labels are borrowed const char*, so composed labels need somewhere to live.
  std::string startedRowLabel;
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
