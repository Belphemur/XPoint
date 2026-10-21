#include "FinishReadingPromptPolicy.h"

namespace finish_prompt {

Decision evaluate(int progressPercent, bool settingEnabled, bool bookCompleted, bool promptDismissed) {
  // ADR-2: once dismissed the modal never fires again for this book — the
  // latch has no forward-page-turn reset, unlike completionPromptDismissedAtHundred.
  if (promptDismissed) return Decision::Latched;
  // Completed books are owned by the existing achievement flow; the prompt is
  // moot (and ADR-5 requires !stats.isCompleted to fire).
  if (bookCompleted) return Decision::None;
  // ADR-3: the setting gates only the new threshold modals.
  if (!settingEnabled) return Decision::None;
  // ADR-5: fire when >= 90% read, showing the crossed band's value (10/5/1).
  // The at-100% end-of-book flow (>= 99.5%) stays with the existing prompt.
  if (progressPercent >= 99) return Decision::Fire1;
  if (progressPercent >= 95) return Decision::Fire5;
  if (progressPercent >= 90) return Decision::Fire10;
  return Decision::None;
}

}  // namespace finish_prompt
