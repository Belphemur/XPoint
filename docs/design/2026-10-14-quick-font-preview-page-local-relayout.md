# Quick font preview — page-local relayout (ADR)

Date: 2026-10-14
Status: accepted, implementation pending (job `quickfont-page-local`)
Parent: `docs/design/2026-09-27-quick-font-preview.md` (amends its relayout
mechanism; close contract unchanged in spirit, tightened below)
Related issues: #137, #143, #168, #194, #195, #205

## Context

The quick font preview (`FontPreviewActivity`) shipped on develop re-lays the
page on every confirmed change through `quickRelayoutPage()`
(`src/activities/reader/QuickPageRelayout.cpp:61`): the engine lays the WHOLE
chapter from its start, the `QuickSink` scans forward page by page (budget
128 pages), and the last page at or before the entry anchor is deep-copied
and painted.

That mechanism is correct in its invariants but wrong in cost and in the
owner's contract for the feature:

- The owner contract (2026-09-28, restated 2026-10-14): the preview shows
  **the text of the currently displayed page only**, re-rendered at the
  candidate settings — "redoing the render of the current page". Not a
  chapter-anchored scan. The 2026-09-27 design doc said the same thing
  ("show only the current page's text — the already-captured
  `QuickPageCapture` buffer is the natural input") but the implementation
  grew a chapter scan instead of using the capture.
- Cost: every size step or family change re-lays up to 128 pages before the
  preview paints. On a long chapter the anchor can sit past the budget
  (`reachedAnchor=false`), the capture is dropped, and the preview keeps the
  stale frame (`FontPreviewActivity.cpp:73`, render keeps previous frame) —
  the "font change appears dead / wrong page" class of bug reports.
- A crash has been reported in this area (serial log pending; will be
  attached to the job when captured). Suspects to audit while implementing:
  `QuickPageCapture` record-array bounds vs the build profile actually
  compiled on x4pro (caps are hardcoded to STANDARD
  `QuickPageCapture.h:27-31`), `paintCapturedPage` painting a capture whose
  font chain was invalidated by a family reload mid-session, and
  `openFontPreview()`'s `abortSession()` + anchor `ttfCurrentCharStart` when
  the aborted inline build never committed a page.

## Decision

1. **Page-local preview.** The preview's render input is the reader's
   CURRENT page — the text the reader displays at open time, obtained from
   the page the reader already owns (committed chapter cache / the
   `QuickPageCapture` taken at entry), NOT a chapter rescan. On every
   confirmed change the preview re-lays ONLY that page's text span
   (`page.charStart .. page.charEnd`) at the candidate settings and paints
   it. Implementation seam: extend the engine's transient layout entry
   (`TtfBookRuntime::quickLayoutPage`, `TtfBookRuntime.h:107`, freeink-sdk
   side) with a start-offset/window mode that flows text beginning at an
   arbitrary char offset and stops at the first page break. If the engine
   cannot start mid-paragraph exactly, start at the paragraph boundary
   containing the page's first char — an approximate preview is acceptable;
   the close reindex computes the true pagination.
   `quickRelayoutPage()` keeps its signature and callers but loses the
   chapter scan: it delegates to the window mode with the captured page's
   own char range. The 128-page budget, `sawCandidate` logic, and
   `budgetStopped` paths die with the scan.
2. **Preview correctness stays subordinate to close.** The preview is a
   rendering of the entry page's text at candidate settings; its page index
   is advisory. The close contract (already shipped) is unchanged and
   remains the authority: net-effect comparison against entry snapshots
   (`FontPreviewActivity.cpp:216-236`); changed →
   `applyReaderTextSettings(pageIndex?)` → the settings-driven CLEAN reindex
   with position preserved (resolved-page seeding from #194/#195/#205
   stays); unchanged → silent pop, zero reflow, zero writes.
3. **Close shows the reindexed view with a forced FULL screen refresh.**
   After a changed close the reader triggers indexing (clean rebuild) and
   the first frame of the reindexed page MUST be a full (non-fast) panel
   refresh: `pagesUntilFullRefresh = 1` stays, and the ADR pins that the
   full-refresh forcing must survive every render path that can draw first
   (no FAST diff may land on the glass before it). Verified against
   `EpubReaderActivity.cpp:5631-5659`.
4. **While the preview is open, no indexing, no FIBP work, no committed-cache
   writes.** Already shipped (`onPreviewFontChanging`, host seam, transient
   pass); restated because the page-local change must not regress it. The
   reindex/backgroud worker starts on pop, per the owner's 2026-09-28
   directive.
5. **Crash fix rides the same change** but follows test-first discipline:
   reproduce on host tests where possible (the quick_page_capture suite is
   the pattern), file the finding with `file:line` evidence, fix, then gate.

## Non-goals

- No incremental cache patching on close (clean rebuild only, per parent
  doc).
- No freeink-sdk behavioral change beyond the transient window-layout entry
  (submodule pin moves only if the window mode requires an SDK API — if so,
  the pin bump is a separate commit with its own rationale).
- Legacy non-TTF builds untouched.

## Consequences

- A size step paints in O(one page layout) instead of O(up to 128 page
  layouts); the anchor-budget stale-frame class disappears for the preview.
- The previewed page may differ slightly in line breaks from what the close
  reindex settles on (approximate-preview rule, unchanged from the parent
  doc) — accepted.
- The engine window mode must respect the same per-page arena lifecycle the
  scan mode does; `QuickPageCapture` stays the deep-copy seam.
- Crash root cause: once the serial log lands, it is diagnosed against this
  design (the ADR names the suspects); if the crash lives outside this
  seam, it gets its own fix commit in the same job, evidence-first.
