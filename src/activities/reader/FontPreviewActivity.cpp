#include "FontPreviewActivity.h"

#include <BookFontLoader.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "QuickPageRelayout.h"
#include "TtfBookRuntime.h"
#include "TtfUiFallback.h"
#include "activities/ActivityResult.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"

#if defined(CROSSPOINT_TTF_READER)

FontPreviewActivity::FontPreviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         freeink::book::TtfBookRuntime* ttf, const uint16_t spineIndex,
                                         const uint32_t anchorChar, const bool autoPageTurn, Host host)
    : Activity("FontPreview", renderer, mappedInput),
      ttf_(ttf),
      spineIndex_(spineIndex),
      anchorChar_(anchorChar),
      autoPageTurn_(autoPageTurn),
      host_(host) {}

void FontPreviewActivity::onEnter() {
  Activity::onEnter();
  snprintf(entryFamily_, sizeof(entryFamily_), "%s", SETTINGS.ttfFontFamilyName);
  entrySize_ = SETTINGS.ttfFontPointSize;
  chrome_ = makeUniqueNoThrow<ReaderToolbarUi>(renderer);
  if (chrome_) {
    chrome_->begin();
  } else {
    LOG_ERR("FPR", "OOM: preview chrome");
  }
  needsRelayout_ = true;
  // Pushed activities must requestUpdate() here or the screen sits blank for
  // up to a minute (the previous screen's pending notify was consumed).
  requestUpdate();
}

void FontPreviewActivity::onExit() {
  familyPopup_.dismiss();
  chrome_.reset();
  preview_.attach(nullptr, 0);  // buffer freed below
  previewBuf_.reset();
  relayoutFont_ = nullptr;
  Activity::onExit();
}

void FontPreviewActivity::relayout() {
  needsRelayout_ = false;
  if (ttf_ == nullptr) return;
  const auto out = quickRelayoutPage(*ttf_, renderer, spineIndex_, anchorChar_, autoPageTurn_, preview_, previewBuf_);
  relayoutFont_ = out.font;
  // An unconfirmed capture (anchor beyond the scan budget, layout failure)
  // must not be displayed as if it were the current page: drop it and let the
  // render keep the frame the preview opened over until close reflows.
  if (!out.reachedAnchor) preview_.reset();
}

void FontPreviewActivity::applySize(const uint32_t value) {
  const uint8_t clamped = static_cast<uint8_t>(std::clamp<uint32_t>(value, CrossPointSettings::TTF_FONT_POINT_SIZE_MIN,
                                                                    CrossPointSettings::TTF_FONT_POINT_SIZE_MAX));
  if (clamped == SETTINGS.ttfFontPointSize) return;
  SETTINGS.ttfFontPointSize = clamped;
  // Persist outside RenderLock, mirroring Text settings' Size tab.
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("FPR", "font preview: settings save failed");
  }
  needsRelayout_ = true;
  requestUpdate();
}

void FontPreviewActivity::openSizeSlider() {
  // Same dialog Text settings' Size tab uses — one sizing mechanism
  // everywhere (design decision 2026-09-27).
  auto sizeDialog = makeUniqueNoThrow<IntervalSelectionActivity>(
      renderer, mappedInput, "TtfPointSize", StrId::STR_FONT_SIZE, SETTINGS.ttfFontPointSize,
      CrossPointSettings::TTF_FONT_POINT_SIZE_MIN, CrossPointSettings::TTF_FONT_POINT_SIZE_MAX, 1, 2,
      StrId::STR_FONT_SIZE_VALUE);
  if (!sizeDialog) {
    LOG_ERR("FPR", "OOM: size slider");
    return;
  }
  startActivityForResult(std::move(sizeDialog), [this](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<IntervalResult>(result.data)) return;
    applySize(std::get<IntervalResult>(result.data).value);
  });
}

