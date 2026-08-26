# Design: Chapter Time-Left in the Reader Status Bar

**Status:** Design / architecture plan — pending review
**Date:** 2026-08-25
**Depends on:** reading-stats SQLite integration (merged on `develop`; see `docs/design/reading-stats-sqlite-integration.md`)

## 1. Goal and summary

Show an estimated **time left in the current chapter** (e.g. `~12 min left`) in the
EPUB reader's status bar, derived from data that is **already in RAM during a
reading session**:

- per-book reading pace: `BookReadingStats.avgSecondsPerForwardPage` +
  `paceSampleCount` (`src/activities/reader/BookReadingStats.h:16-17`), loaded once
  in `EpubReaderActivity::onEnter()` (`src/activities/reader/EpubReaderActivity.cpp:185-193`)
  and kept in the `stats` member for the whole session;
- pages remaining in the chapter: `section->estimatedTotalPages() - section->currentPage - 1`,
  from the same `section` the status bar already reads
  (`EpubReaderActivity.cpp:1748-1752`).

**No schema change, no new DB access, no per-page work.** The estimate is pure
arithmetic on already-loaded members at render time.

## 2. User-facing behavior

### What is shown

- A short text element in the status bar's text lane: `~12 min left`,
  `~1h 5 min left`, or `< 1 min left`.
- The `~` prefix matches the existing convention for estimated values (the
  estimated page count marker in `BaseTheme::drawStatusBar()`,
  `src/components/themes/BaseTheme.cpp:905-924`).
- When the estimate cannot be produced (no pace data, tracking disabled, page
  count unknown — see §6), the element is **omitted entirely**. No `—`
  placeholder: status-bar space is scarce and an always-empty slot teaches
  nothing. (Open question Q1.)

### Where it appears

In the **left cluster** of `BaseTheme::drawStatusBar()`, after
battery → clock(if left) → bookmark, before the centered title. Rationale:

- The right cluster already carries the page counter + book % + `~` estimate
  marker (`BaseTheme.cpp:901-928`); adding a fourth numeric element there
  collides visually and with `STATUS_BAR_CLOCK_RIGHT`.
- The title layout already adapts to a variable-width left cluster:
  `titleMarginLeft = leftClusterWidth + 30` (`BaseTheme.cpp:1003-1004`), so the
  centered title re-centers automatically — no title-layout change needed.
- Auto-turn mode replaces the *title* (`EpubReaderActivity.cpp:1758-1761`) but
  not the left cluster, so the estimate stays visible during auto page turns.

### When it updates

The status bar has no partial refresh; it is redrawn with every full page render
(`EpubReaderActivity::renderContents()` → `renderStatusBar()`, called up to three
times per page: prewarm scan, image-placeholder pass, final BW pass —
`EpubReaderActivity.cpp:1549, 1577, 1583`). The estimate changes only when
`currentPage` or the pace average changes, i.e. per page turn — exactly the
existing refresh cadence. No timer, no extra display updates.

### Localization hook points

New keys in `lib/I18n/translations/english.yaml` (other languages fall back to
English until translated), then `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`:

- `STR_TIME_LEFT_MIN` — `"~%lu min left"`
- `STR_TIME_LEFT_HM` — `"~%luh %lu min left"`
- `STR_TIME_LEFT_LESS_THAN_MIN` — `"< 1 min left"`
- `STR_CHAPTER_TIME_LEFT` — `"Chapter Time Left"` (settings toggle label)

`%lu` placeholders follow the established stats-string pattern
(`STR_STATS_DURATION_MIN: "%lu min"`, `english.yaml:464`), consumed via
`snprintf(buf, len, tr(STR_...), ...)` as in
`BookReadingStats::formatDuration()` (`src/activities/reader/BookReadingStats.cpp:47-59`).
All strings are short and ASCII — no impact on the CJK font-prewarm scan that
`renderStatusBar()` participates in.

## 3. Data derivation

### Formula (recommended: book-level pace × pages remaining in chapter)

```
pagesRemaining  = max(0, section->estimatedTotalPages() - section->currentPage - 1)
paceSecPerPage  = stats.avgSecondsPerForwardPage        // if paceSampleCount >= MIN_BOOK_PACE_SAMPLES (proposed 10)
               ?: globalPaceSecPerPage                  // fallback, see below
               ?: none                                  // → hide element
timeLeftSeconds = paceSecPerPage * pagesRemaining
display         = format via STR_TIME_LEFT_* rules (§2)
```

