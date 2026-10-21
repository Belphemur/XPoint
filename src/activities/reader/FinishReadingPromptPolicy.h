#pragma once

// Pure decision logic for the finish-reading prompt modal
// (docs/design/2026-10-20-finish-reading-prompt.md). Shared by
// EpubReaderActivity and XtcReaderActivity: the caller owns every side effect
// (latching stats, opening the modal, checking overlay/end-of-book state);
// evaluate() itself is stateless and host-testable.

#include <cstdint>

namespace finish_prompt {

// Threshold bands, shown as the hero readout "<N>% left to read" in the
// modal. At most one band is ever fired per book because the dismissal latch
// (ADR-2) silences the book permanently.
enum class Decision {
  None,     // no prompt: below the 10% zone or the setting is off
  Fire10,   // >= 90% read: 10% left to read
  Fire5,    // >= 95% read: 5% left to read
  Fire1,    // >= 99% read: 1% left to read
  Latched,  // dismissed previously — never fires again for this book
};

// progressPercent: 0-100 (e.g. EpubReaderActivity's computeBookProgressPercent
// or XtcReaderActivity's calculateProgress). bookCompleted: stats.isCompleted.
// promptDismissed: the per-book finishPromptDismissed latch. The at-100%
// existing STR_MARK_FINISHED_PROMPT flow is NOT handled here — the caller's
// existing goHomeOrShowCompletionAchievement() path keeps owning progress
// >= 99.5 / the final page.
Decision evaluate(int progressPercent, bool settingEnabled, bool bookCompleted, bool promptDismissed);

}  // namespace finish_prompt
