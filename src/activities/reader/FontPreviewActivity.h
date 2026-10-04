#pragma once

// FontPreviewActivity — full-screen current-page font preview (design
// docs/design/2026-09-27-quick-font-preview.md). Replaces the reader's inline
// quick font sheet: the captured page fills the screen and a bottom chrome
// sheet exposes the Size and Family rows. Every confirmed change re-lays the
// page around the entry anchor through the shared quickRelayoutPage() seam
// (transient pass, no committed-cache work, no indexing) and repaints
// immediately.
//
// Close contract:
//   - no changes (family and point size still the entry values): pops
//     cancelled — the reader repaints the page it still owns; zero reflow,
//     zero SD writes.
//   - changed: pops QuickFontPreviewResult{changed=true}; the reader's result
//     handler runs applyReaderTextSettings() — the SAME settings-driven
//     invalidation a Text-settings font change lands (full clean rebuild,
//     position preserved through the page's char offset).
//
// Changes are persisted per apply (Text-settings discipline) so a home
// gesture or sleep mid-preview cannot lose them; the close reindex is the
// reader's, not this activity's.

#include <Memory.h>

#include "QuickPageCapture.h"
#include "activities/Activity.h"
#include "activities/reader/ReaderToolbarUi.h"
#include "components/OptionPopup.h"

class GfxRenderer;
class MappedInputManager;

namespace freeink::book {
class TtfBookRuntime;
}

#if defined(CROSSPOINT_TTF_READER)

class FontPreviewActivity final : public Activity {
 public:
  // Reader-side seam: called BEFORE the first loader reload a family change
  // triggers, so the reader can pause machinery whose faces borrow the
  // loader's resident bytes (FibpPrefetchWorker; issue #168 latent hazard).
  struct Host {
    void* ctx = nullptr;
    void (*onFamilyChanging)(void* ctx) = nullptr;
  };

  explicit FontPreviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               freeink::book::TtfBookRuntime* ttf, uint16_t spineIndex, uint32_t anchorChar,
                               bool autoPageTurn, Host host);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  void applySize(uint32_t value);
  void openSizeSlider();
  void openFamilyPicker();
  void openLineSpacingPicker();
  void openWordSpacingSlider();
  void openCharacterSpacingPicker();
  void relayout();
  void close();
  void openRow(int row);

  freeink::book::TtfBookRuntime* ttf_;
  const uint16_t spineIndex_;
  const uint32_t anchorChar_;
  const bool autoPageTurn_;
  Host host_;

  std::unique_ptr<ReaderToolbarUi> chrome_;
  OptionPopup<33, 8> familyPopup_;
  // The spacing rows share one modal: only one can be open at a time, and the
  // loop() input branch treats any active popup the same way.
  OptionPopup<8, 4> popup_;
  // One capture buffer per preview session (PSRAM), freed in onExit.
  QuickPageCapture preview_;
  PoolBytes previewBuf_;
  void* relayoutFont_ = nullptr;

  // Rows: 0 = Size, 1 = Family, 2 = Line spacing, 3 = Word spacing,
  // 4 = Character spacing. Every row re-lays the page live.
  int cursorRow_ = 0;
  bool needsRelayout_ = false;
  // Entry snapshots of every row: a net-zero session closes silently (zero
  // reflow, zero SD writes) however many rows the user visited.
  uint8_t entrySize_ = 0;
  char entryFamily_[48] = "";
  uint8_t entryLineSpacing_ = 0;
  uint8_t entryWordSpacing_ = 0;
  uint8_t entryCharacterSpacing_ = 0;
  // The page the last confirmed relayout resolved for the entry anchor, under
  // the settings now in SETTINGS. Handed to the reader so its reindex starts
  // from this page instead of re-deriving the position from a char offset that
  // cannot map until the chapter is fully indexed.
  uint32_t resolvedPage_ = 0;
  bool resolvedAnchor_ = false;
};
#endif  // CROSSPOINT_TTF_READER
