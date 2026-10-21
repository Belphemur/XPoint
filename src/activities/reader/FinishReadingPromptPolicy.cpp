#include "FinishReadingPromptPolicy.h"

namespace finish_prompt {

Decision evaluate(const float progressPercent, const bool settingEnabled, const bool bookCompleted,
                  const bool promptDismissed) {
  // ADR-2: once dismissed the modal never fires again for this book — the
  // latch has no forward-page-turn reset, unlike completionPromptDismissedAtHundred.
  if (promptDismissed) return Decision::Latched;
  // Completed books are owned by the existing achievement flow; the prompt is
  // moot (and ADR-5 requires !stats.isCompleted to fire).
  if (bookCompleted) return Decision::None;
  // ADR-3: the setting gates only the new threshold modals.
  if (!settingEnabled) return Decision::None;
  // ADR-6: at/past the 99.5% completion boundary the existing end-of-book
  // flow owns the finish UX (final page / "Mark as finished?" prompt) — the
  // threshold modal never fires there, so the two prompts cannot stack.
  if (progressPercent >= 99.5f) return Decision::None;
  // ADR-5: fire when >= 90% read, showing the crossed band's value (10/5/1).
  if (progressPercent >= 99.0f) return Decision::Fire1;
  if (progressPercent >= 95.0f) return Decision::Fire5;
  if (progressPercent >= 90.0f) return Decision::Fire10;
  return Decision::None;
}

}  // namespace finish_prompt
