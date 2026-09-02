# Home Card Progress + CrossInk Reading-Stats Display Port

**Status:** Implemented in PR #52 (`feat/home-card-progress` → `develop`).
Merged: pending review on GitHub.
**Target repo:** `Belphemur/crosspoint-x-reader` (branch `develop`)
**Commit on this branch:** `4b7efe09` — "feat(reading-stats): home card progress line + crossink stats card-grid port"
**Source of inspiration:** `uxjulia/crossink` (`development`) — the sister fork next to
this workspace (`../crossink/`). CrossInk's `BookStatsView` (around her
v1.4–v1.5 cycle) introduced the card-grid stats layout and the "Est. Finish" /
"Started N days" / "Reading Streak" derivations. We are porting the display shape and
the labelling; the persistence layer stays CrossPoint's existing v6 binary file (see
`2026-08-25-binary-files.md`).
**Co-author:** Julia Nguyen `<julia@uxj.io>` — credit on every commit in this PR
(see §10 for why and how).

---

## 1. Goal & scope

Two changes land together in one PR — they share the same display helpers and
the same `lastBookProgressPercent` snapshot on the reader exit path.

### 1.1 Home-screen recent-book card — show progress % + time left

The home screen already renders a recent-book cover on top of the menu
(`HomeActivity` → `GUI.drawRecentBookCover` → one of the four themes). Today the
themes show only the title and author. The user has asked for the kind of line
visible in the attached screenshot:

> **A Nefarious Engagement**
> Lynn Messina
> Beatrice Hyde-Clare Mysteries #4.0
> **14% • ~1h 15m**

That is the screenshot of the **stats screen**, not the home card; but the same
display idea ("a compact one-liner of percent + ETA") belongs on the home card
under the title/author block.

We already compute the book-wide percent (`EpubReaderActivity::onExit`, lines
234–241 of `src/activities/reader/EpubReaderActivity.cpp`) and the estimated
time-left (`estimateBookTimeLeftSeconds(...)`, line 238) when a reading session
exits. Today the percent is thrown away and only `estimatedTimeLeftSeconds` is
written to the per-book stats record. This design adds:

- **Snapshot the percent** to the per-book stats record at session commit so the
  home screen does not have to re-open the EPUB (which costs spine parse + opf
  parse + font metrics on a 380 KB-RAM C3 — too expensive for a render path).
- **Render the line** in all four themes under the title/author block.
- **Load the per-book stats** in `HomeActivity::loadRecentBooks()` so the line is
  precomputed once and cached; the cover buffer + stats load are both
  background work on `HomeActivity::loop()`'s idle tick.

### 1.2 Reading-stats screen — adopt CrossInk's card-grid layout

CrossInk's `BookStatsView.cpp` (in `../crossink/src/activities/reader/`) renders
per-book and global stats as **horizontally-gridded "stat cells" inside titled
cards**, with rows of three cells (one row for per-book, one row for global) and
extra rows for date-of-start, est. finish, and time-of-day/day-of-week bar
charts. The visible screenshot shows the resulting layout.

We port the display shape — the cell grid, the labels, the derived metrics
(streak, days-reading, est. finish) — onto CrossPoint's existing
`BookStatsActivity` and `GlobalStatsActivity`. The data we need is **already
persisted** in CrossPoint's v6 binary file (`BookReadingStats` +
`GlobalReadingStats`); only two derived metrics are missing today and must be
added to `GlobalReadingStats`:

- `currentReadingStreak()` (days)
- `longestReadingStreak()` (days, derived from the existing
  `readingHistory` bit array — `computeReadingHistoryLongestStreak()` already
  exists in `ReadingStatsUtils.cpp`)

CrossPoint's existing `bookReadingHistoryBits` plumbing already supports this
(no new bytes on disk — `GlobalReadingStats` is at v3→v4 which is one byte shy
of capacity for the extra metadata, but the streak counters can live in
RAM-only state initialized from the history at load; see §3.3).

---

## 2. Non-goals

- **No** redesign of the home card cover layout. Cover, title, author,
  position are unchanged. The new line is added below the existing author
  block.
- **No** new persistence fields beyond what is strictly required:
  - One new byte on the per-book record (`lastBookProgressPercent`, see §3.2).
  - Zero new bytes on the global record — streak counters are RAM-only.
- **No** touch of the WPM algorithm, the pace sampler, the binary store, or
  the persistence-versioning rules from `2026-08-25-binary-files.md`.
