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

QuickRelayoutResult quickRelayoutPage(freeink::book::TtfBookRuntime& ttf, GfxRenderer& renderer,
                                      const uint16_t spineIndex, const uint32_t anchorChar,
                                      const uint32_t windowEndChar, const bool autoPageTurn, QuickPageCapture& capture,
                                      PoolBytes& sessionBuffer) {
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
  out.characterSpacingPx = params.characterSpacingPx;
  out.wordSpacingPx = params.wordSpacingPx;

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

  // Page-local window (ADR 2026-10-14): the engine lays only the page
  // containing the anchor. At anchor 0 the window is disabled (a full pass
  // would lay the whole chapter) — the sink stops it after page 0. The sink
  // bounds the capture to the entry page's span so a smaller font cannot
  // pull later pages' text into the preview.
  params.windowStartChar = anchorChar;
  QuickWindowSink sink(capture, anchorChar, windowEndChar);

  const auto st = ttf.quickLayoutPage(spineIndex, params, sink);
  out.captured = capture.ready();
  // reachedAnchor: the sink captured a page whose text reaches past the
  // anchor (a pass without one — layout failure, anchor past the chapter's
  // last char — is unconfirmed).
  out.reachedAnchor = sink.reachedAnchor();
  if (!out.reachedAnchor || st != freeink::book::BookStatus::Ok) {
    LOG_DBG("QPR", "quick relayout: window page %s (status %s)", out.reachedAnchor ? "captured" : "missed",
            freeink::book::bookStatusName(st));
  }
  return out;
}

void paintCapturedPage(const freeink::book::Page& page, void* font, GfxRenderer& renderer,
                       freeink::book::TtfBookRuntime& ttf, const int16_t characterSpacingPx,
                       const int16_t wordSpacingPx) {
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
    freeink::book::PagePaint::paintText(page, *chain, renderer, characterSpacingPx, wordSpacingPx);
  } else {
    const freeink::book::FrameTarget frameTarget = makeFrameTarget(renderer);
    freeink::book::PageRenderer::renderText(page, *chain, frameTarget, nullptr, characterSpacingPx, wordSpacingPx);
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