void FontPreviewActivity::openFamilyPicker() {
  // Same enum-picker pattern as the reader's Text panel: one modal over the
  // page. Built-in is index 0; scanned families follow in loader order.
  const uint8_t familyCount = freeink::book::fontLoader.familyCount();
  std::vector<std::string> options;
  options.reserve(1U + familyCount);
  options.emplace_back(tr(STR_BUILTIN_FONT));
  for (uint8_t i = 0; i < familyCount; ++i) {
    options.emplace_back(freeink::book::fontLoader.families()[i].name);
  }

  int currentIndex = 0;
  if (SETTINGS.ttfFontFamilyName[0] != '\0') {
    const auto* current = freeink::book::fontLoader.findFamily(SETTINGS.ttfFontFamilyName);
    if (current != nullptr) currentIndex = 1 + (current - freeink::book::fontLoader.families());
  }

  familyPopup_.show(StrId::STR_FONT_FAMILY, options, currentIndex, [this](const int idx) {
    if (idx <= 0) {
      SETTINGS.ttfFontFamilyName[0] = '\0';
    } else if (idx <= freeink::book::fontLoader.familyCount()) {
      const auto& family = freeink::book::fontLoader.families()[idx - 1];
      if (!freeink::book::fontLoader.isFamilyAvailable(family)) return;
      strncpy(SETTINGS.ttfFontFamilyName, family.name, sizeof(SETTINGS.ttfFontFamilyName) - 1);
      SETTINGS.ttfFontFamilyName[sizeof(SETTINGS.ttfFontFamilyName) - 1] = '\0';
    } else {
      return;
    }
    // The reload on the next makeLayoutParams() frees the loader's resident
    // bytes; the reader pauses machinery whose faces borrow them first.
    if (host_.onFamilyChanging != nullptr) host_.onFamilyChanging(host_.ctx);
    SETTINGS.readerFontEngine = CrossPointSettings::READER_ENGINE_TTF;
    if (!SETTINGS.saveToFile()) {
      LOG_ERR("FPR", "font preview: settings save failed");
    }
    needsRelayout_ = true;
  });
  requestUpdate();
}

void FontPreviewActivity::close() {
  // Net-effect close contract (design decision log): compare the final values
  // with the entry snapshots, so a bounced change (A→B→A) closes silently —
  // zero reflow, zero further SD writes.
  const bool changed = SETTINGS.ttfFontPointSize != entrySize_ ||
                       strncmp(SETTINGS.ttfFontFamilyName, entryFamily_, sizeof(entryFamily_)) != 0;
  ActivityResult result;
  if (changed) {
    result = QuickFontPreviewResult{true};
  } else {
    result.isCancelled = true;
  }
  setResult(std::move(result));
  finish();
}

void FontPreviewActivity::loop() {
  // Family modal owns all input while open (same shape as the reader's
  // overlayPopup branch). The selection callback applies the family; the
  // dismiss callback repaints so the relayout runs exactly once.
  if (familyPopup_.isActive()) {
    familyPopup_.handleInput(mappedInput, [this] {
      if (familyPopup_.isActive()) {
        requestUpdate();  // highlight moved
        return;
      }
      requestUpdate();  // selected or dismissed: render() re-lays if pending
    });
    return;
  }

  const auto routed = chrome_ ? chrome_->route(mappedInput) : ReaderToolbarUi::Routed{};
  if (routed.routed) {
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        close();
        return;
      case ReaderToolbarUi::Event::FontRow:
        if (routed.value == 1) {
          cursorRow_ = 1;
          openFamilyPicker();
        } else {
          cursorRow_ = 0;
          openSizeSlider();
        }
        return;
      default:
        break;
    }
    if (routed.routed) return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    cursorRow_ = 0;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    cursorRow_ = 1;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (cursorRow_ == 1) {
      openFamilyPicker();
    } else {
      openSizeSlider();
    }
    return;
  }
}

void FontPreviewActivity::render(RenderLock&&) {
  if (needsRelayout_) {
    relayout();
  }
#if CROSSPOINT_TTF_UI_FALLBACK
  // AFTER the relayout: a family change reloads the loader's resident bytes
  // inside makeLayoutParams(); the reader's seam ended the borrowed fallback
  // faces before that, and update() re-registers them against the fresh
  // bytes BEFORE any chrome text draws (issue #168). Fast pointer-compare
  // path when clean.
  freeink::book::ttfUiFallback.update(renderer);
#endif
  if (preview_.ready() && relayoutFont_ != nullptr) {
    renderer.clearScreen(0xFF);
    paintCapturedPage(preview_.page(), relayoutFont_, renderer, *ttf_);
  } else {
    // No confirmed capture (scan failed / anchor out of reach): keep the
    // frame the preview opened over rather than showing a wrong page.
    LOG_DBG("FPR", "preview: no confirmed capture, keeping previous frame");
  }

  if (chrome_) {
    ReaderToolbarUi::Model model;
    model.quickFont = true;
    model.panelTitle = tr(STR_FONT);
    model.quickSelected = cursorRow_;
    model.bottomReserve = mappedInput.hasTouch() ? 0 : UITheme::getInstance().getMetrics().buttonHintsHeight;
    std::string sizeText = std::to_string(SETTINGS.ttfFontPointSize) + " pt";
    std::string familyText = SETTINGS.ttfFontFamilyName[0] != '\0' ? SETTINGS.ttfFontFamilyName : tr(STR_BUILTIN_FONT);
    model.sizeText = sizeText.c_str();
    model.familyText = familyText.c_str();
    chrome_->setModel(model);
    chrome_->render();
    if (!mappedInput.hasTouch()) {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  }
  if (familyPopup_.isActive()) {
    familyPopup_.render(renderer);
  }
  renderer.displayBuffer();
}

#endif  // CROSSPOINT_TTF_READER
