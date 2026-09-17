#pragma once
#include <I18n.h>

#include <array>
#include <optional>
#include <string>

#include "activities/UiListActivity.h"
#include "activities/reader/BookReadingStats.h"

class ReadingRhythmActivity;
class FinishedBooksActivity;

// Dev-only raw-window debug page: exists only in serial-debug builds with
// reading stats compiled in (same gate as ReadingStatsDebugActivity).
#if defined(READING_STATS_ENABLED) && defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
#define READING_STATS_DEBUG_PAGE 1
#else
#define READING_STATS_DEBUG_PAGE 0
#endif

// Contextual pager for the reading-stats screens. With a book context it adds
// the per-book entry; otherwise it only shows the device-wide screens.
class ReadingStatsMenuActivity final : public UiListActivity {
 public:
  enum class StatsEntry : uint8_t {
    ThisBook,
    ThisDevice,
    ReadingRhythm,
    FinishedBooks
#if READING_STATS_DEBUG_PAGE
    ,
    DebugRaw
#endif
  };

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
  // Compacted rows: visible row i is menuRowItems[i] with target rowEntries[i].
  freeink::ui::ListItem menuRowItems[4 + READING_STATS_DEBUG_PAGE]{};
  std::array<StatsEntry, 4 + READING_STATS_DEBUG_PAGE> rowEntries{};
  int rowCount = 0;

  void rebuildRowItems();
  void openThisBook();
  void openThisDevice();
  void openReadingRhythm();
  void openFinishedBooks();
#if READING_STATS_DEBUG_PAGE
  void openDebugRaw();
#endif
};
