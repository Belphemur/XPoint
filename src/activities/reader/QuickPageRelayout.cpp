#include "QuickPageRelayout.h"

#if defined(CROSSPOINT_TTF_READER)

#include <BookTypes.h>
#include <Logging.h>

#include "BookFontLoader.h"
#include "CrossPointSettings.h"
#include "adapters/FrameTargetFactory.h"
#include "adapters/PagePaint.h"
#include "render/PageRenderer.h"
#include "render/TtfFont.h"

namespace {
// Deep-copies the candidate pages out of the engine's per-page arena. The
// last page at or before the anchor is the preview target; a page past the
// anchor confirms the previous capture (see QuickPageCapture.h).
class QuickSink final : public freeink::book::PageSink {
 public:
  QuickSink(const uint32_t target, const uint8_t maxPages, bool& foundRef, uint32_t& pageIndexRef,
            QuickPageCapture& captureRef)
      : target_(target), maxPages_(maxPages), found_(foundRef), pageIndex_(pageIndexRef), capture_(captureRef) {}

  bool onPage(const freeink::book::Page& page) override {
    if (page.charStart > target_) {
      // The previous captured page is the target page: it ended before the
      // first page past the anchor. Stop with a confirmed preview.
      if (sawCandidate_) found_ = true;
      return false;
    }
    // Later pages also match until the first page past the anchor, so only
    // the last capture matters. Pages are deep-copied out of the engine's
    // per-page arena. A page too large for the buffer is SKIPPED, not
    // painted inline: an inline speculative frame would be left on screen
    // while the status bar still reported the old page.
    sawCandidate_ = true;
    pageIndex_ = page.pageIndex;
    if (!capture_.capture(page)) capture_.reset();
    if (page.pageIndex + 1 >= maxPages_) {
      budgetStopped_ = true;
      return false;
    }
    return true;
  }

  bool sawCandidate() const { return sawCandidate_; }
  bool budgetStopped() const { return budgetStopped_; }

 private:
  uint32_t target_;
  uint8_t maxPages_;
  bool& found_;
  uint32_t& pageIndex_;
  QuickPageCapture& capture_;
  bool sawCandidate_ = false;
  bool budgetStopped_ = false;
};
}  // namespace

QuickRelayoutResult quickRelayoutPage(freeink::book::TtfBookRuntime& ttf, GfxRenderer& renderer,
                                      const uint16_t spineIndex, const uint32_t anchorChar, const bool autoPageTurn,
                                      QuickPageCapture& capture, PoolBytes& sessionBuffer, const uint8_t maxPages) {
  QuickRelayoutResult out;

  // A failed layout must not leave the previous settings' capture ready: the
  // caller would otherwise paint it through a null/stale font chain.
  capture.reset();

  freeink::book::LayoutParams params;
  ttf.makeLayoutParams(renderer, params, autoPageTurn);
  if (params.font == nullptr) {
    LOG_ERR("QPR", "Quick relayout: no font chain");
    return out;
  }
  out.font = params.font;

  // One backing buffer per surface session: allocated on the first relayout,
  // freed when the owning surface closes, so changes never churn the PSRAM
  // pool. Without it the sink would fall back to painting every scanned page.
  if (!capture.attached()) {
    if (!sessionBuffer) sessionBuffer = poolMakeBytes(QuickPageCapture::kBufferBytes);
    if (sessionBuffer) {
      capture.attach(sessionBuffer.get(), QuickPageCapture::kBufferBytes);
    } else {
      LOG_ERR("QPR", "OOM: quick relayout buffer");
      return out;
    }
  }

  const uint32_t targetChar = anchorChar;
  bool found = false;
  uint32_t pageIndex = 0;
  QuickSink sink(targetChar, maxPages, found, pageIndex, capture);

  const auto st = ttf.quickLayoutPage(spineIndex, params, sink, maxPages);
  // A page past the anchor confirms the last captured page as the target; a
  // natural end-of-chapter without that confirmation means the anchor page
  // itself was the last page.
  if (sink.sawCandidate() && !sink.budgetStopped()) found = true;
  out.captured = capture.ready();
  out.reachedAnchor = found;
  out.pageIndex = pageIndex;
  if (!out.reachedAnchor || st != freeink::book::BookStatus::Ok) {
    LOG_DBG("QPR", "quick relayout: anchor %s (status %s)", out.reachedAnchor ? "reached" : "missed",
            freeink::book::bookStatusName(st));
  }
  return out;
}

void paintCapturedPage(const freeink::book::Page& page, void* font, GfxRenderer& renderer,
                       freeink::book::TtfBookRuntime& ttf) {
  // §11 Q7 construction (a): with text AA engaged the paint goes via PagePaint
  // (the tone-1 boundary of the shared uniform quantizer) and a dual plane
  // walk supplies the two gray tones through the panel's AA waveform — the
  // same 4-level pipeline the bitmap reader uses. Images keep the 1bpp
  // engine path (no plane bits for image pixels).
  const bool pageHasImages = page.imageCount > 0 && SETTINGS.imageRendering == CrossPointSettings::IMAGES_DISPLAY;
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  const bool smoothText = !freeink::book::fontLoader.effectiveMonochrome();
#else
  const bool smoothText = SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_SMOOTH;
#endif
  const bool grayParity = smoothText && !pageHasImages && renderer.grayscaleCapabilities().supported();
  auto* chain = static_cast<freeink::book::FontChain*>(font);
  if (grayParity) {
    freeink::book::PagePaint::paintText(page, *chain, renderer);
  } else {
    const freeink::book::FrameTarget frameTarget = makeFrameTarget(renderer);
    freeink::book::PageRenderer::renderText(page, *chain, frameTarget, nullptr);
    // Ruby annotations are engine records — the same pass the engine's own
    // render() runs; no CrossPoint layout involvement.
    if (page.rubyCount > 0) {
      freeink::book::PageRenderer::renderRubies(page, *chain, frameTarget);
    }
  }
  freeink::book::PageRenderer::renderRules(page, makeFrameTarget(renderer));
  if (pageHasImages) {
    const freeink::book::BookStatus st = freeink::book::PageRenderer::renderImages(
        page, ttf.source(), ttf.catalog().zip(), ttf.scratch(), makeFrameTarget(renderer));
    if (st != freeink::book::BookStatus::Ok) {
      LOG_DBG("QPR", "TTF image render failed: %s", freeink::book::bookStatusName(st));
    }
  } else if (SETTINGS.imageRendering == CrossPointSettings::IMAGES_PLACEHOLDER) {
    // §3.5 item 11: image policy is CrossPoint-side; placeholder mode draws
    // the engine's reserved geometry as an outline instead of decoding.
    for (uint16_t m = 0; m < page.imageCount; ++m) {
      const auto& image = page.images[m];
      if (image.width <= 0 || image.height <= 0) continue;
      renderer.drawRect(image.x, image.y, image.width, image.height);
    }
  }
}

#endif  // CROSSPOINT_TTF_READER