**Fallback global pace (optional but recommended):** no global pace field exists,
but a rough one is derivable from already-loaded members:

```
globalPaceSecPerPage = globalStats.totalReadingSeconds / globalStats.totalPagesTurned
```

used only when `globalStats.totalPagesTurned >= 50` (proposed threshold). Both
fields are loaded into the `globalStats` member at `onEnter()`
(`EpubReaderActivity.cpp:189`). This gives a first-session-of-a-new-book estimate
instead of a blank. The book-level pace takes over after ~10 forward pages.

`avgSecondsPerForwardPage` is a weighted running average over forward-page dwell
times (2 s min, 600 s idle cap; `MIN_READING_PACE_SAMPLE_SECONDS`,
`READING_IDLE_THRESHOLD_SECONDS` — `EpubReaderActivity.cpp:57-61`), capped at
1000 samples (`BookReadingStats.cpp:9, 22-41`). It persists across sessions, so a
re-opened book shows an estimate from page 1.

### Computable today vs. needs new data

| Input | Available today? | Source |
|---|---|---|
| Book-level pace (`avgSecondsPerForwardPage`, `paceSampleCount`) | ✅ | RAM `stats` member, loaded `onEnter` |
| Pages remaining in chapter | ✅ | `section->estimatedTotalPages() - currentPage - 1` (exact once the section build finalizes; EMA estimate while building) |
| Rough global pace fallback | ✅ | derived from RAM `globalStats` member |
| Per-chapter pace | ❌ | would require schema v2 — **not recommended** (§5) |
| Book-level time-left for `estimatedTimeLeftSeconds` | ⚠️ partially | field/column exist but are never assigned; no book-wide page count exists (progress is byte-based, `Epub::calculateProgress`, `lib/Epub/Epub.cpp:909-919`) — see §7 |

### Why this is render-safe

The status-bar estimate reads only `stats` / `globalStats` (RAM members since
`onEnter`) and `section` (already dereferenced by the same function). It never
touches `ReadingStatsStore`. This satisfies the wear-leveling contract of the
merged design (D3, `reading-stats-sqlite-integration.md:68-82`): single writer,
DELETE journal, writes batched in `onExit()` (`EpubReaderActivity.cpp:216-221`),
**no DB I/O on the render path**. The estimator must stay a pure function over
its inputs so this invariant is structural, not conventional.

## 4. Required code touch-points

| File | Change |
|---|---|
| `src/activities/reader/ReadingStatsUtils.{h,cpp}` | New pure helper, e.g. `estimateChapterTimeLeftSeconds(const BookReadingStats&, const GlobalReadingStats&, uint16_t pagesRemaining) -> std::optional<uint32_t>`, plus a `formatChapterTimeLeft(uint32_t seconds, char* buf, size_t len)` using the new `STR_TIME_LEFT_*` keys. Host-unit-testable (mirrors `test/reading_stats/` shims). |
| `src/CrossPointSettings.h` | New field `uint8_t statusBarChapterTimeLeft = 0;` beside the other status-bar fields (`:203-216`); new `StatusBarSpec::showChapterTimeLeft` bool (`:352-364`); include it in `StatusBarSpec::textLaneVisible()` (`:370-373`) so the lane is reserved when it is the only element. |
| `src/CrossPointSettings.cpp` | Map the field in `statusBarSpec()` (`:235-250`); persist in `toJson`/`fromJson` (key `"statusBarChapterTimeLeft"`). |
| `src/components/themes/BaseTheme.h` / `.cpp` | Extend `drawStatusBar()` (`BaseTheme.h:270-273`, `BaseTheme.cpp:882`) with one parameter: `const char* chapterTimeLeft` (nullptr ⇒ skip). Render it in the left cluster with the existing `leftClusterWidth` accumulation, `SMALL_FONT_ID`, `snprintf`-produced caller buffer (no new heap, no `std::string` on the render path). |
| `src/activities/reader/EpubReaderActivity.cpp` | `renderStatusBar()` (`:1748-1779`): under `#ifdef READING_STATS_ENABLED`, if `sb.showChapterTimeLeft && SETTINGS.shouldTrackReadingStats() && section`, compute `pagesRemaining`, call the estimator, `snprintf` into a stack `char buf[24]`, pass to `drawStatusBar`. Pass `nullptr` otherwise. The function is `const`; all inputs are members — no signature change needed here. |
| `src/activities/reader/TxtReaderActivity.cpp` (`:333-339`), `src/activities/reader/XtcReaderActivity.cpp` (`:99-140`), `src/activities/settings/StatusBarSettingsActivity.cpp` (`:296`) | Update the three other `drawStatusBar` call sites for the new parameter: TXT/XTC pass `nullptr` (stats are EPUB-only today — no `READING_STATS_ENABLED` wiring exists in those readers); the settings preview passes a sample string (e.g. `~12 min left`) when the preview toggle is on, so the preview stays truthful. |
| `src/SettingsList.h` | `SettingInfo::Toggle(StrId::STR_CHAPTER_TIME_LEFT, &CrossPointSettings::statusBarChapterTimeLeft, "statusBarChapterTimeLeft", StrId::STR_CUSTOMISE_STATUS_BAR)` next to `statusBarChapterPageCount` (`:417-418`), wrapped in `#ifdef READING_STATS_ENABLED` like the tracking toggle (`:346-347`). |
| `lib/I18n/translations/*.yaml` | Add the four keys (English reference; others inherit). Regenerate `I18nKeys.h`/`I18nStrings.{h,cpp}` (gitignored, build-time). |
| `test/reading_stats/` | Extend the host suite: estimator unit tests (thresholds, fallback, clamping) — the existing shims (`test/reading_stats/shims/I18n.h`) already stub `tr()` for this style of code. |

