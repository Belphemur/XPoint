#include "TtfRenderDebugActivity.h"

#if defined(CROSSPOINT_TTF_DEBUG)

#include <Arduino.h>  // ESP heap counters
#include <BookFontLoader.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <render/PageRenderer.h>

#include <cstdint>
#include <cstring>

#include "CrossPointSettings.h"
#include "adapters/FrameTargetFactory.h"
#include "adapters/SdCardBookSource.h"
#include "fontIds.h"

namespace book = freeink::book;

namespace {

constexpr char kDebugTextPath[] = "/.crosspoint/ttf_debug.txt";
constexpr size_t kScratchBytes = 24576;
constexpr uint16_t kBaseSizePx = 29;  // 14pt at 150 DPI

// Hard-coded plain-text chapter (flash); paragraphs split on blank lines.
const char kDebugText[] =
    "It was a bright cold day in April, and the clocks were striking "
    "thirteen. Winston Smith, his chin nuzzled into his breast in an effort "
    "to escape the vile wind, slipped quickly through the glass doors of "
    "Victory Mansions, though not quickly enough to prevent a swirl of "
    "gritty dust from entering along with him.\n\n"
    "The hallway smelt of boiled cabbage and old rag mats. At one end of it "
    "a coloured poster, too large for indoor display, had been tacked to the "
    "wall. It depicted simply an enormous face, more than a metre wide: the "
    "face of a man of about forty-five, with a heavy black moustache and "
    "ruggedly handsome features.\n\n"
    "There were no windows in the room at all. On either side of it there "
    "were shelves, and the shelves were crowded with books — reference "
    "volumes, dictionaries, atlases, and the faded spines of novels nobody "
    "had opened in years. The single lamp on the desk cast a pool of yellow "
    "light across the open page, and outside the wind kept worrying the loose "
    "tiles on the roof next door.\n\n"
    "He had the feeling that he had seen this room before, or one exactly "
    "like it, in some earlier fragment of his life: the same low ceiling, "
    "the same humming radiator, the same small clock whose second hand "
    "ticked with the patience of something that had all the time in the "
    "world. He sat down, unfolded the paper, and began, at last, to read.\n";

// Static BSS (not heap): measuring the heap delta is the point of this rig.
alignas(alignof(max_align_t)) uint8_t s_scratch[kScratchBytes];

// Renders page 0 as it arrives, then stops layout. Runs are consumed in the
// callback — they are valid only while onPage() executes.
struct DebugSink : book::PageSink {
  book::FontChain& fonts;
  const book::FrameTarget& target;
  bool rendered = false;

  DebugSink(book::FontChain& f, const book::FrameTarget& t) : fonts(f), target(t) {}

  bool onPage(const book::Page& page) override {
    if (!rendered && page.runCount > 0) {
      rendered = true;
      book::PageRenderer::renderText(page, fonts, target);
    }
    return false;  // first page only
  }
};

}  // namespace

void TtfRenderDebugActivity::onEnter() {
  Activity::onEnter();

  const uint32_t heapBefore = ESP.getFreeHeap();
  const uint32_t psramBefore = ESP.getFreePsram();
  LOG_INF("TTFDBG", "heap before: %u psram: %u", heapBefore, psramBefore);

  const auto showFatal = [this]() {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_TTF_DEBUG_RENDER), true);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    requestUpdate();
  };

#if !defined(BOARD_HAS_PSRAM)
  if (ESP.getFreeHeap() < 32768) {
    LOG_ERR("TTFDBG", "heap gate: %u < 32768", ESP.getFreeHeap());
    showFatal();
    return;
  }
#endif

  // Seed the debug chapter once. The settings dir may not exist on a fresh
  // card; openFileForWrite does not create parent directories.
  if (!Storage.exists(kDebugTextPath)) {
    Storage.ensureDirectoryExists("/.crosspoint");
    HalFile f;
    if (!Storage.openFileForWrite("TTFDBG", kDebugTextPath, f)) {
      LOG_ERR("TTFDBG", "text seed write failed: %s", kDebugTextPath);
      showFatal();
      return;
    }
    if (f.write(kDebugText, strlen(kDebugText)) != strlen(kDebugText)) {
      LOG_ERR("TTFDBG", "text seed short write: %s", kDebugTextPath);
      showFatal();
      return;
    }
  }

  book::fontLoader.ensureLoaded();
  book::FontChain* fonts = book::fontLoader.getReaderFont();
  book::SdCardBookSource source(kDebugTextPath);
  if (!source.isValid()) {
    LOG_ERR("TTFDBG", "debug text source unavailable: %s", kDebugTextPath);
    showFatal();
    return;
  }

  book::LayoutParams params;
  params.pageWidth = renderer.getScreenWidth();
  params.pageHeight = renderer.getScreenHeight();
  int viewTop, viewRight, viewBottom, viewLeft;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);
  params.marginTop = static_cast<int16_t>(viewTop + SETTINGS.screenMargin);
  params.marginRight = static_cast<int16_t>(viewRight + SETTINGS.screenMargin);
  params.marginBottom = static_cast<int16_t>(viewBottom + SETTINGS.screenMargin);
  params.marginLeft = static_cast<int16_t>(viewLeft + SETTINGS.screenMargin);
  params.baseSizePx = kBaseSizePx;
  params.language = "en";
  params.font = fonts;

  book::Arena scratch;
  scratch.init(s_scratch, sizeof(s_scratch));

  book::FrameTarget target = makeFrameTarget(renderer);
  renderer.clearScreen(0xFF);  // white background; PageRenderer inks glyphs only

  DebugSink sink(*fonts, target);
  uint32_t pageCount = 0;
  const book::BookStatus status = book::ChapterLayout::layoutPlainText(source, params, scratch, sink, &pageCount);

  LOG_INF("TTFDBG", "status=%s pages=%u scratchHigh=%u heap %u->%u psram %u->%u", bookStatusName(status), pageCount,
          scratch.highWater(), heapBefore, ESP.getFreeHeap(), psramBefore, ESP.getFreePsram());

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  requestUpdate();
}

void TtfRenderDebugActivity::loop() {
  mappedInput.update();
  using B = MappedInputManager::Button;
  if (mappedInput.wasReleased(B::Back) || mappedInput.wasReleased(B::Confirm) || mappedInput.wasReleased(B::Left) ||
      mappedInput.wasReleased(B::Right) || mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::Down) ||
      mappedInput.wasReleased(B::PageBack) || mappedInput.wasReleased(B::PageForward)) {
    finish();
  }
}

#endif  // CROSSPOINT_TTF_DEBUG
