#pragma once

#include <cstddef>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

class GfxRenderer;

// Shared finish-reading prompt modal (docs/design/2026-10-20-finish-reading-prompt.md).
// Drawn over the live reader page — no full clear — at ¾ screen width with the
// OptionPopup chrome family (UiSliderDialog precedent): small bold centered title
// caption ("Almost there"), large centered hero readout ("N%" + "left to read"), a
// body question ("Did you finish the book?"), and a two-pill option row on touch
// boards ("Yes, finished" / "Keep reading") or two centered hint lines on button
// boards (front and side keys both = Keep reading — every physical key is a
// low-commit answer, see loop()).
//
// Input ownership: while this modal is open, page-turn input is consumed here — the
// reader never receives a gesture the modal claims. Any button / tap-outside
// dismisses (keeps reading); "Yes, finished" is touch-only, so no habitual key
// press can ever complete the book (completion still happens via the existing
// end-of-book confirmation flow).
class FinishReadingPromptActivity final : public Activity, private UiAppHost {
 public:
  explicit FinishReadingPromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const uint8_t percentRemaining);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Buttons that were already held when the modal entered (e.g. the page-turn
  // key still down from the turn that raised the prompt, or a held key whose
  // release rawInputActive()-based boards cannot see). Each one's first
  // release is dropped: that release belongs to the pre-modal press, never to
  // an answer. Bit per MappedInputManager::Button.
  uint16_t heldAtEnterButtons = 0;
  bool isStaleRelease(MappedInputManager::Button button);

  // The UiAppHost app hosts the shared prompt popup drawn over the page
  // underneath; the screen builder below assigns its content per board class.
  static void promptScreen(UiScreen& screen, void* user);
  static void onYesEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onKeepEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildPromptScreen(UiScreen& screen);

  // Remaining-percent hero readout (e.g. 10 -> "10% left to read").
  uint8_t percentRemaining;
  ButtonNavigator buttonNavigator;

  void confirm();
  void dismiss();
};
