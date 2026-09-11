#pragma once

#if defined(CROSSPOINT_TTF_DEBUG)

#include "activities/Activity.h"
// Temporary engine bring-up rig (design §4 Phase 1): lays out one hard-coded
// plain-text chapter through ChapterLayout::layoutPlainText + PageRenderer
// and flushes the result to the panel. Reachable only from a hidden Settings
// row that exists in CROSSPOINT_TTF_DEBUG builds.
class TtfRenderDebugActivity : public Activity {
 public:
  TtfRenderDebugActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TtfRenderDebug", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
};

#endif  // CROSSPOINT_TTF_DEBUG