- **No** re-doing the WPM-floored display in `STR_STATS_WPM_VALUE`. The existing
  percent format strings from crossink (`"%d%%"`) replace CrossPoint's
  `STR_PROGRESS_LBL` value formatter.

---

## 3. The persistence change: `lastBookProgressPercent`

### 3.1 Why a new field and not a derived calculation

The percent requires opening the EPUB to compute `Epub::calculateProgress()`,
which reads the cumulative spine item sizes (touching `<cachePath>/book.bin`)
and renders at least one page to count words. On the ESP32-C3 home screen this
is too expensive. Caching the percent on session exit is one write of one byte
— the only sane choice.

### 3.2 Where it lives in the binary layout

The v6 record (109 B) at `<cachePath>/stats_v6.bin` already reserves byte 108
as `reserved (0)` (see `BookReadingStats.cpp` lines 16–35 layout comment). We
**activate that byte** as `lastBookProgressPercent`:

```text
v6 (109 bytes):
  [73-74]  wpm.avg                   uint16_t LE, trimmed mean WPM (0 = none)
  [75-76]  wpm.count                 uint16_t LE, samples in window (0-15)
  [77-106] wpm.samples[15]           uint16_t LE each
  [107]    wpm.pos                   uint8_t
  [108]    lastBookProgressPercent   uint8_t, 0-100 (sentinel 0xFF = unknown)
```

This is **not** a binary-format break and therefore does **not** require a
v6→v7 bump:

- Records written by older builds always have byte 108 = 0 (the `memset(0)`
  in `save()` covers it). When we read those back, byte 108 = 0 is a legal
  percent value (0% = "opened, no pages turned yet"), so the home card will
  show "0% • ~1h 15m" rather than suppressing the line. That's acceptable:
  showing the actual truth of "0% read so far" is not a regression.
- The sentinel `0xFF = "unknown"` is reserved and means "the reader has
  never persisted a percent for this book" (e.g. a book added to recents
  but never opened, or the home card loaded an old record before any
  reader session committed). When the loader sees `>100`, it falls back to
  the unknown sentinel — `readWpmWindow()` already does this defensive
  clamp for free.

`BookReadingStats.h` gets one new field:

```cpp
uint8_t lastBookProgressPercent = UNKNOWN_BOOK_PROGRESS_PERCENT;
// ...
constexpr uint8_t UNKNOWN_BOOK_PROGRESS_PERCENT = 0xFF;
```

`BookReadingStats.cpp`:

- `readWpmWindow(...)` adds `uint8_t& lastBookProgressPercent` as an out-param
  and clamps the byte (`raw <= 100 ? raw : UNKNOWN`).
- `save()` writes `data[108] = lastBookProgressPercent`.
- `clearWpmStats()` is **unchanged** — clearing the WPM window is unrelated to
  the user's reading progress. Comment in the existing code already says
  "Zeros the WPM window only. Sessions, totals, dates, and bucket history are
  kept." The new field belongs to that same family.

### 3.3 Why no new fields on the global record

CrossInk's `BookStatsView` global card displays **Reading Streak** and **Books
Read**. CrossPoint already has `bookStatsCompletionCount()` on
`GlobalReadingStats` (or the equivalent — needs confirmation in §5 review).
The streak is derivable in O(1) from the existing `readingHistory` bit
array plus the current date — `computeReadingHistoryCurrentStreak()` already
exists in `ReadingStatsUtils.cpp`. We add two RAM-only convenience methods:

```cpp
uint16_t currentReadingStreakDays(const ReadingStatsDate* today = nullptr);
uint16_t longestReadingStreakDays();
```

These do not touch the binary record — they are recomputed from
`readingHistory` on every call. The cost is at most 91 byte inspections
(`READING_HISTORY_BYTES` × 8 bits). No new fields, no version bump.

---

## 4. Reader-side: persist percent on session exit

`EpubReaderActivity::onExit()` (`src/activities/reader/EpubReaderActivity.cpp`,
around lines 223–243) already computes `bookProgress` from
`Epub::calculateProgress(currentSpineIndex, sectionProg)`. Today the percent
is used only for the menu activity; the per-book stats record gets only the
estimated-time-left value. We add three lines after the existing
`chapterPages > 0 && bookProgress in [0,1]` guard:

```cpp
const int percent = clampPercent(static_cast<int>(bookProgress * 100.0f + 0.5f));
stats.lastBookProgressPercent = static_cast<uint8_t>(percent);
```

This fires on **every** session exit (including "open-then-quit-without-reading"),
so the percent is always up-to-date. The percent is gated on the same validity
checks as `estimatedTimeLeftSeconds`, which means a torn or invalid book does
not poison the snapshot.

