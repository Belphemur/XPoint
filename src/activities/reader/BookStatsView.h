#pragma once

#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "FinishedBooksIndex.h"
#include "GlobalReadingStats.h"

class GfxRenderer;
class MappedInputManager;

constexpr size_t FINISHED_BOOKS_ENTRIES_PER_PAGE = 4;

// Card-grid renderers for the reading-stats screens, ported from crossink's
// BookStatsView. Each function clears the screen and draws the full body; the
// caller draws the screen title and the button hints on top.
namespace BookStatsView {

void renderPerBookStatsPage(GfxRenderer& renderer, const MappedInputManager* mappedInput, const std::string& bookTitle,
                            const BookReadingStats& stats, float progressPercent, bool hasEstimatedTimeLeft,
                            uint32_t estimatedTimeLeftSeconds, bool showButtonHints);

void renderGlobalStatsPage(GfxRenderer& renderer, const MappedInputManager* mappedInput, const char* screenTitle,
                           const GlobalReadingStats& stats, bool showButtonHints);

void renderNoRtcCombinedStatsPage(const GfxRenderer& renderer, const MappedInputManager* mappedInput,
                                  const std::string& bookTitle, const BookReadingStats& bookStats,
                                  float progressPercent, bool hasEstimatedTimeLeft, uint32_t estimatedTimeLeftSeconds,
                                  const GlobalReadingStats& deviceStats, const GlobalReadingStats* allDevicesStats,
                                  bool showButtonHints);

void renderReadingRhythmPage(GfxRenderer& renderer, const MappedInputManager* mappedInput,
                             const GlobalReadingStats& stats, bool showButtonHints, bool showMoreButton);

void renderFinishedBooksPage(GfxRenderer& renderer, const MappedInputManager* mappedInput,
                             const std::vector<FinishedBookEntry>& finishedBooks, const GlobalReadingStats& globalStats,
                             size_t pageIndex, bool showButtonHints, bool showPreviousPage, bool showNextPage,
                             bool showMoreButton);

void renderEditBookDatesPage(GfxRenderer& renderer, const MappedInputManager* mappedInput, const std::string& bookTitle,
                             const BookReadingStats& stats, int selectedField, bool showButtonHints);

void renderReadingAchievementPage(GfxRenderer& renderer, const MappedInputManager* mappedInput,
                                  const std::string& bookTitle, const BookReadingStats& stats,
                                  const GlobalReadingStats& globalStats, bool showButtonHints);

}  // namespace BookStatsView
