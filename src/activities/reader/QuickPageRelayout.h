#pragma once

// QuickPageRelayout — shared current-page relayout/paint seam for the quick
// font surfaces (the reader's inline font sheet and FontPreviewActivity).
//
// quickRelayoutPage() runs the transient ChapterLayout pass around a char
// anchor (design ttf/2026-09-10 §6 "page-only relayout"): the engine lays the
// chapter from its start, the sink skips ahead by Page::charStart, and every
// candidate page is deep-copied out of the per-page arena with
// QuickPageCapture instead of painted inline (issue #137 — one paint per
// change, not one per scanned page). paintCapturedPage() is the extracted
// body of EpubReaderActivity::paintTtfPage so the preview paints the captured
// page with the exact pipeline the reader uses (AA gray-parity rules
// included) rather than a fork of it.
//
// No committed cache is touched and no FIBP worker state changes; the caller
// owns invalidation, cursor bookkeeping, and the session capture buffer.

#include <GfxRenderer.h>
#include <Memory.h>
#include <layout/ChapterLayout.h>

#include "QuickPageCapture.h"
#include "TtfBookRuntime.h"

struct QuickRelayoutResult {
  bool reachedAnchor = false;  // a page past the anchor confirms the last capture
  bool captured = false;       // a candidate page was deep-copied (ready to paint)
  uint32_t pageIndex = 0;
  void* font = nullptr;  // the FontChain the layout ran with (loader-owned; valid until a reload)
};

// Relayout the page containing `anchorChar` in `spineIndex` at the CURRENT
// settings into `capture`. `autoPageTurn` mirrors the reader's
// automaticPageTurnActive flag so the preview matches the layout a close
// reflow would produce. `sessionBuffer` is the caller's one-per-session
// pooled backing buffer: allocated on first use (PSRAM via poolMakeBytes),
// reused for every change, freed by the owner when its surface closes.
// Returns the outcome; painting (paintCapturedPage) is the caller's decision.
QuickRelayoutResult quickRelayoutPage(freeink::book::TtfBookRuntime& ttf, GfxRenderer& renderer,
                                      const uint16_t spineIndex, const uint32_t anchorChar, const bool autoPageTurn,
                                      QuickPageCapture& capture, PoolBytes& sessionBuffer,
                                      const uint8_t maxPages = 128);

// Paint a transient engine Page (captured or freshly read) with the reader's
// TTF page pipeline: text via the shared uniform quantizer when AA gray
// parity holds, 1bpp engine path otherwise; images per the user's display
// policy. Extracted from EpubReaderActivity::paintTtfPage — keep this body
// and the reader member in lockstep (the member now delegates here).
void paintCapturedPage(const freeink::book::Page& page, void* font, GfxRenderer& renderer,
                       freeink::book::TtfBookRuntime& ttf);
