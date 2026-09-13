#pragma once
#include <I18n.h>

#include "activities/Activity.h"
#include "activities/reader/GlobalReadingStats.h"

class ReadingRhythmActivity final : public Activity {
  GlobalReadingStats globalStats;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Orientation::Portrait;

 public:
  explicit ReadingRhythmActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
