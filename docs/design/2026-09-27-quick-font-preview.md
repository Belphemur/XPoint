# Quick-menu font sizing unification + Font preview Activity

Date: 2026-09-27
Status: implemented on `wt/quick-font-preview` (one `[task-N]` commit per
phase)
Parent: `docs/design/ttf/2026-09-10-font-architecture-and-ux.md` (§5, §6,
§14.2)

## Problem

Two inconsistencies live in the current reader font quick sheet
(`Overlay::FontSheet`):

1. **Sizing mechanism divergence.** The quick sheet sizes the font with the
   bespoke ±1 pt stepper `quickFontStep()` (`EpubReaderActivity.cpp`
   ~:4763) while Text settings uses the shared `IntervalSelectionActivity`
   slider (`"TtfPointSize"`, bounds
   `CrossPointSettings::TTF_FONT_POINT_SIZE_MIN/MAX`, smallStep 1 / largeStep
   2, `StrId::STR_FONT_SIZE_VALUE` value label) for TTF faces. Requirement
   (owner brief): the quick menu must FULLY use the same font-size mechanism
   as Text settings — bounds identical, persistence identical
   (`SETTINGS.saveToFile()` written outside `RenderLock`).
2. **Inline preview.** The quick sheet re-renders the current page from an
   inline overlay (`renderQuickFontPage()` + `QuickPageCapture`) while the
   reader activity keeps owning the whole input/routing stack underneath.
   The owner wants the quick preview text to become its OWN Activity: show
   only the current page's text (the already-captured `QuickPageCapture`
   buffer is the natural input), apply family/size changes live, do NO
   indexing while open, and on close either return silently (no changes) or
   persist and make the reader re-index CLEANLY (full rebuild per existing
   invalidation rules — not incremental patching) with the reading position
   preserved.

## Goals

- One sizing mechanism everywhere (TTF slider; legacy bitmap size list).
- A dedicated Font preview Activity with a strict close contract.
- No regression to FIBP prefetch, resume-claim, capture machinery, or the
  queued-press coalescing input pattern (hard UX rule: buttons may render-be
  busy, never dead).
- Legacy (C3, non-TTF) builds stay green and keep their current behavior.

## Non-goals

- No sha-driven cache flushes; no scheduler changes.
- No freeink-sdk work (submodule pin stays `1149d710`).
- No incremental cache patching — the clean reindex reuses the settings
  invalidation that already exists.

## Component boundary

### Quick-menu sizing unification (task-2)

The quick sheet's size row buttons stop calling `quickFontStep()`. They
launch the same `IntervalSelectionActivity` ("TtfPointSize") Text settings'
Size tab uses, from the FontSheet input path:

- `EpubReaderActivity::openQuickSizeSlider()` builds one
  `IntervalSelectionActivity` via `makeUniqueNoThrow` (OOM → LOG_ERR +
  break/return, no attempt loop) and `startActivityForResult` with these
  parameters, copied verbatim from `TextSettingsActivity::activateRow`
  Tab::Size: name `"TtfPointSize"`, title `StrId::STR_FONT_SIZE`, initial
  `SETTINGS.ttfFontPointSize`, min/max `TTF_FONT_POINT_*`, smallStep 1,
  largeStep 2, value format `StrId::STR_FONT_SIZE_VALUE`.
- The result handler applies the clamped value to `SETTINGS.ttfFontPointSize`
  and calls `SETTINGS.ttfFontPointSizeChanged()`
- After it returns, the sheet overlay is still open and paints again in the
  next reader render pass; the payload carries a `QuickRelayoutDirty` flag:
  true = the preview page must re-render (renderQuickFontPage()) before the
  sheet repaints; a cancelled dialog (isCancelled) leaves nothing.
- If the value did not actually change, the preview page is NOT re-laid-out;
  the sheet is repainted from the existing stored page snapshot.
- Bounds clamp with the SAME clamp Text settings does
  (`std::clamp<uint32_t>` against the same MIN/MAX).

### Font preview Activity (task-3)

`FontPreviewActivity` (new, `src/activities/reader/FontPreviewActivity.{h,cpp}`)
is a full-screen Activity pushed via `startActivityForResult` from the
EpubReader text panel rows ("Size" and "Family" rows route here on TTF
builds) instead of `openFontSheet()`.

- This is the SAME mechanism Text settings already uses to hand work back
  (the Tab::Size handler does exactly this), so no new navigation concepts.
- The activity is heap-allocated (`makeUniqueNoThrow`), pushed on the
  ActivityManager stack; when the user closes it the framework pops it and
  invokes the reader's result callback, standard lifecycle.

  While the preview is serving, the reader activity underneath is not
  rendered and not pumped.