---

## 5. Home-side: cache stats per recent book

`HomeActivity::loadRecentBooks()` (around line 36) already iterates the recent
books and fills `recentBooks`. We add a parallel
`std::vector<std::string> recentBookProgressLines;` (or a small struct) populated
by a new `loadRecentBookStats()` step. For each recent book:

1. Compute `<cachePath>` from the book's path. CrossInk and CrossPoint both
   store books at `<SD root>/<bookPath>` and the cache dir is
   `/.crosspoint/cache_<hash>` indexed by the source path. The existing
   `RecentBook.coverBmpPath` already encodes the cache dir basename; we
   reuse the same pattern via `Epub::getCachePath()` if the file is an EPUB,
   or skip for XTC/TXT (XTC/TXT readers do not currently expose a stats API
   in CrossPoint — see §5.2).
2. `BookReadingStats::load(cachePath)`.
3. If `lastBookProgressPercent == UNKNOWN`, use the `"-"` sentinel line;
   themes treat that sentinel as "no line" (see §8).
4. Else format `"%u%% • %s"` via the new i18n key `STR_PROGRESS_LINE_FMT`
   (§6), using `formatCompactReadingDuration(estimatedTimeLeftSeconds)`
   for the second half.

### 5.1 Cost

- One `BookReadingStats::load()` per recent book: 109 B read + 73 B read +
  validation. At 8–10 recent books (the `MAX_RECENT_BOOKS` cap), this is
  16–20 small file opens. Performed on `HomeActivity::loop()`'s idle tick
  (same place `loadRecentCovers` runs today) — the cover-loading popup is
  already reused as a progress indicator.
- A precomputed `std::string` per book: ~24 bytes heap each, 240 B total.
  Inside the `recentBookProgressLines` vector. Acceptable.

### 5.2 XTC and TXT

`XtcReaderActivity` and `TxtReaderActivity` do not currently persist a
`BookReadingStats` record. The home card line will simply be empty for those
formats — no regression, and the per-book stats screen is already EPUB-only.
This matches crossink's behavior (her stats also cover EPUBs only). No
additional work for XTC/TXT in this PR.

---

## 6. The display line

A single new i18n key:

```yaml
STR_PROGRESS_LINE_FMT: "%u%% • %s"
```

