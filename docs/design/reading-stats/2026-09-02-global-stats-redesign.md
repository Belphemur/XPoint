# Global Stats Screen — CrossInk Card-Grid Layout (Readable Heatmap + Bar Charts)

**Status:** Implemented in PR #54 (`fix/global-stats-crossink-redesign` → `develop`).
Merged: pending review on GitHub.
**Target repo:** `Belphemur/crosspoint-x-reader` (branch `develop`)
**Worktree:** `crosspoint-global-stats-redesign` (off `origin/develop@48178b4a`,
which already includes the merged PR #52)

## Context — what the user wants

PR #52 (merged) ported a partial CrossInk card-grid layout to CrossPoint.
The shipped result is two issues:

1. **"Global stats for global don't look like CrossInk at all, we need to
   have a clear nice card like CrossInk."** — The shipped global screen
   is still a `UiListActivity` (label + value rows) with charts tacked on.
2. **"Also the heatmap bar chart is far from being readable."** — 4×4
   cells with 1px gaps, no day labels, no week labels, no title, no
   today-marker. Bar chart bars are too short, no value labels, no axis.

The user clarified:

- **"Use the card display as per screenshot and crossink code normally."**
  — Just port crossink's `BookStatsView` rendering directly. Same card
  display, same layout, same exact look. Don't reinvent the scrollable
  widget system; copy the existing default/compact layout switching
  crossink already encodes in `getStatsLayout()`.

## Scope (corrected from the previous draft)

- **`BookStatsActivity` (per-book screen)**: replace its current rendering
  with crossink's `renderPerBookStatsPage(...)`. Shows the per-book top
  card (Sessions / Reading Time / Progress / Avg Session / Time Left /
  Reading Speed (WPM)), then the Started (N days) + Est. Finish row, then the
  two bar chart cards (Time of Day, Day of Week). No global stats
  shown on this screen — same as crossink.

- **`GlobalStatsActivity` (settings-menu global screen)**: replace its
  current rendering with crossink's `renderGlobalStatsPage(...)`. Shows
  the global card (Sessions / Reading Time / Reading Speed (WPM) / Avg Session /
  Reading Streak / Books Read) only.

- **No scrollable widget, no per-book-screen-shows-global** — that's
  the original v1 design that the user overrode. Each screen shows
  exactly one scope, exactly like crossink.

## Goal

1. Port **`src/activities/reader/BookStatsView.h/.cpp`** from crossink's
   `main` branch into CrossPoint. Strip out crossink's touch-wiring
   helpers (deferred per the last PR) and the `HalClock` / `FreeInkUICore`
   deps that don't exist in CrossPoint; keep the rendering layout
   primitives, the StatsLayout system (default + compact), the
   `drawStatCell` / `drawSectionCard` / `drawHorizontalBars` / time-bucket
   derivations, and the `renderPerBookStatsPage` / `renderGlobalStatsPage`
   / `renderNoRtcCombinedStatsPage` entry points.
2. Replace `BookStatsActivity::render(...)` to call
   `BookStatsView::renderPerBookStatsPage(...)` and remove the
   `UiListActivity` / row-list plumbing it currently has (keep the
   row-list state for backward compat with `setResult(ClearPaceResult{})`,
   but no longer draw the list).
3. Replace `GlobalStatsActivity::render(...)` to call
   `BookStatsView::renderGlobalStatsPage(...)` and drop the
   `UiListActivity` inheritance, becoming a plain `Activity` with one
   `render(RenderLock&&)` override (mirroring `BookStatsActivity`'s shape).
4. Keep the `ClearPaceResult` plumbing in both activities (the per-book
   screen already uses it via `setResult`; the global screen clears
   in place and continues). The "Clear reading speed" action in both
   screens stays accessible — **the per-book screen** gets it from a
   single back button + `setResult(ClearPaceResult{})` (existing), the
   **global screen** gets it from a list-row interaction. (No "button"
   change for now — the per-book button-hint footer is unchanged, and
   the global screen's clear-pace row stays as today.)

## Cell grids (mirror crossink exactly)

**Per-book top card** (3×2 grid; 2 rows without RTC, 3 rows with RTC):
```
┌──────────────────┬──────────────────┬──────────────────┐
│ Sessions         │ Reading Time      │ Progress         │
│ 42               │ 18h 25m           │ 14%              │
├──────────────────┼──────────────────┼──────────────────┤
│ Avg Session      │ Time Left         │ Reading Speed (WPM) │
│ 26m              │ ~1h 15m           │ 0.4              │
└──────────────────┴──────────────────┴──────────────────┘
```
With RTC, a third row appears (Started + Est. Finish, each spanning half
the card width):
```
│ Started Mar 4, 2025 (38 days)        │ Est. Finish Apr 12, 2025         │
```

**Global card** (3×2 grid; 2 rows):
```
┌──────────────────┬──────────────────┬──────────────────┐
│ Sessions         │ Reading Time      │ Reading Speed (WPM) │
│ 312              │ 102h 15m          │ 0.6              │
├──────────────────┼──────────────────┼──────────────────┤
│ Avg Session      │ Reading Streak    │ Books Read       │
│ 20m              │ 12 days            │ 8                │
└──────────────────┴──────────────────┴──────────────────┘
```

## Design decisions

### 6.1 File layout

- **NEW** `src/activities/reader/BookStatsView.h` — public surface:
  `renderPerBookStatsPage`, `renderGlobalStatsPage`,
  `renderNoRtcCombinedStatsPage`, plus `renderEditBookDatesPage` (the
  per-book date-edit flow; out-of-scope for the first cut but the
  signature mirrors crossink so the activity can wire it later).
- **NEW** `src/activities/reader/BookStatsView.cpp` — port of crossink's
  `BookStatsView.cpp` minus touch-wiring / crossink-only helpers.
- **MODIFIED** `src/activities/reader/BookStatsActivity.h/.cpp` — drop
  the list-based render; replace `render(...)` with a call to
  `BookStatsView::renderPerBookStatsPage(...)`. Keep the
  `setResult(ClearPaceResult{})` plumbing for the back button to use.
  Keep the read-only data fields (title, stats, progressPercent, etc.).
- **MODIFIED** `src/activities/settings/GlobalStatsActivity.h/.cpp` —
  drops `UiListActivity` inheritance, becomes a plain `Activity` with
  a single `render(RenderLock&&)` override that calls
  `BookStatsView::renderGlobalStatsPage(...)`. Keeps the
  `globalStats.clearWpmStats()` action on `Confirm` for the existing
  clear-pace UX.
- **UNCHANGED** `src/components/themes/BaseTheme.h/.cpp` — the
  card-grid rendering is now in `BookStatsView`, not the theme layer.
  The previous chart helpers (`BaseTheme::drawBarChart`,
  `BaseTheme::drawBarChartRow`, `BaseTheme::drawBarChartTitle`,
  `BaseTheme::drawHeatmapGrid`) are removed in this PR because no
  caller remains after the rewrite (crossink's `drawHorizontalBars`
  in `BookStatsView.cpp` doesn't reuse them — crossink doesn't
  either).

### 6.2 Layout system (port of crossink's `getStatsLayout`)

CrossInk's `StatsLayout` struct + `kDefaultLayout` / `kCompactLayout`
constants are ported verbatim. The layout picks default vs compact based
on available height (`getStatsLayout` / `getNoRtcCombinedLayout`). The
home screen is 800×480 with a topPadding + button-hints overhead, so
the default layout fits in landscape; portrait or short screens fall
back to compact.

The `drawStatCell` / `drawSectionCard` / `drawHorizontalBars` helpers
also come from crossink and live in `BookStatsView.cpp` (not in the
theme — crossink doesn't put them in the theme either; they're stats-
page-specific). They're rendered with the renderer's
`drawText` / `drawRect` / `fillRect` primitives directly.

### 6.3 What gets removed

- `BaseTheme::drawBarChartTitle` — no longer used (crossink doesn't
  have this helper; titles are drawn by `drawSectionCard`).
- `BaseTheme::drawBarChart` / `drawBarChartRow` — kept but unused by the
  new BookStatsView path. Will be removed in a follow-up cleanup PR if
  nothing else uses them.
- `BaseTheme::drawHeatmapGrid` — same, kept for now.
- `GlobalStatsActivity::rebuildRowItems` / `valueCache` / `rowItems` /
  `clearPaceRow` / `buildScreen` / `formatStreak` / `formatStatsDate` —
  all removed. The clear-pace action moves to the existing
  `setResult(ClearPaceResult{})` pattern OR a confirm button on
  Confirm-press; for now, keep the existing in-place
  `clearPaceRow` index + `activateIndex` behavior since crossink
  also has a per-screen "Clear Pace" action.
- `GlobalStatsActivity::drawCharts` / `chartBandHeight` / `chartsVisible` /
  `chartBandRect` — all removed (the chart rendering now lives in
  `BookStatsView`).

### 6.4 What gets added

- `STR_STATS_PAGES_PER_MIN: "Pages/Min"` (crossink uses this in its
  per-book top card, but we deliberately DON'T — see "Why WPM
  replaces Pages/Min" below). The key is still added to the
  `I18nKeys.h` for completeness, but is not referenced by
  `BookStatsView`.
- `STR_STATS_NO_RTC_BANNER: "RTC clock not available"` (new i18n key —
  for the no-RTC combined-stats page, used when both per-book and
  global are shown together without an RTC).
- The existing `STR_STATS_*` keys we already added in PR #52 are
  reused; no further additions needed.

### 6.5 Why WPM replaces Pages/Min in both card grids

The original crossink-derived design had the rightmost cell of both
cards as `Pages/Min` (the average pages-per-minute rate, computed
from `pagesPerMinute(totalPagesTurned, totalReadingSeconds)`). After
ship-review the user asked for a change: **`Pages/Min` is hard to
interpret at a glance** (it conflates book-length and reading speed),
and the primary reading-speed metric users care about is **WPM** (the
trimmed-mean words-per-minute from the existing 15-sample window).

The PR #54 final commit (`994a022a`) replaces the `Pages/Min` cell
with a **Reading Speed (WPM)** cell in **both** the per-book and the
global card. Same display rule in both: `"<n> WPM"` via
`STR_STATS_WPM_VALUE` when `wpm.count > 0`, `"-"` via
`STR_STATS_WPM_UNAVAILABLE` otherwise — same display rule the
`BookStatsActivity` and `GlobalStatsActivity` already used in the
row-list rendering. The `pagesPerMinute()` helper is removed
(no callers after the rewrite).

Rationale for keeping `STR_STATS_PAGES_PER_MIN` in the i18n catalog
even though it is unreferenced: future stats screens (a per-book
"Speed History" graph, or the crossink no-RTC combined page) may
want it, and removing it would force a `gen_i18n.py` re-run + shim
edit for no current benefit. The shim's `case StrId::STR_STATS_PAGES_PER_MIN`
return is harmless dead code until a future caller reuses it.

## Constraints

- **No new platform build flags.** Same `pio run -e default` must
  succeed.
- **RAM budget unchanged.** All rendering is `constexpr` + `char[]`
  buffers; no `std::vector` or `std::string` in the render path. (CrossInk
  has `std::string` in some helpers; replace with `const char*` in our
  port to match CrossPoint's existing convention.)
- **No version bump** on `BookReadingStats` or `GlobalReadingStats`
  (v6 / v4 unchanged). The redesign is purely a rendering change.
- **Co-author trailer** preserved on the commit
  (`Co-authored-by: Julia Nguyen <julia@uxj.io>`).
- **One single commit**, branch `fix/global-stats-crossink-redesign`.
  No push until user signs off.

## Verify (must pass before commit)

```bash
./bin/clang-format-fix -g

# Host tests still pass (no changes to test code expected, but verify)
cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release
cmake --build build/test -j$(nproc) --target ReadingStatsStoreTest
./build/test/reading_stats/ReadingStatsStoreTest   # expect 42/42

# Required: firmware build covers the new BookStatsView + the
# BookStatsActivity / GlobalStatsActivity changes. PRs that touch
# src/ are gated by the `Build default` / `Build x4pro` / etc. jobs in
# .github/workflows/ci.yml — those are the source of truth. Do not
# mark the PR ready while any board build is failing.
```

## Out of scope (deferred)

- Heatmap rendering — the no-RTC combined page (where heatmap would
  make sense) isn't reachable on the per-book screen when an RTC is
  present (per crossink's `renderPerBookStatsPage` behavior). The
  user explicitly mentioned "the heatmap bar chart is far from being
  readable" but on second look at the screenshot, the user wants the
  bar chart readability fixed, not necessarily the heatmap. Crossink
  doesn't render a heatmap on the per-book screen either — heatmap is
  only on the no-RTC combined stats page if at all. We'll add a
  heatmap card to the global screen IF `renderGlobalStatsPage` from
  crossink has one; if not, we skip. (CrossInk's `renderGlobalStatsPage`
  does NOT include a heatmap — only the time-of-day + day-of-week bar
  charts. The heatmap lives in `renderNoRtcCombinedStatsPage` only,
  which is reachable when the RTC is absent. CrossInk treats RTC
  absence as the trigger to show the combined view, which is
  backwards from "user wants a heatmap on the global screen".)
- Touch wiring (deferred per PR #52).
- Translators add the new key in their own YAMLs later; we only
  ship the English baseline.

## Implementation note

The port of `BookStatsView.cpp` from crossink requires removing
crossink-specific dependencies that don't exist in CrossPoint:

- `FreeInkUICore` / `FreeInkApp` — only used in crossink for
  `formatCompactReadingDuration` and `TouchRegistry`. In CrossPoint
  we already have `formatCompactReadingDuration` in `ReadingStatsUtils`.
  Drop the FreeInkUI include entirely.
- `CompactHeader` / `TouchHeaderBackButton` — crossink uses these for
  the "Reading Stats" title. CrossPoint's `BookStatsActivity` draws
  the title directly with `renderer.drawText`; keep that.
- `HalClock` — only `halClock.isAvailable()` and the title bar check.
  In CrossPoint, the RTC gate is `getCurrentLocalReadingStatsDateTime`
  in `ReadingStatsUtils.cpp`. Replace.
- `TouchRegistry` / `BookStatsTouchTarget` — touch wiring, deferred.
- `I18N.get(labels[i])` — crossink's typed i18n getter. CrossPoint
  uses `tr(STR_*)` (the same underlying function, different name).

After stripping those, the file is ~500 lines of pure rendering code
that ports directly.
