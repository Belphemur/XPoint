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

### Quick-menu sizing unification (as built)

The bespoke ±1 pt stepper is gone. The unified sizing mechanism lives inside
`FontPreviewActivity` (the intermediate step that first hosted it in the
inline sheet was superseded when the sheet was removed — the mechanism, not
the sheet, is the deliverable):

- `FontPreviewActivity::openSizeSlider()` builds one
  `IntervalSelectionActivity` via `makeUniqueNoThrow` (OOM → LOG_ERR +
  return) and `startActivityForResult` with the parameters Text settings'
  Size tab uses: name `"TtfPointSize"`, title `StrId::STR_FONT_SIZE`, initial
  `SETTINGS.ttfFontPointSize`, min/max `TTF_FONT_POINT_*`, smallStep 1,
  largeStep 2, value format `StrId::STR_FONT_SIZE_VALUE`.
- The result handler applies the clamped value to `SETTINGS.ttfFontPointSize`
  and persists it (`SETTINGS.saveToFile()`, outside RenderLock — Text
  settings' discipline, so a home gesture or sleep mid-preview cannot lose a
  change).
- On every applied change the preview re-lays the page around the entry
  anchor through the shared `quickRelayoutPage()` seam and repaints
  immediately (`requestUpdate()`); the e-ink FAST refresh is the debounce.
- Legacy (non-TTF) builds never open the preview: their Text panel rows keep
  the full-screen Text settings flow with the bitmap size list.

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
  new indexing triggered by the preview. The reader's own background paths
  (`ttfBackgroundBuildTick`, `ttfPrefetchTick`) only run from the reader's
  `loop()`, which the ActivityManager suspends while the preview is pushed —
  that is the pause mechanism. A live inline build session found at preview
  open is aborted and the current chapter cache reopened, so the preview can
  re-lay the page at every change (the partial stays resumable). The FIBP
  worker keeps its own independent schedule (same as during normal reading);
  its caches are fingerprint-keyed, so the close reindex invalidates stale
  generations exactly as a Text-settings change does.
- **Time/unit budget.** Frames are incremental: a size step re-captures the
  page into the SAME backing buffer (attach → scan → paint), so peak memory
  is one capture buffer + the engine's existing layout scratch for a single
  page — no per-test accumulation. Sessions are single-buffer by design
  (`poolMakeBytes` → `QuickPageCapture::attach`, freed when the activity
  exits in `onExit()`), matching the quick-sheet pool contract.

### Close contract

Two exits, decided at close time by comparing the FINAL values with the
ENTRY snapshots (family string and point size captured in `onEnter()`):

- **No net change** (values match the entry snapshots — including a bounced
  change A→B→A): the preview pops CANCELLED. The reader's result handler
  skips the reindex entirely and restores the Text panel. No further SD
  writes, no reflow. (The session may already have persisted intermediate
  values; a bounced change ends at the entry values, so nothing is lost.)
- **Changed** (family or point size differs from entry): the preview pops
  `QuickFontPreviewResult{changed=true}`.
  - During the session each applied change was already persisted
    (`SETTINGS.saveToFile()` outside RenderLock, Text-settings discipline);
    family changes reach the loader through the relayout path —
    `makeLayoutParams()` calls `fontLoader.selectFamily(SETTINGS.ttfFontFamilyName)`
    and `getReaderFont()` reloads the bytes — there is no separate commit
    step and no invented API.
  - The reader's result callback runs `applyReaderTextSettings()`, the SAME
    settings-driven invalidation a Text-settings font change lands (save +
    UI-fallback resync + `markDirty()` + cache invalidation → full clean
    rebuild). Nothing new is invented; there is nothing to bypass.
  - Both close branches return to the Text panel the preview was opened
    from.
- **Position preservation.** The reader's position machinery is untouched
  while the preview is open; the reflow restores the position through the
  saved char offset (`ttfCurrentCharStart`) when the fresh generation
  rebuilds — the same mechanism a Text-settings change uses. No position
  bookkeeping is invented inside the preview.
- **Unconfirmed captures.** When the relayout scan cannot confirm the anchor
  page (anchor beyond the page budget, layout failure), the capture is
  dropped and the preview keeps the frame it opened over rather than
  displaying a different page as if it were the current one.

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
- 2026-09-27 — (review round 1) The close contract compares the FINAL values
  with the ENTRY snapshots instead of a sticky changed flag, so a bounced
  change (A→B→A) closes silently with zero reflow.
- 2026-09-27 — (review round 1) The relayout scan resets the capture up
  front, and an unconfirmed scan (anchor beyond the budget, layout failure)
  drops the capture: the preview never displays a different page as if it
  were the current one. A live inline build session found at open is aborted
  (partial stays resumable) and the chapter cache reopened so the preview can
  re-lay at every change.
- 2026-09-27 — (review round 1) `ttfUiFallback.update()` runs AFTER the
  relayout (the reader's seam releases the borrowed faces BEFORE the family
  reload): update-before-reload would re-register faces against bytes the
  same render then frees (issue #168 ordering).

<!--
Verify with:
grep -c "Decision log" docs/design/2026-09-27-quick-font-preview.md
-->
