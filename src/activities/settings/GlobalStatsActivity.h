#pragma once

#include "activities/Activity.h"
#include "activities/reader/GlobalReadingStats.h"

// Card-grid view of the aggregate (all-books) reading statistics
// (BookStatsView). Confirm clears the cross-book reading-speed (WPM) window
// on the loaded record and saves it; Back cancels.
class GlobalStatsActivity final : public Activity {
  GlobalReadingStats globalStats;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Orientation::Portrait;

 public:
  explicit GlobalStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