`%%` is the printf escape for a literal percent sign. The renderer fills in
the percent (0–100) and the preformatted compact time-left
(`formatCompactReadingDuration`, already exists in `ReadingStatsUtils.cpp` and
produces "1h 15m" / "<1m" / "20m" / "2h" — exactly the user's screenshot).

The bullet character is `•` (U+2022), matching the screenshot.

### 6.1 Theme rendering

We add the line under the title/author block in **all four themes**:

- `BaseTheme::drawRecentBookCover` (the canonical cover-only layout) — add
  one centred text row beneath the existing author.
- `LyraTheme::drawRecentBookCover` — add one row beneath the existing
  author in the right-hand text column.
- `Lyra3CoversTheme::drawRecentBookCover` — this is the three-cover row;
  the percent line goes under each title.
- `RoundedRaffTheme::drawRecentBookCover` — same pattern: one row under
  the centred title.

All four delegate to the same helper:

```cpp
void renderProgressLine(GfxRenderer& renderer, int x, int y, int width,
                        const char* line, bool inverted);
```

defined on `BaseTheme`. The formatting happens once upstream in the
activity (`HomeActivity`): it builds the final line with
`snprintf(buf, sizeof(buf), tr(STR_PROGRESS_LINE_FMT), percent, compactTime)`
so each theme just consumes a `const char*`. Unknown progress is represented
by the `"-"` sentinel from §5; themes compare against it and skip the line
entirely, so the card simply shows no progress row. The helper:

- Renders inverted or not based on the surrounding card state (selected vs.
  unselected).

The activity code (`HomeActivity`) does the formatting once during
`loadRecentBooks` so each theme just consumes a `const char*`.

---

## 7. The reading-stats screen (the screenshot the user pasted)

CrossInk's screenshot is the `BookStatsView` rendered inside her app. We
port the **display shape** onto CrossPoint's existing
`BookStatsActivity::rebuildRowItems` and `GlobalStatsActivity::rebuildRowItems`.
The list-based activity structure stays; the per-row formatting gains
CrossInk's "compact cell" pattern within each row.

### 7.1 Per-book (`BookStatsActivity`)

Re-arrange rows into three themed groups, matching the CrossInk top card:

```
[Header bar: book title]

┌──────────────────────────────────────────────┐
│ Sessions       Reading Time        Progress  │
│ 42             18h 25m             14%       │
│ Avg Session    Time Left           Pages/Min │
│ 26m            ~1h 15m             0.4       │
│ Started Mar 4, 2025 (38 days)                │
│ Est. Finish Apr 12, 2025                     │
└──────────────────────────────────────────────┘

[Time of Day bar chart — only if RTC available]
[Day of Week bar chart — only if RTC available]
```

(Rows 3 of the crossink screenshot are exactly the crossink view above.)

When the RTC is unavailable (X4 Classic without battery, some C3 builds),
the bottom two rows collapse to a 2-row layout — same as CrossInk's
`shouldShowRtcBasedStats()` gate.

### 7.2 Global (`GlobalStatsActivity`)

Mirror the per-book shape:

```
[Header bar: "All Books (This Device)"]

┌──────────────────────────────────────────────┐
│ Sessions       Reading Time        Pages/Min │
│ 312            102h 15m            0.6       │
│ Avg Session    Reading Streak      Books Read│
│ 20m            12 days             8         │
└──────────────────────────────────────────────┘
```

### 7.3 Time-of-day and day-of-week bar charts

CrossInk has bar charts for time-of-day (morning / afternoon / evening / night)
and day-of-week (Mon–Sun) inside the stats screen. CrossPoint's
`GlobalReadingStats` already persists the bucket seconds (`timeOfDaySeconds[4]`
and `dayOfWeekSeconds[7]`), and `BookReadingStats` also has both. We **add** a
small `drawBarChartRow()` helper to `BaseTheme` that renders
`label + bar + duration` for each bucket, sized to fit a 100×22 bar within
the available width. This is the only rendering primitive added — `BaseTheme`
exposes:

```cpp
void drawBarChart(GfxRenderer& renderer, Rect rect, const char* const* labels,
                  const uint32_t* values, int count);
```

### 7.4 Streak

`computeReadingHistoryCurrentStreak()` and
`computeReadingHistoryLongestStreak()` already exist in `ReadingStatsUtils.cpp`
(they were added for the upstream crossink-style history view). We add
`GlobalReadingStats::currentReadingStreakDays()` and
`GlobalReadingStats::longestReadingStreakDays()` as thin wrappers that
take the optional `today` date pointer (existing convention).

---

## 8. i18n keys

Add to `lib/I18n/translations/english.yaml`:

```yaml
STR_PROGRESS_LINE_FMT: "%u%% • %s"        # new, used on the home card
STR_STATS_PROGRESS_LBL: "Progress"        # already exists in crossink; add to us
STR_STATS_DAY: "day"
STR_STATS_DAYS: "days"
STR_STATS_NO_STREAK: "No streak yet"
STR_STATS_NEW_READER: "New Reader"
STR_STATS_MORNING_READER: "Morning Reader"
STR_STATS_AFTERNOON_READER: "Afternoon Reader"
STR_STATS_EVENING_READER: "Evening Reader"
STR_STATS_NIGHT_READER: "Night Reader"
STR_STATS_DAY_STREAK_FORMAT: "%u day streak"
STR_STATS_LONGEST_STREAK_LBL: "Longest Streak"
STR_STATS_DAILY_AVG_LBL: "Daily Avg"
STR_STATS_TIME_OF_DAY: "Time of Day"
STR_STATS_DAY_OF_WEEK: "Day of Week"
STR_STATS_EST_FINISH_DATE: "Est. Finish Date"
STR_STATS_FINISHED_DATE: "Finished Date"
STR_STATS_THIS_BOOK: "This Book"
STR_STATS_ALL_TIME: "All Books"
STR_TIME_LEFT: "Time Left"
```

These mirror crossink's keys verbatim where possible. Translators add the
equivalent in their YAMLs; a follow-up PR per language is fine. We
**do not** translate the bullet glyph (`•`) — it is not language-specific.

### 8.1 The printF format pitfall

Per `crosspoint-reader-dev` skill rule: format strings must use `%u` / `%s`,
NOT `{0}` / `{1}`. `STR_PROGRESS_LINE_FMT` MUST be `"%u%% • %s"`. The
verification step (grep `lib/I18n/I18nStrings.cpp` for the literal) is
mandatory before the PR opens.

---

## 9. Tests

`test/reading_stats/ReadingStatsBinaryStoreTest.cpp` gains one round-trip
test for `lastBookProgressPercent`:

```cpp
TEST(BookReadingStatsV6, ProgressPercentRoundTrip) {
  BookReadingStats stats;
  stats.lastBookProgressPercent = 42;
  stats.save(BOOK_DIR);
  const auto loaded = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(loaded.lastBookProgressPercent, 42);
}
```

Plus a sentinel-clamp test:

```cpp
TEST(BookReadingStatsV6, ProgressPercentUnknownClampedFromTornByte) {
  uint8_t data[STATS_FILE_SIZE] = {};
  data[0] = 6; // version
  data[108] = 200; // out-of-range
  Storage.writeFile(BOOK_DIR "/stats_v6.bin", data, STATS_FILE_SIZE);
  const auto loaded = BookReadingStats::load(BOOK_DIR);
  EXPECT_EQ(loaded.lastBookProgressPercent, UNKNOWN_BOOK_PROGRESS_PERCENT);
}
```

`test/reading_stats/ReadingStatsUtilsTest.cpp` gains the formatter test
(modelled on crossink's `BookStatsViewTest` if she has one — otherwise just
the canonical input/output matrix):

```cpp
TEST(ProgressLineFormat, Produces14PercentPlus1h15m) {
  char buf[40];
  BookReadingStats stats;
  stats.lastBookProgressPercent = 14;
  stats.estimatedTimeLeftSeconds = 75 * 60; // 1h 15m
  formatHomeProgressLine(stats, buf, sizeof(buf));
  EXPECT_STREQ(buf, "14% • 1h 15m");
}
```

Plus tests for the new `GlobalReadingStats` streak accessors.

`ReadingStatsUtilsTest.cpp` already exercises
`computeReadingHistoryCurrentStreak` and
`computeReadingHistoryLongestStreak` (added during the crossink port). We
verify those still pass; if any are missing, we add the missing two.

The total test count rises from 37 to **41** (or whatever the actual delta
ends up being — there are likely 2–3 streak tests to add).

---

## 10. Co-author credit

CrossInk's `BookStatsView` is the source of the card-grid layout, the
"Est. Finish Date" derivation, and the time-of-day / day-of-week bar chart
shape. Her work landed in `uxjulia/crossink` under her authorship
(`Julia Nguyen <julia@uxj.io>`, the only commit author on `BookStatsView.*`
and the stats i18n keys).

Per the project's standing convention (and the user-confirmed 2026-09-02
directive), **every commit in this PR carries**:

