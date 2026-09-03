#pragma once

#include <string>

#include "BookReadingStats.h"
#include "activities/Activity.h"

// Card-grid view of a single book's reading statistics (BookStatsView).
// Confirm reports ClearPaceResult back to the reader, which owns the
// authoritative record and re-saves it; Back cancels.
class BookStatsActivity final : public Activity {
  std::string bookTitle;
  std::string truncatedTitle;  // cached in onEnter() after the orientation flip
  int titleX = 0;              // cached text origin (screenWidth - titleWidth) / 2
  BookReadingStats stats;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Orientation::Portrait;

 public:
  explicit BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                             const BookReadingStats& stats);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