## 5. Is new per-chapter stats storage needed? — No

**Recommendation: derive from the existing book-level pace; do not add per-chapter
tracking.**

- **Accuracy gain is marginal.** Pace varies somewhat by chapter content (dense
  tables vs. dialogue), but the dominant signal is the reader's own speed on this
  book, which the book-level weighted average already captures and refines
  continuously.
- **SD wear-leveling cost is real.** Per-chapter rows mean one upsert per chapter
  visited in every `onExit()` transaction (DELETE journal rewrites whole pages
  per commit), an unbounded row count per book, a `user_version` bump with a
  migration, and a larger `book_stats` surface — all to improve an estimate that
  is displayed with a `~` marker by design.
- **The merged design deliberately keeps writes minimal** (aggregate rows only,
  batch at exit; `reading-stats-sqlite-integration.md:196-199`). Per-chapter
  tracking cuts against that decision.

If per-chapter accuracy is ever wanted, the cheaper path is *not* persistence:
weight the in-RAM pace estimate by samples taken while `currentSpineIndex` equals
the rendered chapter. That is a possible follow-up, not part of this design.

## 6. Edge cases

| Case | Behavior |
|---|---|
| Tracking disabled (`SETTINGS.shouldTrackReadingStats() == false`) | Element hidden. `onEnter` skips loading stats, so `stats` is default-constructed (`avgSecondsPerForwardPage == 0`) and the store refuses to open (`ReadingStatsStore.cpp:64-67`) — the estimator naturally yields "unknown". The settings toggle is only compiled on `READING_STATS_ENABLED` targets and should also be hidden/disabled in the status-bar settings UI when tracking is off. |
| First read of a book (`paceSampleCount < 10`) | Fall back to the derived global pace if `globalStats.totalPagesTurned >= 50`; otherwise hide. |
| Chapter still building (`section->isBuilding()`) | `estimatedTotalPages()` is an EMA extrapolation — the same number the page counter already shows with `~` (`BaseTheme.cpp:906`). Show the time estimate anyway; the `~` prefix covers the compounded uncertainty. It self-corrects as the build progresses. |
| Short chapter / last page (`pagesRemaining == 0`) | `0` seconds ⇒ format as `< 1 min left` (or hide on the final page — reviewer choice, Q3). |
| Page count zero / no `section` | Hide (guard `section && pageCount > 0`). |
| Auto page turn | Estimate stays in the left cluster while the title lane shows the auto-turn banner. Note: auto-turn dwell times enter the pace average like any forward turn (dwells under 2 s are filtered by `MIN_READING_PACE_SAMPLE_SECONDS`), so heavy auto-turn use will bias the pace toward the turn rate. Acceptable; worth documenting in the PR. |
| Re-opened / previously read book | Pace persists across sessions (capped weighted average), so the estimate appears from page 1. |
| Backward navigation | Backward turns never sample pace (only forward turns do, `EpubReaderActivity.cpp:1082-1086`); the estimate simply reflects the new position. |
| TXT / XTC readers | Unchanged; they pass `nullptr`. The design does not block wiring them later. |