```gited
Co-authored-by: Julia Nguyen <julia@uxj.io>
```

This is a `git commit` trailer — GitHub renders it as a co-author credit
on the merge commit and in the contributors graph. We add it via
`git -c trailer="Co-authored-by: Julia Nguyen <julia@uxj.io>"` or by
appending it to the message before committing. Identity is **NOT** set
on the commit (we keep CrossPoint's `Antoine Aflalo <197810+Belphemur@users.noreply.github.com>`
as the author); only the trailer is added.

Per the `crosspoint-reader-dev` skill's git-identity rule: do not override
the commit author with `git -c user.name=...` for these commits. The trailer
is independent of the author identity.

---

## 11. Verification checklist (pre-PR)

- [ ] `cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release && cmake --build build/test -j`
      passes 41/41 (was 37/37 baseline).
- [ ] `pio run -e simulator` (if present) or `pio run -e default` builds clean
      (RAM ≤ 28%, Flash ≤ 85%).
- [ ] `pio check -e default --fail-on-defect low --fail-on-defect medium`
      clean.
- [ ] `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/` and grep
      `I18nStrings.cpp` for `"%u%% • %s"` literal — confirms the printf
      format survived the generator.
- [ ] Manual: open an EPUB, read past page 1, quit, return to home — the
      card shows `N% • ~Xh Ym` with N matching the menu's progress bar.
- [ ] Manual: open `BookStatsActivity` and `GlobalStatsActivity` — confirm
      the three-cell rows match crossink's layout.
- [ ] All review threads (Copilot, CodeRabbit, or human) answered in-thread
      before resolve (per `answer-code-review` skill).

---

## 12. Out-of-scope follow-ups (separate PRs)

- Porting the same `BookStatsView` for XTC and TXT readers (no current
  stats record for those formats — bigger feature).
- "Daily Avg" derivation requires the global reading history; once a
  `dailyAvgSeconds` field is added to `GlobalReadingStats` (separate design
  doc), the cell renders trivially.
- The progress line on the home card is invisible in dark mode if the
  theme renders inverted cards — a follow-up can adjust `inverted` arg
  per-theme.