#pragma once

#include <string>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "activities/Activity.h"

// Per-book reading-stats viewer. The summary page keeps the reader's
// Clear-pace Confirm contract; Edit Dates is a second page and Achievement is
// the completion celebration used by the reader flow.
class BookStatsActivity final : public Activity {
 public:
  enum class InitialPage : uint8_t { Summary, Achievement };

 private:
  enum class Page : uint8_t { Summary, EditDates, Achievement };

  std::string bookTitle;
  std::string bookAuthor;
  std::string bookCachePath;
  std::string truncatedTitle;  // cached in onEnter() after the orientation flip
  int titleX = 0;              // cached text origin (screenWidth - titleWidth) / 2
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Orientation::Portrait;
  Page page = Page::Summary;
  int selectedEditField = 0;
  bool didChangeStats = false;

  bool hasEditableBook() const { return !bookCachePath.empty(); }
  void cycleEditField();
  void adjustSelectedDateField(int delta);
  void applyCompletedState(bool completed);
  ReadingStatsDate defaultDateForField(bool finishedField) const;
  void clearEditedDate(bool finishedField);
  bool shouldClearDateOnAdjust(const ReadingStatsDate& date, bool finishedField, int fieldIndex, int delta) const;
  void normalizeEditedDates(bool editedFinishedField);
  void saveStats();

 public:
  explicit BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                             std::string author, const BookReadingStats& stats, std::string bookCachePath = {},
                             const GlobalReadingStats& initialGlobalStats = {},
                             InitialPage initialPage = InitialPage::Summary);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