## 7. Decisions (locked by reviewer — Nidoros, 2026-08-25)

1. **Global fallback:** YES — when the book has too few own samples, fall back to
   the derived global pace (`globalStats.totalReadingSeconds / totalPagesTurned`,
   threshold `totalPagesTurned >= 50`). Book-level pace takes over once
   `paceSampleCount >= 10`.
2. **Toggle default:** ON (`statusBarChapterTimeLeft = 1`). Customizable like the
   other status-bar fields via `StatusBarSpec`.
3. **Unknown state:** OMIT the element (pass `nullptr` to `drawStatusBar`). No
   placeholder.
4. **`estimatedTimeLeftSeconds`:** WRITE the existing (currently-unassigned) field
   at book level in `onExit()` (persist via `store.saveBook`). Keep it in scope.
5. **Notation:** minutes-only numbers in the status bar — `~N min left` or
   `< 1 min left`. No `h/min` mixed form in the status bar (that form is fine in
   the stats screen only).
6. **Final page:** when `pagesRemaining == 0`, show `< 1 min left` (do not hide).

## 8. Open questions for the reviewer


1. **Unknown state:** omit the element (recommended) or show a placeholder like
   `~-- min`? Omitting keeps the lane clean but makes the feature less
   discoverable on first use.
2. **Fallback pace:** use the derived global pace (`totalReadingSeconds /
   totalPagesTurned`, threshold 50) for books without their own pace, or hide
   until 10 book-level samples exist? The global figure blends per-page dwell
   with session overhead and is rougher.
3. **Final page of a chapter:** show `< 1 min left` or hide at `pagesRemaining == 0`?
4. **Default of the new toggle:** on or off? `trackReadingStats` defaults to on
   (`CrossPointSettings.h:297`); defaulting this on too makes the feature
   visible without settings spelunking but adds status-bar text for everyone on
   PSRAM targets.
5. **Bonus scope:** `BookReadingStats.estimatedTimeLeftSeconds`
   (`BookReadingStats.h:19`) exists in the schema, is round-tripped, and is shown
   in `BookStatsActivity` (`BookStatsActivity.cpp:66`) but is **never assigned**.
   A book-level estimate (e.g. `totalReadingSeconds × (100 − progress%) /
   progress%`, or pace × estimated remaining pages via byte-ratio) could populate
   it in `onExit()`. In scope for this feature, or a separate PR?
6. **String length in narrow layouts:** `~1h 25 min left` plus battery + clock in
   the left cluster on an 800-wide landscape screen is fine; on a 480-wide
   portrait screen with all left-cluster elements on, the title truncation point
   moves right. Is trimming to `~85 min`-style (minutes only, no hours) preferred
   for the status bar, keeping the h/min form only in the stats screen?

## 8. Implementation plan skeleton (no code)

1. **Estimator + formatting helper** in `ReadingStatsUtils.{h,cpp}` (pure,
   `std::optional<uint32_t>`) with host unit tests in `test/reading_stats/`.
2. **Settings:** `statusBarChapterTimeLeft` field, `StatusBarSpec.showChapterTimeLeft`,
   `textLaneVisible()` update, JSON persistence, `SettingsList.h` toggle.
3. **I18n:** add the four keys to `english.yaml`, run `gen_i18n.py`.
4. **Theme API:** extend `BaseTheme::drawStatusBar()` with the `const char*`
   parameter and left-cluster rendering; update the TXT, XTC, and settings-preview
   call sites.
5. **Reader wiring:** `EpubReaderActivity::renderStatusBar()` computes and passes
   the string (stack buffer, `#ifdef READING_STATS_ENABLED`).
6. **Verify:** `./bin/clang-format-fix -g`; host tests; `pio run -e x4pro`
   (stats-enabled target) and `pio run -e default` (stats-disabled target) must
   both compile — the `#ifdef` boundaries are the risk. On device: enable the
   toggle, confirm `~N min left` appears after ~10 pages, disappears with
   tracking off, and that no DB write occurs until `onExit` (serial log around
   `ReadingStatsStore`).
7. **Docs:** one line in the user-facing feature list/changelog if the project
   keeps one; this file remains the design record.
