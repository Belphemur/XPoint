#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "TouchLongPressMode.h"
#include "activities/Activity.h"
#include "activities/ActivityResult.h"
#include "util/Dictionary.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader. On touch devices a
// touch-down moves the highlight and a tap on a word looks it up directly.
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop, int initialX = -1,
                                        int initialY = -1, TouchLongPressMode mode = TouchLongPressMode::Dictionary)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        initialX(initialX),
        initialY(initialY),
        mode(mode) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Screen box of one selectable word. `text` points into the owned Page's
  // TextBlock arena (NUL-terminated), valid for this activity's lifetime.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    uint16_t textOffset;
    uint16_t textLength;
    EpdFontFamily::Style style;
    uint32_t selectionGroup;
    bool syntheticHyphen;
  };

  // A logical word may contain several rendered boxes when pagination split it
  // across lines. The segment indexes are kept in one flat vector to avoid a
  // heap allocation per word on the X4 Pro.
  struct WordSelection {
    uint32_t group;
    uint16_t segmentStart;
    uint16_t segmentCount;
  };

  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void extractWords();
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  std::string selectionText(int selectionIndex) const;
  std::string resolveFootnoteHref(const char* word) const;
  void moveVertical(int direction);
  void performLookup();
  void performLookup(const std::string& query);
  void drawHighlightRange() const;
  bool drawHighlightWithSnapshot();
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  const int initialX;
  const int initialY;
  const TouchLongPressMode mode;
  int fontId = 0;
  int lineHeight = 0;

  std::vector<WordBox> words;
  std::vector<WordSelection> selections;
  std::vector<uint16_t> selectionSegments;
  int selected = 0;
  uint16_t rowCount = 0;

  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight
  // box, so a selection move restores them and repaints only the two affected
  // boxes instead of re-running the full two-pass page render (which also
  // reloads every SD-font glyph on the page). snapshotIdx is the word whose
  // under-pixels are saved; -1 means the framebuffer no longer holds a clean
  // page (popup drawn, sub-activity shown) and the next render must be full.
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;
};