- **State ownership.** The preview owns only view snapshots and its own
  input state:
  - the captured page (`QuickPageCapture` — the SAME machinery,
    `test/quick_page_capture` keeps covering it; no second capture build is
    forked),
  - the `freeink::book::LayoutParams` snapshot the capture relayout pass
    produced,
  - the pending size/family deltas that the user has edited since the last
    commit.

  The reader RETAINS ownership of the source of truth: `SETTINGS`
  (`ttfFontPointSize`, `ttfFontFamilyName`), the book cache, FIBP caches,
  the current page/anchor bookkeeping. The preview NEVER writes any of it
  directly from its own page paint; the adopt step is the single write edge,
  and it happens only on a confirmed close.

- **What is captured, when.** Entering the preview (the reader's
  "Preview" action) captures the current page with the existing
  `QuickPageCapture` machinery: a `QuickSink` scan relayouts up to the page's
  char anchor (the merge-source anchor `ttfCurrentCharStart`) and deep-copies
  the page into a pooled buffer (one allocation per preview session, PSRAM
  via `poolMakeBytes`, sized `QuickPageCapture::kBufferBytes`). This is a
  mechanical extraction of the tail of today's `renderQuickFontPage()` — the
  budget, anchor, skip-oversized-page rule, and approximate-preview rule all
  move (design doc references: *page-only relayout*, §6 of the font doc).
  The activity also snapshots `SETTINGS.ttfFontPointSize` /
  `ttfFontFamilyName` at entry as the "committed" baseline, so the close
  path can diff current view values against entry values.

- **Live apply inside the preview.** Every size step or family selection is
  an in-place view mutation with an immediate re-render on the preview page
  → `requestUpdate()`. The DOM is the captured page + the changed setting;
  the paint path is exactly `paintTtfPage`-equivalent work (the activity
  carries no reader state, so the layout+pilot run happens inside the
  preview over the capture; the engine's pages handle their own AA parity:
  same `paintTtfPage` pipeline in the reader after close).

