#pragma once
#include <I18n.h>

#include <vector>

#include "activities/Activity.h"
#include "activities/reader/FinishedBooksIndex.h"
#include "activities/reader/GlobalReadingStats.h"

class FinishedBooksActivity final : public Activity {
  GlobalReadingStats globalStats;
  std::vector<FinishedBookEntry> finishedBooks;
  size_t finishedBooksPage = 0;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Orientation::Portrait;

 public:
  explicit FinishedBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
