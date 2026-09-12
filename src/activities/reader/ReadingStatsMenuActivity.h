#pragma once
#include <I18n.h>

#include <optional>
#include <string>

#include "activities/UiListActivity.h"
#include "activities/reader/BookReadingStats.h"

class ReadingRhythmActivity;
class FinishedBooksActivity;

// Contextual pager for the reading-stats screens. With a book context it adds
// the per-book entry; otherwise it only shows the device-wide screens.
class ReadingStatsMenuActivity final : public UiListActivity {
 public:
  enum class StatsEntry : uint8_t { ThisBook, ThisDevice, ReadingRhythm, FinishedBooks };

  explicit ReadingStatsMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                    std::optional<BookReadingStats> bookStats = std::nullopt,
                                    std::string bookTitle = {}, std::string bookCachePath = {},
                                    std::string bookAuthor = {});
  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return tr(STR_READING_STATS); }

  std::optional<BookReadingStats> bookStats;
  std::string bookTitle;
  std::string bookAuthor;
  std::string bookCachePath;
  freeink::ui::ListItem menuRowItems[4]{};

  void rebuildRowItems();
  void openThisBook();
  void openThisDevice();
  void openReadingRhythm();
  void openFinishedBooks();
};