- **Indexing freeze.** While open, the preview performs NO layout work
  beyond the capture/use-in-session relayout pages, per the brief ("while
  inside the preview activity, NO indexing happens"). No cache writes, no
  FIBP worker churn. The FIBP worker and the reader-suspended background
  build keep whatever state they had; the ActivityManager stopping the
  reader's `loop()` when the preview pushed on top is the pause mechanism.
  (`ActivityManager::pushActivity` keeps the paused activity on the stack
  and its loop() is no longer scheduled — the pool freeze is inherited from
  that.)
- **Time/unit budget.** Frames are incremental: a size step re-captures the
  page into the SAME backing buffer (attach → scan → paint), so peak memory
  is one capture buffer + the engine's existing layout scratch for a single
  page — no per-test accumulation. Sessions are single-buffer by design
  (`poolMakeBytes` → `QuickPageCapture::attach`, freed when the activity
  exits in `onExit()`), matching the quick-sheet pool contract.

### Close contract

Two exits, distinguished by whether the user changed anything:

- **No changes** (family and point size are still the entry snapshots):
  `setResult(QuickFontPreviewResult{.family = "", .pointSize = 0})` — the
  empty/mono marker documented in `ActivityResult.h` and consumed by
  `TextSettingsActivity` already for plain confirms. The reader's result
  handler does nothing (no saveToFile, no invalidation, no reflow). Zero
  reflow, zero SD writes, camera-capture buffer freed in `onExit()`.
- **Changed** (family tag or point size differs from entry):
  - the preview applies the final values with the same mutation set the
    quick sheet used before this change: `SETTINGS.ttfFontFamilyName` write
    (or `'\0'` clearing for Built-in), `loader.selectFamily(...)`,
    `SETTINGS.ttfFontPointSizeChanged()`, clamped writes — all of it
    sequenced exactly as the inline sheet does today, minus the frame
    invalidation (this is the delegation point, not a re-implementation);
  - it saves the settings itself, so the SD write happens OUTSIDE RenderLock,
    mirroring Text settings' intent comment ("persist immediately … SD write
    happens outside its RenderLock");
  - then `setResult(QuickFontPreviewResult{...})` — the reader's result
    callback runs `applyReaderTextSettings()`, the SAME settings-driven
    invalidation a Text-settings size change lands today (fingerprint fold +
    full clean rebuild). Nothing new is invented; there is nothing to
    bypass.
- **Position preservation.** Identical to the mechanism the quick sheet and
  Text settings already use: the page layout run is keyed by the per-page
  char anchor, and the reader restores positions through the saved char
  offset when the fresh generation rebuilds — the anchor is taken before the
  reindex (same `rememberCurrentContentOffset()` family or the sheet's
  pre-reflow `memoryCurrentContentOffset`); no position bookkeeping is
  invented inside the preview.

### Quick-menu / in-book Text panel wiring change

- `showTextRowPopup(row)` on TTF builds (today's TTF branch that
  `openFontSheet()`s) instead pushes a `FontPreviewActivity` result flow —
  both the "size row" and "family row" handlers converge on the single
  preview push (the row concept collapses because the full-screen preview
  exposes both controls natively).
- The inline `Overlay::FontSheet` state, `quickFontRow`,
  `quickFontFamilyPending`, `quickFontStep()`, `renderQuickFontPage()`, the
  `QuickSink` inner class, and the sheet's `builtQuickFont` toolbar branch
  become dead on the TTF path and are deleted
  (`ReaderToolbarUi::buildQuickFont`'s caller removal leaves no orphan —
  probe every caller; the toolbar model's `quickFont` flag and its
  `buildQuickFont()` renderer move into the preview activity, unchanged).
- `closeFontSheet()` and the FontSheet input routing are removed with the
  overlay; the overlay enum loses its `FontSheet` entry. Nothing else
  consumed it (verified by grep).
- The reader's Text panel had TWO rows on TTF (size, family) — both now
  route to the preview. The panel keeps its row count (no layout change).
- **Extraction note (paint seam):** the preview needs `paintTtfPage` and the
  `QuickSink` relayout scan, today private members of EpubReaderActivity.
  Extraction: the scan/paint helpers become free functions in `ReaderUtils`
  (or free-standing helpers in a small `QuickPageRelayout.h` next to the
  QuickPageCapture header), parameterized by `TtfBookRuntime&` +
  `GfxRenderer&` + the capture, taking the same arguments the member
  functions read from the activity. Both the reader (before opening the
  preview) and the preview activity call the SAME helpers. `renderQuickFontPage()`'s
  log messages and the pool-buffer lifecycle move into the helpers so the
  behavior is structurally identical.

### Legacypipeline (non-TTF builds)

- `showTextRowPopup` keeps its legacy branch exactly as today (no preview
  activity). The non-TTF compile of `FontPreviewActivity` never happens: the
  file is compiled only on `CROSSPOINT_TTF_READER` builds, and the reader
  include + call site are guarded by the same macro (the shared-file guard
  rule that hit CI for the earlier quick sheet: guard the call site with the
  same `defined(CROSSPOINT_TTF_READER)` as the declaration). The C3 build
  neither grows nor shrinks; the bitmap `applySize`/size-list path in Text
  settings is untouched.

## UI map of the preview sheet

Full-screen page view, chrome on the bottom. From the toolbar kit the
preview reuses `ReaderToolbarUi`'s existing component shapes:

- The page body occupies the whole viewable area
  (`renderer.getOrientedViewableTRBL()`; no hardcoded 800/480). The captured
  page paints with the same `paintTtfPage` helper logic (extraction: the
  static paint helper moves to a home both call — see Extraction note).
- Bottom quick bar (touch + button hints share one layout):
  - Family row: current family name, tap/Confirm opens the family
    `OptionPopup` (the same modal the quick sheet used; Built-in first, then
    scanned families in loader order; unavailable families skipped).
  - Size row: current `pt` value, Confirm opens the TTF
    `IntervalSelectionActivity` slider (TTF) or nothing (legacy C3 never
    runs this activity).
- Back gesture/button closes the sheet with the close contract above.
- All user-visible strings through `tr(StrId::...)`; new keys go through
  `scripts/gen_i18n.py` (edit `english.yaml`, regenerate — generated files
  are gitignored).

## Decision log

- 2026-09-27 — Quick menu size row switches from the ±1-pt bespoke stepper
  to the shared `IntervalSelectionActivity` ("TtfPointSize") slider — same
  bounds/persistence as Text settings (owner directive: one sizing
  mechanism, saved outside RenderLock).
- 2026-09-27 — The inline quick font overlay (`Overlay::FontSheet` +
  `renderQuickFontPage` + sibling quickFont* members) is replaced by a
  full-screen `FontPreviewActivity`; the two-text-panel rows converge on
  that activity. The sheet's pool-buffer lifecycle (one capture buffer per
  session, freed at close) moves into the activity verbatim.
- 2026-09-27 — The close contract rides the existing settings-driven
  invalidation (`applyReaderTextSettings()`); no new invalidation path, no
  incremental patching, no sha-driven flushes.
- 2026-09-27 — The preview paints via the reader's TTF page-paint code;
  rather than duplicating it, the paint body stays accessible through a
  minimal shared seam (see Extraction note) so the preview and the reader
  share one implementation (DRY).
- 2026-09-27 — The quick-sheet "approximate preview" semantics (keep the
  last laid-out page when the anchor is out of reach; never shift the
  cursor) are inherited unchanged.
- 2026-09-27 — Non-TTF builds are untouched: FontPreviewActivity/event
  routing/handlers compile only under `CROSSPOINT_TTF_READER`, and the C3
  path continues to use TextSettingsActivity full-screen (compile-and-behave
  gate, not a feature gate).
- 2026-09-27 — Commit tagging: one `[task-N]` conventional commit per brief
  phase, review fixes as `[review]` commits, fused squash tags in the PR
  body (squash merge convention).

<!--
Verify with:
grep -c "Decision log" docs/design/2026-09-27-quick-font-preview.md
-->
